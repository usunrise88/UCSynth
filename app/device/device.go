// Package device is the client-side model of the synth: it builds a param registry from LIST,
// serializes all frame writes through one priority-aware writer (notes jump ahead of SET bursts
// and STAT polls), coalesces SETs, applies VALUE/STAT echoes, and exposes an immutable Snapshot
// for the UI. It depends only on an io.ReadWriteCloser, so tests drive it against a fake firmware
// over net.Pipe — no serial port, no hardware, no GUI. Pure logic; no Gio import.
package device

import (
	"io"
	"sync"
	"time"

	"ucsynth/app/proto"
)

type State int

const (
	Connecting State = iota
	Synced
	Disconnected
	Errored
)

func (s State) String() string {
	switch s {
	case Connecting:
		return "подключение…"
	case Synced:
		return "подключено"
	case Disconnected:
		return "отключено"
	case Errored:
		return "ошибка"
	}
	return "?"
}

// Snapshot is an immutable view of the model for one UI frame.
type Snapshot struct {
	State  State
	Err    error
	Params []proto.Param // discovery (id) order
	Stat   proto.Stat
	// Missing is how many PARAM frames LISTEND promised but never arrived (0 = registry complete).
	// Non-zero means the rack is silently short — worth telling the user rather than looking like
	// the firmware simply does not have those params.
	Missing int
	// LastErr is the most recent RSP_ERR code from the firmware, 0 if none. Without this the
	// firmware's only way of saying "I rejected that" goes straight to the floor.
	LastErr uint8
	// Stale is set when nothing has been decoded for staleAfter while the port is still open: the
	// board stopped answering (watchdog, wedged I2C) but the connection looks perfectly fine.
	Stale bool
	// Presets is the device's stored preset directory from the last completed PRESET_LIST. The tree
	// is derived from each entry's Path ("Folder/Name"). Empty until PresetList() completes.
	Presets []proto.Preset
	// Patterns is the device's stored pattern directory (from SeqList). SeqPattern is the current live
	// 16-step pattern (from SeqGet). Both empty/default until the corresponding request completes.
	Patterns   []proto.SeqEntry
	SeqPattern [proto.SeqSteps]proto.SeqStep
	// SeqRev bumps on every RSP_SEQ_STEP the device sends. The firmware never pushes step edits, so
	// SeqPattern (and thus SeqRev) only moves when the client asks (SeqGet, or SeqLoad→SeqGet). The
	// editor watches this to reseed its working copy exactly when a fresh dump lands, and never while
	// the user is editing — otherwise a stale snapshot would fight local edits.
	SeqRev uint32
}

// Param returns the param with the given id and whether it was found.
func (s Snapshot) Param(id uint16) (proto.Param, bool) {
	for _, p := range s.Params {
		if p.ID == id {
			return p, true
		}
	}
	return proto.Param{}, false
}

const (
	setFlushHz  = 40                    // SET coalesce flush rate
	statPollMs  = 500                   // STAT poll cadence (~2 Hz)
	closeGrace  = 30 * time.Millisecond // let the writer flush NOTE_OFFs before teardown
	noteOffWait = 50 * time.Millisecond // how long a NOTE_OFF waits for queue room (see NoteOff)
	closeWait   = 1 * time.Second       // cap on waiting for the goroutines in Close

)

// Timing knobs for registry sync and liveness. Variables rather than constants so tests can shorten
// them; nothing outside tests should write these.
var (
	// LIST is not idempotent-by-luck: logs share this wire, so a frame lost to CRC is expected
	// (tech-debt T-003). Without a retry the model sits in Connecting forever with the board alive.
	listTimeout = 1500 * time.Millisecond
	listTries   = 3
	// staleAfter — no decoded frame for this long while Synced means the board went quiet. STAT is
	// polled at 2 Hz, so four missed replies is unambiguous.
	staleAfter = 4 * statPollMs * time.Millisecond
)

