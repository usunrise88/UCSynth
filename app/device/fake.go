package device

import (
	"encoding/binary"
	"io"
	"math"
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
}

func NewFake(conn io.ReadWriteCloser, params []proto.Param, stat proto.Stat) *Fake {
	f := &Fake{conn: conn, dec: proto.NewDecoder(), params: map[uint16]proto.Param{}, stat: stat}
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

func (f *Fake) respond(body []byte) {
	switch proto.Opcode(body) {
	case proto.CmdList:
		f.mu.Lock()
		for _, id := range f.order {
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
	default:
		f.write(proto.ErrRespFrame(proto.ErrUnknownCmd))
	}
}

func (f *Fake) write(frame []byte) { _, _ = f.conn.Write(frame) }

// clampQuant mirrors control.cpp clamp_and_quantize: clamp to [min,max], round non-float types.
func clampQuant(p proto.Param, v float32) float32 {
	if v < p.Min {
		v = p.Min
	}
	if v > p.Max {
		v = p.Max
	}
	if p.Type != proto.TypeFloat {
		v = float32(math.Round(float64(v)))
	}
	return v
}
