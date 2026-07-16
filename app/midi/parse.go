package midi

// Parse decodes a 3-byte MIDI channel message (status, data1, data2). Running status is not handled
// — WinMM delivers complete messages. A note-on with velocity 0 is treated as note-off (the common
// convention many controllers use for key release).
func Parse(status, d1, d2 uint8) Message {
	ch := status & 0x0F
	switch status & 0xF0 {
	case 0x90:
		if d2 == 0 {
			return Message{Kind: NoteOff, Chan: ch, Data1: d1, Data2: 0}
		}
		return Message{Kind: NoteOn, Chan: ch, Data1: d1, Data2: d2}
	case 0x80:
		return Message{Kind: NoteOff, Chan: ch, Data1: d1, Data2: d2}
	case 0xB0:
		return Message{Kind: ControlChange, Chan: ch, Data1: d1, Data2: d2}
	default:
		return Message{Kind: Other, Chan: ch, Data1: d1, Data2: d2}
	}
}

// ParseWord decodes a packed WinMM MIM_DATA dwParam: status in the low byte, then data1, data2.
func ParseWord(dw uint32) Message {
	return Parse(uint8(dw), uint8(dw>>8), uint8(dw>>16))
}
