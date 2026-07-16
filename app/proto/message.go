package proto

import (
	"encoding/binary"
	"errors"
	"math"
)

// Opcodes (body[0]). Requests PC→MCU, responses MCU→PC. See protocol.h / serial-protocol.md.
const (
	CmdSet     = 0x01 // [id:u16][val:f32]
	CmdGet     = 0x02 // [id:u16]
	CmdList    = 0x03 // (none)
	CmdNoteOn  = 0x04 // [note:u8][vel:u8]
	CmdNoteOff = 0x05 // [note:u8]
	CmdStat    = 0x06 // (none)

	RspAck     = 0x80 // (none) — reply to NOTE_ON/OFF
	RspValue   = 0x81 // [id:u16][val:f32] — reply to GET and SET (post-clamp)
	RspParam   = 0x82 // [id:u16][type:u8][min,max,def,cur:f32][namelen:u8][name]
	RspListEnd = 0x83 // [count:u16]
	RspStat    = 0x86 // [heap,minheap,uptime_ms,cpu_permille,underruns:u32]
	RspErr     = 0xFF // [code:u8]
)

// Param types (PARAM.type). Tells the GUI which control to draw and how to interpret the float.
const (
	TypeFloat = 0
	TypeInt   = 1
	TypeEnum  = 2
	TypeBool  = 3
)

// Error codes (RSP_ERR body).
const (
	ErrUnknownCmd = 1
	ErrBadID      = 2
	ErrBadLen     = 3
)

var errShort = errors.New("proto: response body too short")

// --- request frame builders ---

func SetFrame(id uint16, val float32) []byte {
	b := make([]byte, 7)
	b[0] = CmdSet
	binary.LittleEndian.PutUint16(b[1:], id)
	binary.LittleEndian.PutUint32(b[3:], math.Float32bits(val))
	return EncodeFrame(b)
}

func GetFrame(id uint16) []byte {
	b := make([]byte, 3)
	b[0] = CmdGet
	binary.LittleEndian.PutUint16(b[1:], id)
	return EncodeFrame(b)
}

func ListFrame() []byte { return EncodeFrame([]byte{CmdList}) }

func StatFrame() []byte { return EncodeFrame([]byte{CmdStat}) }

func NoteOnFrame(note, vel uint8) []byte  { return EncodeFrame([]byte{CmdNoteOn, note, vel}) }
func NoteOffFrame(note uint8) []byte      { return EncodeFrame([]byte{CmdNoteOff, note}) }

// --- response types ---

type Value struct {
	ID  uint16
	Val float32
}

type Param struct {
	ID              uint16
	Type            uint8
	Min, Max, Def, Cur float32
	Name            string
}

type ListEnd struct{ Count uint16 }

type Stat struct {
	Heap, MinHeap, UptimeMS, CPUPermille, Underruns uint32
}

type Err struct{ Code uint8 }

// Opcode returns body[0], or 0 for an empty body.
func Opcode(body []byte) uint8 {
	if len(body) == 0 {
		return 0
	}
	return body[0]
}

func ParseValue(body []byte) (Value, error) {
	if len(body) < 7 {
		return Value{}, errShort
	}
	return Value{
		ID:  binary.LittleEndian.Uint16(body[1:]),
		Val: math.Float32frombits(binary.LittleEndian.Uint32(body[3:])),
	}, nil
}

func ParseParam(body []byte) (Param, error) {
	if len(body) < 21 {
		return Param{}, errShort
	}
	nl := int(body[20])
	if len(body) < 21+nl {
		return Param{}, errShort
	}
	return Param{
		ID:   binary.LittleEndian.Uint16(body[1:]),
		Type: body[3],
		Min:  math.Float32frombits(binary.LittleEndian.Uint32(body[4:])),
		Max:  math.Float32frombits(binary.LittleEndian.Uint32(body[8:])),
		Def:  math.Float32frombits(binary.LittleEndian.Uint32(body[12:])),
		Cur:  math.Float32frombits(binary.LittleEndian.Uint32(body[16:])),
		Name: string(body[21 : 21+nl]),
	}, nil
}

func ParseListEnd(body []byte) (ListEnd, error) {
	if len(body) < 3 {
		return ListEnd{}, errShort
	}
	return ListEnd{Count: binary.LittleEndian.Uint16(body[1:])}, nil
}

func ParseStat(body []byte) (Stat, error) {
	if len(body) < 21 {
		return Stat{}, errShort
	}
	u := func(off int) uint32 { return binary.LittleEndian.Uint32(body[off:]) }
	return Stat{Heap: u(1), MinHeap: u(5), UptimeMS: u(9), CPUPermille: u(13), Underruns: u(17)}, nil
}

func ParseErr(body []byte) (Err, error) {
	if len(body) < 2 {
		return Err{}, errShort
	}
	return Err{Code: body[1]}, nil
}

// --- response frame builders (device is a client and never sends these; used by the fake
//     firmware for headless integration tests, and by round-trip tests) ---

func ValueRespFrame(id uint16, val float32) []byte {
	b := make([]byte, 7)
	b[0] = RspValue
	binary.LittleEndian.PutUint16(b[1:], id)
	binary.LittleEndian.PutUint32(b[3:], math.Float32bits(val))
	return EncodeFrame(b)
}

func ParamRespFrame(p Param) []byte {
	name := p.Name
	if len(name) > 64 {
		name = name[:64]
	}
	b := make([]byte, 21, 21+len(name))
	b[0] = RspParam
	binary.LittleEndian.PutUint16(b[1:], p.ID)
	b[3] = p.Type
	binary.LittleEndian.PutUint32(b[4:], math.Float32bits(p.Min))
	binary.LittleEndian.PutUint32(b[8:], math.Float32bits(p.Max))
	binary.LittleEndian.PutUint32(b[12:], math.Float32bits(p.Def))
	binary.LittleEndian.PutUint32(b[16:], math.Float32bits(p.Cur))
	b[20] = byte(len(name))
	b = append(b, name...)
	return EncodeFrame(b)
}

func ListEndFrame(count uint16) []byte {
	b := make([]byte, 3)
	b[0] = RspListEnd
	binary.LittleEndian.PutUint16(b[1:], count)
	return EncodeFrame(b)
}

func StatRespFrame(s Stat) []byte {
	b := make([]byte, 21)
	b[0] = RspStat
	for i, v := range []uint32{s.Heap, s.MinHeap, s.UptimeMS, s.CPUPermille, s.Underruns} {
		binary.LittleEndian.PutUint32(b[1+i*4:], v)
	}
	return EncodeFrame(b)
}

func AckFrame() []byte         { return EncodeFrame([]byte{RspAck}) }
func ErrRespFrame(code uint8) []byte { return EncodeFrame([]byte{RspErr, code}) }
