package ui

import (
	"gioui.org/layout"
	"gioui.org/unit"
	"gioui.org/widget"

	"ucsynth/app/midi"
)

func (c *Controller) enumMidi() {
	names, err := midi.List()
	if err != nil {
		c.midiMsg = "MIDI: " + err.Error()
		return
	}
	c.midiNames = names
	c.midiBtns = make([]widget.Clickable, len(names))
	if len(names) == 0 {
		c.midiMsg = "MIDI-устройств не найдено"
	} else {
		c.midiMsg = ""
	}
}

// toggleMidi opens device i (closing any open one), or closes it if it is already open.
func (c *Controller) toggleMidi(i int) {
	if c.midiIn != nil {
		c.midiIn.Close()
		c.midiIn = nil
	}
	if c.midiOpen == i {
		c.midiOpen = -1
		c.midiMsg = "MIDI закрыт"
		return
	}
	in, err := midi.Open(i, c.onMidi)
	if err != nil {
		c.midiOpen = -1
		c.midiMsg = "MIDI: " + err.Error()
		return
	}
	c.midiIn = in
	c.midiOpen = i
	c.midiMsg = "MIDI ← " + c.midiNames[i]
}

// onMidi runs on the MIDI message-pump goroutine; route notes through the race-safe sink and ask
// the UI to repaint (so the live message counter/last-message diagnostic updates on each note).
func (c *Controller) onMidi(m midi.Message) {
	switch m.Kind {
	case midi.NoteOn:
		c.sink.on(m.Data1, m.Data2)
	case midi.NoteOff:
		c.sink.off(m.Data1)
	}
	if c.invalidate != nil {
		c.invalidate()
	}
}

func (c *Controller) handleMidi(gtx C) {
	if c.midiRefresh.Clicked(gtx) {
		c.enumMidi()
	}
	for i := range c.midiBtns {
		if i < len(c.midiNames) && c.midiBtns[i].Clicked(gtx) {
			c.toggleMidi(i)
		}
	}
}

func (c *Controller) layoutMidiRow(gtx C) D {
	children := []layout.FlexChild{
		layout.Rigid(label(c.th, unit.Sp(11), "MIDI", colMuted).Layout),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.midiRefresh, "Обновить", false, false, false) }),
	}
	for i := range c.midiBtns {
		i := i
		children = append(children,
			layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D {
				return c.midiBtns[i].Layout(gtx, func(gtx C) D {
					return segPill(c.midiOpen == i).draw(gtx, c.th, c.midiNames[i])
				})
			}),
		)
	}
	// Status + always-on transport diagnostic, left-aligned right after the chips so it can't be
	// pushed off the right edge (helps diagnose MIDI on the user's machine, which I can't run).
	status := c.midiMsg
	if d := midi.Debug(); d != "" {
		if status != "" {
			status += "  ·  "
		}
		status += d
	}
	children = append(children,
		layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
		layout.Rigid(label(c.th, unit.Sp(12), status, colMuted).Layout),
		layout.Flexed(1, func(gtx C) D { return D{Size: gtx.Constraints.Min} }),
	)
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, children...)
}
