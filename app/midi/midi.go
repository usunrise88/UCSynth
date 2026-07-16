// Package midi provides MIDI input for the controller. Message parsing is pure and cross-platform;
// the transport is Windows-only (WinMM via stdlib syscall — cgo-free, so the app still cross-builds
// from Linux) with a stub elsewhere. Only note on/off and control-change are decoded.
package midi

// Kind is the decoded message category.
type Kind int

const (
	NoteOn Kind = iota
	NoteOff
	ControlChange
	Other
)

// Message is a decoded MIDI channel message.
type Message struct {
	Kind  Kind
	Chan  uint8 // 0..15
	Data1 uint8 // note number or controller number
	Data2 uint8 // velocity or controller value
}

// Input is an open MIDI input port. Close stops delivery and releases the device.
type Input interface {
	Close() error
}
