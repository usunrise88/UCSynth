package midi

import "testing"

func TestParse(t *testing.T) {
	cases := []struct {
		st, d1, d2 uint8
		want       Message
	}{
		{0x90, 60, 100, Message{NoteOn, 0, 60, 100}},
		{0x92, 64, 80, Message{NoteOn, 2, 64, 80}},
		{0x90, 60, 0, Message{NoteOff, 0, 60, 0}}, // note-on vel 0 → note-off
		{0x80, 60, 64, Message{NoteOff, 0, 60, 64}},
		{0x81, 62, 0, Message{NoteOff, 1, 62, 0}},
		{0xB0, 1, 127, Message{ControlChange, 0, 1, 127}}, // mod wheel
		{0xE0, 0, 64, Message{Other, 0, 0, 64}},           // pitch bend → Other
	}
	for _, c := range cases {
		if got := Parse(c.st, c.d1, c.d2); got != c.want {
			t.Errorf("Parse(%#x,%d,%d) = %+v, want %+v", c.st, c.d1, c.d2, got, c.want)
		}
	}
}

func TestParseWord(t *testing.T) {
	// WinMM packs status | data1<<8 | data2<<16.
	dw := uint32(0x90) | uint32(60)<<8 | uint32(100)<<16
	if got := ParseWord(dw); got != (Message{NoteOn, 0, 60, 100}) {
		t.Fatalf("ParseWord = %+v", got)
	}
}
