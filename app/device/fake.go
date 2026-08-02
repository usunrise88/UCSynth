package device

import (
	"encoding/binary"
	"io"
	"math"
	"sort"
	"sync"

	"ucsynth/app/proto"
)

// Fake is an in-memory firmware simulator speaking the protocol over an io.ReadWriteCloser (the
// far end of a net.Pipe in tests). It mirrors the C `control` registry semantics (clamp + quantize
// on SET) so the device package can be exercised end-to-end with no serial port and no hardware.
// Could also back an offline "demo mode" later.
type Fake struct {
	conn   io.ReadWriteCloser
	dec    *proto.Decoder
	mu     sync.Mutex
	order  []uint16
	params map[uint16]proto.Param
	stat   proto.Stat

	dropParams int  // omit this many PARAM frames from the next LIST (simulated CRC loss)
	dropSticky bool // keep dropping on every LIST instead of just the next one
	mute       bool // stop answering entirely (simulated wedged firmware)

	presets    map[uint16]fakePreset // in-memory "NVS": slot → {path, values}
	presetNext uint16                // monotonic slot counter (mirrors preset_store next_id)

	seqPattern [proto.SeqSteps]proto.SeqStep // live pattern
	seqStore   map[uint16]fakeSeqRec         // in-memory pattern "NVS"
	seqNext    uint16
}

// fakePreset mirrors a stored preset blob: a tree path + a registry snapshot (id→value).
type fakePreset struct {
	path   string
	values map[uint16]float32
}

// fakeSeqRec mirrors a stored pattern: a tree path + the 16-step pattern.
type fakeSeqRec struct {
	path string
	pat  [proto.SeqSteps]proto.SeqStep
}

func NewFake(conn io.ReadWriteCloser, params []proto.Param, stat proto.Stat) *Fake {
	f := &Fake{conn: conn, dec: proto.NewDecoder(), params: map[uint16]proto.Param{}, stat: stat,
		presets: map[uint16]fakePreset{}, seqStore: map[uint16]fakeSeqRec{}}
	for _, p := range params {
		f.order = append(f.order, p.ID)
		f.params[p.ID] = p
	}
	return f
}

// Run reads requests and writes responses until the connection closes. Blocks — run in a goroutine.
func (f *Fake) Run() {
	buf := make([]byte, 1024)
	for {
		n, err := f.conn.Read(buf)
		if n > 0 {
			for _, body := range f.dec.Push(buf[:n]) {
				f.respond(body)
			}
		}
		if err != nil {
			return
		}
	}
}

// DropNextParams makes the next LIST omit n PARAM frames while still reporting the true count in
// LISTEND — what a log line colliding with a PARAM frame looks like from the client's side. With
// sticky set, every LIST drops them, so the client's retries can be exercised to exhaustion.
func (f *Fake) DropNextParams(n int, sticky bool) {
	f.mu.Lock()
	f.dropParams, f.dropSticky = n, sticky
	f.mu.Unlock()
}

// Mute makes the fake stop answering, simulating firmware that wedged with the port still open.
func (f *Fake) Mute() { f.mu.Lock(); f.mute = true; f.mu.Unlock() }

func (f *Fake) muted() bool { f.mu.Lock(); defer f.mu.Unlock(); return f.mute }