type Device struct {
	conn     io.ReadWriteCloser
	dec      *proto.Decoder
	onChange func()

	mu        sync.RWMutex
	order     []uint16
	params    map[uint16]proto.Param
	stat      proto.Stat
	state     State
	err       error
	held      map[uint8]bool
	listCount int       // params LISTEND says exist (-1 until LISTEND arrives)
	missing   int       // shortfall accepted after listTries attempts
	lastErr   uint8     // most recent RSP_ERR code
	lastRx    time.Time // when the last frame decoded — liveness

	presets     []proto.Preset // last completed PRESET_LIST result
	presetBuild []proto.Preset // accumulating current PRESET_LIST (committed on PRESET_END)

	patterns     []proto.SeqEntry              // last completed SEQ_LIST result
	patternBuild []proto.SeqEntry              // accumulating current SEQ_LIST
	seqPattern   [proto.SeqSteps]proto.SeqStep // current live pattern (from SEQ_GET / RSP_SEQ_STEP)
	seqRev       uint32                        // bumps on each RSP_SEQ_STEP — reseed signal for the editor

	noteCh  chan []byte
	frameCh chan []byte

	pendMu  sync.Mutex
	pending map[uint16]float32

	done      chan struct{}
	closeOnce sync.Once
	wg        sync.WaitGroup
}

// New wraps a byte stream. onChange (may be nil) is called whenever the model changes — the UI
// sets it to window.Invalidate. Call Start to begin.
func New(conn io.ReadWriteCloser, onChange func()) *Device {
	if onChange == nil {
		onChange = func() {}
	}
	return &Device{
		conn:      conn,
		dec:       proto.NewDecoder(),
		onChange:  onChange,
		params:    map[uint16]proto.Param{},
		held:      map[uint8]bool{},
		noteCh:    make(chan []byte, 32),
		frameCh:   make(chan []byte, 64),
		pending:   map[uint16]float32{},
		done:      make(chan struct{}),
		state:     Connecting,
		listCount: -1,
		lastRx:    time.Now(),
	}
}

// Start launches the reader/writer/syncer goroutines and requests the registry (LIST).
func (d *Device) Start() {
	d.wg.Add(3)
	go d.reader()
	go d.writer()
	go d.syncer()
	d.enqueue(d.frameCh, proto.ListFrame())
}

// syncer re-asks for the registry until it is complete. It exists because LISTEND (or any single
// PARAM) can be lost to a log line colliding with the frame, and nothing else would ever notice:
// the model would stay in Connecting, or come up with a silently short rack. After listTries it goes
// live with whatever arrived and records the shortfall in Missing — a partial panel with a visible
// warning beats a permanent "подключение…".
func (d *Device) syncer() {
	defer d.wg.Done()
	for try := 1; ; try++ {
		select {
		case <-d.done:
			return
		case <-time.After(listTimeout):
		}

		d.mu.Lock()
		if d.state != Connecting {
			d.mu.Unlock()
			return
		}
		if try >= listTries {
			d.state = Synced
			if d.listCount > len(d.order) {
				d.missing = d.listCount - len(d.order)
			}
			d.mu.Unlock()
			d.onChange()
			return
		}
		d.mu.Unlock()
		d.enqueue(d.frameCh, proto.ListFrame())
	}
}

// --- public control ---

// SetParam queues a value change (coalesced latest-per-id, flushed at setFlushHz).
func (d *Device) SetParam(id uint16, val float32) {
	d.pendMu.Lock()
	d.pending[id] = val
	d.pendMu.Unlock()
}

// Refresh re-reads one param's value (GET) — for external-change reflection (no push in v1).
func (d *Device) Refresh(id uint16) { d.enqueue(d.frameCh, proto.GetFrame(id)) }

// --- presets (stage 6) ---

// requestPresetList clears the accumulator and asks for the device's preset directory. The result
// lands in Snapshot.Presets once PRESET_END arrives.
func (d *Device) requestPresetList() {
	d.mu.Lock()
	d.presetBuild = nil
	d.mu.Unlock()
	d.enqueue(d.frameCh, proto.PresetListFrame())
}

// PresetList requests the device's stored preset directory.
func (d *Device) PresetList() { d.requestPresetList() }

// PresetSave snapshots the device's current registry into slot (proto.PresetSlotNew = new) at path,
// then refreshes the directory so the tree shows the result.
func (d *Device) PresetSave(slot uint16, path string) {
	d.enqueue(d.frameCh, proto.PresetSaveFrame(slot, path))
	d.requestPresetList()
}

