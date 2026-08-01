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

// Golden frames — FULL literal bytes including the two CRC bytes, mirroring the hex tables in
// docs/serial-protocol.md and the identical set in test/host/test_protocol.cpp. This is the real
// cross-anchor T-007 asked for: the CRC bytes here are hardcoded literals computed by an
// independent reference (not recomputed by CRC16), so a change that flips CRC coverage
// (LEN+BODY→BODY), CRC byte order, or a field layout fails HERE even though the Go encoder and its
// own CRC16 would still agree with each other. Keep these three sources byte-identical.
//
// If a genuine protocol change lands, update all three (doc + both tests) together — that edit is
// the point where the wire contract is deliberately re-agreed.
func TestEncodeGoldenFrames(t *testing.T) {
	cases := []struct {
		name string
		got  []byte
		want []byte // complete frame: sync(2) + LEN + BODY + CRC_LE(2)
	}{
		{"LIST", ListFrame(), []byte{0x55, 0xAA, 0x01, 0x03, 0x5D, 0x1E}},
		{"GET id0", GetFrame(0), []byte{0x55, 0xAA, 0x03, 0x02, 0x00, 0x00, 0x7C, 0x71}},
		{"SET id0=0.5", SetFrame(0, 0.5), []byte{0x55, 0xAA, 0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFB, 0x89}},
		{"STAT", StatFrame(), []byte{0x55, 0xAA, 0x01, 0x06, 0xF8, 0x4E}},
		{"NOTE_ON 60,100", NoteOnFrame(60, 100), []byte{0x55, 0xAA, 0x03, 0x04, 0x3C, 0x64, 0x06, 0xAF}},
		{"NOTE_OFF 60", NoteOffFrame(60), []byte{0x55, 0xAA, 0x02, 0x05, 0x3C, 0xD6, 0xAA}},
		{"VALUE 0.8", ValueRespFrame(0, 0.8), []byte{0x55, 0xAA, 0x07, 0x81, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3F, 0x17, 0xB3}},
		{"empty body", EncodeFrame(nil), []byte{0x55, 0xAA, 0x00, 0xF0, 0xE1}},
	}
	for _, c := range cases {
		// Encoder must produce the exact bytes, CRC included.
		if !bytes.Equal(c.got, c.want) {
			t.Errorf("%s: encoded % X, want % X", c.name, c.got, c.want)
		}
		// And our decoder must accept the literal golden bytes and recover the body — proves the
		// CRC these literals carry is the one our decoder validates against.
		d := NewDecoder()
		bodies := d.Push(append([]byte(nil), c.want...))
		if len(bodies) != 1 {
			t.Errorf("%s: golden frame decoded into %d bodies, want 1", c.name, len(bodies))
			continue
		}
		wantBody := c.want[3 : len(c.want)-2]
		if !bytes.Equal(bodies[0], wantBody) {
			t.Errorf("%s: decoded body % X, want % X", c.name, bodies[0], wantBody)
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

// The log line above contains no 0x55, so it never exercises resync at all. A false sync does:
// nothing may be lost after one, in either feeding order.
func TestDecoderResyncAfterFalseSync(t *testing.T) {
	d := NewDecoder()
	d.Push([]byte{'x', Sync0, Sync1}) // false sync with no frame behind it

	got := 0
	for rep := 0; rep < 20; rep++ {
		got += len(d.Push(StatFrame()))
	}
	if got != 20 {
		t.Fatalf("decoded %d of 20 frames after a false sync", got)
	}
}

// A false sync must not withhold a frame that has already fully arrived behind it. Left unhandled
// this is a head-of-line stall: the ready frame waits for bytes that may never come.
func TestDecoderNoHeadOfLineStall(t *testing.T) {
	d := NewDecoder()
	// One push: false sync, then a complete GET frame. The false sync's LEN byte is whatever the
	// real frame starts with, so it declares a length we do not have.
	in := append([]byte{Sync0, Sync1}, GetFrame(5)...)
	bodies := d.Push(in)
	if len(bodies) != 1 {
		t.Fatalf("decoded %d frames, want the GET frame delivered immediately", len(bodies))
	}
	if Opcode(bodies[0]) != CmdGet {
		t.Fatalf("wrong frame recovered: %v", bodies[0])
	}
}

// LEN=0 is legal on the wire (the firmware emits and accepts it), so the encoder must not panic on
// an empty body — code that round-trips a received body through the encoder would crash.
func TestEncodeEmptyBody(t *testing.T) {
	f := EncodeFrame(nil)
	d := NewDecoder()
	bodies := d.Push(f)
	if len(bodies) != 1 || len(bodies[0]) != 0 {
		t.Fatalf("empty body did not round-trip: %v", bodies)
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