func (f *Fake) respond(body []byte) {
	if f.muted() {
		return
	}
	switch proto.Opcode(body) {
	case proto.CmdList:
		f.mu.Lock()
		drop := f.dropParams
		if !f.dropSticky {
			f.dropParams = 0
		}
		for i, id := range f.order {
			if i < drop {
				continue // frame lost on the wire
			}
			f.write(proto.ParamRespFrame(f.params[id]))
		}
		count := uint16(len(f.order))
		f.mu.Unlock()
		f.write(proto.ListEndFrame(count))
	case proto.CmdGet:
		if len(body) < 3 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		id := binary.LittleEndian.Uint16(body[1:])
		f.mu.Lock()
		p, ok := f.params[id]
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadID))
			return
		}
		f.write(proto.ValueRespFrame(id, p.Cur))
	case proto.CmdSet:
		if len(body) < 7 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		id := binary.LittleEndian.Uint16(body[1:])
		val := math.Float32frombits(binary.LittleEndian.Uint32(body[3:]))
		f.mu.Lock()
		p, ok := f.params[id]
		if ok {
			p.Cur = clampQuant(p, val)
			f.params[id] = p
		}
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadID))
			return
		}
		f.write(proto.ValueRespFrame(id, p.Cur))
	case proto.CmdNoteOn, proto.CmdNoteOff:
		f.write(proto.AckFrame())
	case proto.CmdStat:
		f.mu.Lock()
		s := f.stat
		f.mu.Unlock()
		f.write(proto.StatRespFrame(s))
	case proto.CmdPresetSave:
		slot, path, ok := parseSlotPath(body)
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		f.mu.Lock()
		use := slot
		if slot == proto.PresetSlotNew {
			use = f.presetNext
			f.presetNext++
		}
		vals := map[uint16]float32{} // snapshot current registry
		for id, p := range f.params {
			vals[id] = p.Cur
		}
		f.presets[use] = fakePreset{path: path, values: vals}
		f.mu.Unlock()
		f.write(proto.PresetSavedFrame(use))
	case proto.CmdPresetLoad:
		if len(body) < 3 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		slot := binary.LittleEndian.Uint16(body[1:])
		f.mu.Lock()
		pr, ok := f.presets[slot]
		if ok {
			for id, p := range f.params { // reset to default, then apply stored values
				p.Cur = p.Def
				f.params[id] = p
			}
			for id, v := range pr.values {
				if p, o := f.params[id]; o {
					p.Cur = v
					f.params[id] = p
				}
			}
		}
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdPresetDelete:
		if len(body) < 3 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		slot := binary.LittleEndian.Uint16(body[1:])
		f.mu.Lock()
		_, ok := f.presets[slot]
		delete(f.presets, slot)
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdPresetRename:
		slot, path, ok := parseSlotPath(body)
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		f.mu.Lock()
		pr, found := f.presets[slot]
		if found {
			pr.path = path
			f.presets[slot] = pr
		}
		f.mu.Unlock()
		if !found {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdPresetList:
		f.mu.Lock()
		slots := make([]uint16, 0, len(f.presets))
		for s := range f.presets {
			slots = append(slots, s)
		}
		sort.Slice(slots, func(i, j int) bool { return slots[i] < slots[j] })
		frames := make([][]byte, 0, len(slots)+1)
		for _, s := range slots {
			frames = append(frames, proto.PresetRespFrame(s, f.presets[s].path))
		}
		count := uint16(len(slots))
		f.mu.Unlock()
		for _, fr := range frames {
			f.write(fr)
		}
		f.write(proto.PresetEndFrame(count))
	case proto.CmdSeqSetStep:
		if len(body) < 2 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		st, _, err := proto.DecodeStep(body[2:])
		if err != nil {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		f.mu.Lock()
		if int(body[1]) < proto.SeqSteps {
			f.seqPattern[body[1]] = st
		}
		f.mu.Unlock()
		f.write(proto.AckFrame())
	case proto.CmdSeqGet:
		f.mu.Lock()
		pat := f.seqPattern
		f.mu.Unlock()
		for s := 0; s < proto.SeqSteps; s++ {
			f.write(proto.SeqStepRespFrame(uint8(s), pat[s]))
		}
	case proto.CmdSeqSave:
		slot, path, ok := parseSlotPath(body)
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		f.mu.Lock()
		use := slot
		if slot == 0xFFFF {
			use = f.seqNext
			f.seqNext++
		}
		f.seqStore[use] = fakeSeqRec{path: path, pat: f.seqPattern}
		f.mu.Unlock()
		f.write(proto.SeqSavedFrame(use))
	case proto.CmdSeqLoad:
		if len(body) < 3 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		slot := binary.LittleEndian.Uint16(body[1:])
		f.mu.Lock()
		rec, ok := f.seqStore[slot]
		if ok {
			f.seqPattern = rec.pat
		}
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdSeqDelete:
		if len(body) < 3 {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		slot := binary.LittleEndian.Uint16(body[1:])
		f.mu.Lock()
		_, ok := f.seqStore[slot]
		delete(f.seqStore, slot)
		f.mu.Unlock()
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdSeqRename:
		slot, path, ok := parseSlotPath(body)
		if !ok {
			f.write(proto.ErrRespFrame(proto.ErrBadLen))
			return
		}
		f.mu.Lock()
		rec, found := f.seqStore[slot]
		if found {
			rec.path = path
			f.seqStore[slot] = rec
		}
		f.mu.Unlock()
		if !found {
			f.write(proto.ErrRespFrame(proto.ErrNoPreset))
			return
		}
		f.write(proto.AckFrame())
	case proto.CmdSeqList:
		f.mu.Lock()
		sslots := make([]uint16, 0, len(f.seqStore))
		for s := range f.seqStore {
			sslots = append(sslots, s)
		}
		sort.Slice(sslots, func(i, j int) bool { return sslots[i] < sslots[j] })
		sframes := make([][]byte, 0, len(sslots)+1)
		for _, s := range sslots {
			sframes = append(sframes, proto.SeqEntryFrame(s, f.seqStore[s].path))
		}
		scount := uint16(len(sslots))
		f.mu.Unlock()
		for _, fr := range sframes {
			f.write(fr)
		}
		f.write(proto.SeqEndFrame(scount))
	default:
		f.write(proto.ErrRespFrame(proto.ErrUnknownCmd))
	}
}

// parseSlotPath decodes [slot:u16][path_len:u8][path] from a request body.
func parseSlotPath(body []byte) (uint16, string, bool) {
	if len(body) < 4 {
		return 0, "", false
	}
	slot := binary.LittleEndian.Uint16(body[1:])
	pl := int(body[3])
	if len(body) < 4+pl {
		return 0, "", false
	}
	return slot, string(body[4 : 4+pl]), true
}

func (f *Fake) write(frame []byte) { _, _ = f.conn.Write(frame) }

// clampQuant mirrors control.cpp clamp_and_quantize: clamp to [min,max], round non-float types.
// The comparisons are inverted so NaN lands on min instead of sailing through — see the C version
// for why that matters. Keeping the Fake faithful to the firmware's *bugs* would make every
// integration test in this package structurally unable to catch them.
func clampQuant(p proto.Param, v float32) float32 {
	if !(v >= p.Min) { // false for NaN and for v < min
		v = p.Min
	}
	if !(v <= p.Max) { // false for +Inf and for v > max
		v = p.Max
	}
	if p.Type != proto.TypeFloat {
		v = float32(math.Round(float64(v)))
	}
	return v
}