// PresetLoad applies a stored preset to the device, then re-LISTs params so cached cur values refresh.
func (d *Device) PresetLoad(slot uint16) {
	d.enqueue(d.frameCh, proto.PresetLoadFrame(slot))
	d.enqueue(d.frameCh, proto.ListFrame())
}

// PresetDelete removes a slot, then refreshes the directory.
func (d *Device) PresetDelete(slot uint16) {
	d.enqueue(d.frameCh, proto.PresetDeleteFrame(slot))
	d.requestPresetList()
}

// PresetRename changes a slot's tree path (values untouched), then refreshes the directory.
func (d *Device) PresetRename(slot uint16, path string) {
	d.enqueue(d.frameCh, proto.PresetRenameFrame(slot, path))
	d.requestPresetList()
}

// --- sequencer patterns (stage 7) ---

// SeqSetStep uploads one edited step to the device's live pattern.
func (d *Device) SeqSetStep(step uint8, st proto.SeqStep) {
	d.enqueue(d.frameCh, proto.SeqSetStepFrame(step, st))
}

// SeqGet requests the device's live pattern (16 RSP_SEQ_STEP → Snapshot.SeqPattern).
func (d *Device) SeqGet() { d.enqueue(d.frameCh, proto.SeqGetFrame()) }

func (d *Device) requestSeqList() {
	d.mu.Lock()
	d.patternBuild = nil
	d.mu.Unlock()
	d.enqueue(d.frameCh, proto.SeqListFrame())
}

// SeqList requests the device's stored pattern directory.
func (d *Device) SeqList() { d.requestSeqList() }

// SeqSave persists the live pattern to a NVS slot (proto.PresetSlotNew = new), then refreshes the list.
func (d *Device) SeqSave(slot uint16, path string) {
	d.enqueue(d.frameCh, proto.SeqSaveFrame(slot, path))
	d.requestSeqList()
}

// SeqLoad applies a stored pattern to the live one, then re-GETs it so Snapshot.SeqPattern refreshes.
func (d *Device) SeqLoad(slot uint16) {
	d.enqueue(d.frameCh, proto.SeqLoadFrame(slot))
	d.enqueue(d.frameCh, proto.SeqGetFrame())
}

func (d *Device) SeqDelete(slot uint16) {
	d.enqueue(d.frameCh, proto.SeqDeleteFrame(slot))
	d.requestSeqList()
}

func (d *Device) SeqRename(slot uint16, path string) {
	d.enqueue(d.frameCh, proto.SeqRenameFrame(slot, path))
	d.requestSeqList()
}

func (d *Device) NoteOn(note, vel uint8) {
	d.mu.Lock()
	d.held[note] = true
	d.mu.Unlock()
	d.enqueue(d.noteCh, proto.NoteOnFrame(note, vel))
}

// NoteOff releases a note. A dropped NOTE_ON is merely inaudible, but a dropped NOTE_OFF is a
// permanent drone — the firmware holds the gate open and nothing else releases it. So this waits for
// room in the queue, and forgets the note only once the frame is actually queued: clearing d.held
// first would lose the note twice, since AllNotesOff (the Panic button) walks d.held to recover.
func (d *Device) NoteOff(note uint8) { d.noteOffBy(note, time.Now().Add(noteOffWait)) }

func (d *Device) noteOffBy(note uint8, deadline time.Time) bool {
	if !d.enqueueBy(d.noteCh, proto.NoteOffFrame(note), deadline) {
		return false // still held — Panic can retry
	}
	d.mu.Lock()
	delete(d.held, note)
	d.mu.Unlock()
	return true
}

// AllNotesOff sends NOTE_OFF for every currently-held note (panic; no all-off opcode exists).
// One deadline covers the whole batch, so a stalled writer can't freeze the caller per-note.
func (d *Device) AllNotesOff() {
	d.mu.RLock()
	notes := make([]uint8, 0, len(d.held))
	for n := range d.held {
		notes = append(notes, n)
	}
	d.mu.RUnlock()
	deadline := time.Now().Add(noteOffWait)
	for _, n := range notes {
		d.noteOffBy(n, deadline)
	}
}

