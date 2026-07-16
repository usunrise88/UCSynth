//go:build !windows

package midi

import "errors"

// List returns no devices off Windows (the transport is WinMM-only).
func List() ([]string, error) { return nil, nil }

// Open is unsupported off Windows; the app still builds and runs (MIDI just unavailable).
func Open(index int, handler func(Message)) (Input, error) {
	return nil, errors.New("MIDI-вход поддерживается только на Windows")
}
