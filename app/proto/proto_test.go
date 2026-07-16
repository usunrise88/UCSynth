package proto

import (
	"bytes"
	"encoding/binary"
	"math"
	"testing"
)

// CRC parity with firmware (test_protocol.cpp asserts the same check value).
func TestCRCCheckValue(t *testing.T) {
	if got := CRC16([]byte("123456789")); got != 0x29B1 {
		t.Fatalf("CRC-16/CCITT-FALSE check value = 0x%04X, want 0x29B1", got)
	}
}

// Golden frames from docs/serial-protocol.md.
func TestEncodeGoldenFrames(t *testing.T) {
	cases := []struct {
		name string
		got  []byte
		// prefix = everything except the trailing 2 CRC bytes (which we recompute+verify)
		prefix []byte
	}{
		{"LIST", ListFrame(), []byte{0x55, 0xAA, 0x01, 0x03}},
		{"GET id0", GetFrame(0), []byte{0x55, 0xAA, 0x03, 0x02, 0x00, 0x00}},
		{"SET id0=0.5", SetFrame(0, 0.5), []byte{0x55, 0xAA, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F}},
		{"STAT", StatFrame(), []byte{0x55, 0xAA, 0x01, 0x06}},
	}
	for _, c := range cases {
		if len(c.got) != len(c.prefix)+2 {
			t.Errorf("%s: frame len %d, want %d", c.name, len(c.got), len(c.prefix)+2)
			continue
		}
		if !bytes.Equal(c.got[:len(c.prefix)], c.prefix) {
			t.Errorf("%s: prefix % X, want % X", c.name, c.got[:len(c.prefix)], c.prefix)
		}
		// CRC covers head = LEN+BODY = bytes [2 : len-2]
		head := c.got[2 : len(c.got)-2]
		wantCRC := CRC16(head)
		gotCRC := binary.LittleEndian.Uint16(c.got[len(c.got)-2:])
		if gotCRC != wantCRC {
			t.Errorf("%s: CRC 0x%04X, want 0x%04X", c.name, gotCRC, wantCRC)
		}
	}
}

// Encode → Decode round-trip.
func TestFrameRoundTrip(t *testing.T) {
	frame := SetFrame(20, 12345.0)
	d := NewDecoder()
	bodies := d.Push(frame)
	if len(bodies) != 1 {
		t.Fatalf("decoded %d frames, want 1", len(bodies))
	}
	v, err := ParseValue(bodies[0]) // body layout of SET matches VALUE for id+f32 fields
	if err != nil {
		t.Fatal(err)
	}
	if v.ID != 20 || v.Val != 12345.0 {
		t.Fatalf("round-trip got id=%d val=%v", v.ID, v.Val)
	}
}

// Decoder must resync after an ASCII log line (mirrors test_protocol.cpp "кадр после ASCII-лога").
func TestDecoderResyncAfterLog(t *testing.T) {
	d := NewDecoder()
	d.Push([]byte("I (123) audio: init\n")) // ESP_LOG noise — fails CRC, skipped
	bodies := d.Push(StatFrame())
	if len(bodies) != 1 || Opcode(bodies[0]) != CmdStat {
		t.Fatalf("frame after ASCII log not recovered: %v", bodies)
	}
}

// A bad CRC frame is dropped; a following good frame still decodes.
func TestDecoderBadCRC(t *testing.T) {
	bad := StatFrame()
	bad[len(bad)-1] ^= 0xFF // corrupt CRC high byte
	d := NewDecoder()
	if got := d.Push(bad); len(got) != 0 {
		t.Fatalf("bad-CRC frame decoded: %v", got)
	}
	if got := d.Push(GetFrame(5)); len(got) != 1 {
		t.Fatalf("good frame after bad one not decoded: %v", got)
	}
}

// Byte-at-a-time feeding (streaming) yields the same frame.
func TestDecoderStreaming(t *testing.T) {
	frame := NoteOnFrame(60, 100)
	d := NewDecoder()
	var bodies [][]byte
	for _, b := range frame {
		bodies = append(bodies, d.Push([]byte{b})...)
	}
	if len(bodies) != 1 || Opcode(bodies[0]) != CmdNoteOn {
		t.Fatalf("streaming decode failed: %v", bodies)
	}
}

// PARAM parse from a hand-built body.
func TestParseParam(t *testing.T) {
	body := make([]byte, 21)
	body[0] = RspParam
	binary.LittleEndian.PutUint16(body[1:], 20)
	body[3] = TypeFloat
	binary.LittleEndian.PutUint32(body[4:], math.Float32bits(20))
	binary.LittleEndian.PutUint32(body[8:], math.Float32bits(20000))
	binary.LittleEndian.PutUint32(body[12:], math.Float32bits(20000))
	binary.LittleEndian.PutUint32(body[16:], math.Float32bits(880))
	name := "cutoff"
	body[20] = byte(len(name))
	body = append(body, name...)

	p, err := ParseParam(body)
	if err != nil {
		t.Fatal(err)
	}
	if p.ID != 20 || p.Type != TypeFloat || p.Max != 20000 || p.Cur != 880 || p.Name != "cutoff" {
		t.Fatalf("ParseParam got %+v", p)
	}
}

func TestParseStat(t *testing.T) {
	s := Stat{Heap: 12345, MinHeap: 6789, UptimeMS: 42, CPUPermille: 500, Underruns: 3}
	body := make([]byte, 21)
	body[0] = RspStat
	for i, v := range []uint32{s.Heap, s.MinHeap, s.UptimeMS, s.CPUPermille, s.Underruns} {
		binary.LittleEndian.PutUint32(body[1+i*4:], v)
	}
	got, err := ParseStat(body)
	if err != nil {
		t.Fatal(err)
	}
	if got != s {
		t.Fatalf("ParseStat got %+v, want %+v", got, s)
	}
}