// Snapshot returns an immutable view for the current UI frame.
func (d *Device) Snapshot() Snapshot {
	d.mu.RLock()
	defer d.mu.RUnlock()
	ps := make([]proto.Param, 0, len(d.order))
	for _, id := range d.order {
		ps = append(ps, d.params[id])
	}
	prs := make([]proto.Preset, len(d.presets))
	copy(prs, d.presets)
	pats := make([]proto.SeqEntry, len(d.patterns))
	copy(pats, d.patterns)
	return Snapshot{
		State:      d.state,
		Err:        d.err,
		Params:     ps,
		Stat:       d.stat,
		Missing:    d.missing,
		LastErr:    d.lastErr,
		Stale:      d.state == Synced && time.Since(d.lastRx) > staleAfter,
		Presets:    prs,
		Patterns:   pats,
		SeqPattern: d.seqPattern,
		SeqRev:     d.seqRev,
	}
}

// Close flushes held notes, stops the goroutines and closes the connection. It waits for the
// goroutines but only up to closeWait: a serial handle with an outstanding overlapped read does not
// always return from Read when closed, and hanging forever here would freeze the caller. Callers on
// a UI thread should still not call this inline — see ui.disconnect.
func (d *Device) Close() error {
	d.AllNotesOff()
	time.Sleep(closeGrace) // give the priority writer a moment to emit the NOTE_OFFs
	d.stop(Disconnected, nil)
	err := d.conn.Close() // unblocks the reader's Read

	done := make(chan struct{})
	go func() { d.wg.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(closeWait):
		// Goroutines are parked in a Read that never returned. They hold only the dead connection,
		// so leaking them is strictly better than never returning from Close.
	}
	return err
}

// --- internals ---

func (d *Device) reader() {
	defer d.wg.Done()
	buf := make([]byte, 1024)
	for {
		n, err := d.conn.Read(buf)
		if n > 0 {
			changed := false
			for _, body := range d.dec.Push(buf[:n]) {
				d.mu.Lock()
				d.lastRx = time.Now() // a decoded frame is proof the board is answering
				d.mu.Unlock()
				if d.handle(body) {
					changed = true
				}
			}
			if changed {
				d.onChange()
			}
		}
		if err != nil {
			d.stop(Errored, err)
			return
		}
	}
}

func (d *Device) handle(body []byte) bool {
	switch proto.Opcode(body) {
	case proto.RspParam:
		p, err := proto.ParseParam(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		if _, ok := d.params[p.ID]; !ok {
			d.order = append(d.order, p.ID)
		}
		d.params[p.ID] = p
		d.mu.Unlock()
		return true
	case proto.RspListEnd:
		le, err := proto.ParseListEnd(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		// The count field exists precisely so a dropped PARAM is detectable. Staying in Connecting
		// on a shortfall lets syncer re-ask; accepting it blindly is how a knob goes missing with no
		// diagnostic at all.
		d.listCount = int(le.Count)
		if len(d.order) >= d.listCount {
			d.state = Synced
			d.missing = 0
		}
		d.mu.Unlock()
		return true
	case proto.RspValue:
		v, err := proto.ParseValue(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		if p, ok := d.params[v.ID]; ok {
			p.Cur = v.Val
			d.params[v.ID] = p
		}
		d.mu.Unlock()
		return true
	case proto.RspStat:
		s, err := proto.ParseStat(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		d.stat = s
		d.mu.Unlock()
		return true
	case proto.RspErr:
		e, err := proto.ParseErr(body)
		if err != nil {
			return false
		}
		// No request/response correlation exists, so we cannot say *which* request was rejected —
		// but surfacing the code still beats the previous behaviour of dropping it silently while
		// the UI kept rendering the locally-computed value as if the change had taken effect.
		d.mu.Lock()
		d.lastErr = e.Code
		d.mu.Unlock()
		return true
	case proto.RspPreset:
		p, err := proto.ParsePreset(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		d.presetBuild = append(d.presetBuild, p)
		d.mu.Unlock()
		return false // accumulate quietly; the UI refreshes on PRESET_END
	case proto.RspPresetEnd:
		d.mu.Lock()
		d.presets = append([]proto.Preset(nil), d.presetBuild...)
		d.presetBuild = nil
		d.mu.Unlock()
		return true
	case proto.RspPresetSaved:
		// The assigned slot is not needed by the UI: after a save we re-LIST, and the new preset
		// appears in the tree by its path. Just let the change propagate.
		return true
	case proto.RspSeqStep:
		step, st, err := proto.ParseSeqStep(body)
		if err != nil || int(step) >= proto.SeqSteps {
			return false
		}
		d.mu.Lock()
		d.seqPattern[step] = st
		d.seqRev++
		d.mu.Unlock()
		return true
	case proto.RspSeqEntry:
		e, err := proto.ParseSeqEntry(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		d.patternBuild = append(d.patternBuild, e)
		d.mu.Unlock()
		return false // accumulate quietly; the UI refreshes on SEQ_END
	case proto.RspSeqEnd:
		d.mu.Lock()
		d.patterns = append([]proto.SeqEntry(nil), d.patternBuild...)
		d.patternBuild = nil
		d.mu.Unlock()
		return true
	case proto.RspSeqSaved:
		return true // after save we re-LIST; the pattern appears by path
	default:
		return false // ACK — nothing to record
	}
}

func (d *Device) writer() {
	defer d.wg.Done()
	setFlush := time.NewTicker(time.Second / setFlushHz)
	statPoll := time.NewTicker(statPollMs * time.Millisecond)
	defer setFlush.Stop()
	defer statPoll.Stop()

	for {
		// Priority: drain notes ahead of everything else.
		select {
		case <-d.done:
			d.drainNotes()
			return
		case f := <-d.noteCh:
			d.write(f)
		default:
			select {
			case <-d.done:
				d.drainNotes()
				return
			case f := <-d.noteCh:
				d.write(f)
			case f := <-d.frameCh:
				d.write(f)
			case <-setFlush.C:
				d.flushSets()
			case <-statPoll.C:
				d.write(proto.StatFrame())
			}
		}
	}
}

// drainNotes emits any queued NOTE_OFFs at shutdown (best effort; ignore write errors).
func (d *Device) drainNotes() {
	for {
		select {
		case f := <-d.noteCh:
			_, _ = d.conn.Write(f)
		default:
			return
		}
	}
}

func (d *Device) flushSets() {
	d.pendMu.Lock()
	if len(d.pending) == 0 {
		d.pendMu.Unlock()
		return
	}
	frames := make([][]byte, 0, len(d.pending))
	for id, v := range d.pending {
		frames = append(frames, proto.SetFrame(id, v))
	}
	d.pending = map[uint16]float32{}
	d.pendMu.Unlock()
	for _, f := range frames {
		d.write(f)
	}
}

func (d *Device) write(f []byte) {
	if _, err := d.conn.Write(f); err != nil {
		d.stop(Errored, err)
	}
}

// enqueue is best-effort: drop when full. Only for frames the model can re-derive — SETs go via the
// pending map, and LIST/GET/STAT are re-requested or repeated on a ticker.
func (d *Device) enqueue(ch chan []byte, f []byte) bool {
	select {
	case ch <- f:
		return true
	case <-d.done:
		return false
	default:
		return false
	}
}

// enqueueBy is enqueue with a deadline, for frames whose loss is unrecoverable (NOTE_OFF).
func (d *Device) enqueueBy(ch chan []byte, f []byte, deadline time.Time) bool {
	select { // fast path: room available, or already shutting down
	case ch <- f:
		return true
	case <-d.done:
		return false
	default:
	}
	t := time.NewTimer(time.Until(deadline))
	defer t.Stop()
	select {
	case ch <- f:
		return true
	case <-d.done:
		return false
	case <-t.C:
		return false
	}
}

func (d *Device) stop(state State, err error) {
	d.closeOnce.Do(func() {
		d.mu.Lock()
		d.state = state
		if err != nil {
			d.err = err
		}
		d.mu.Unlock()
		close(d.done)
	})
	d.onChange()
}
