package ui

import (
	"image"

	"gioui.org/io/event"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/unit"
)

// rollGeom is the grid geometry from the last frame, used to decode a click into a (step, pitch).
type rollGeom struct {
	x0, cellW, cellH int
	steps, hi        int
}

func isBlackKey(pitch int) bool {
	switch ((pitch % 12) + 12) % 12 {
	case 1, 3, 6, 8, 10:
		return true
	}
	return false
}

func (c *Controller) handleSeq(gtx C) {
	if c.playBtn.Clicked(gtx) {
		if c.player.Playing() {
			c.player.Stop()
		} else {
			c.player.Start()
		}
	}
	if c.clearBtn.Clicked(gtx) {
		c.player.Clear()
	}
	if c.tempoDec.Clicked(gtx) {
		c.player.SetBPM(c.player.BPM() - 5)
	}
	if c.tempoInc.Clicked(gtx) {
		c.player.SetBPM(c.player.BPM() + 5)
	}
}

func (c *Controller) layoutPianoRoll(gtx C) D {
	return card(gtx, colLine, colPanel, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(c.rollTransport),
			layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
			layout.Flexed(1, c.rollGrid),
		)
	})
}

func (c *Controller) rollTransport(gtx C) D {
	playLbl := "▶ Играть"
	if c.player.Playing() {
		playLbl = "■ Стоп"
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.playBtn, playLbl, true, false, c.player.Playing()) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.clearBtn, "Очистить", false, false, false) }),
		layout.Flexed(1, func(gtx C) D { return D{Size: gtx.Constraints.Min} }),
		layout.Rigid(label(c.th, unit.Sp(12), "Темп", colMuted).Layout),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.tempoDec, "−", false, false, false) }),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(10), Right: unit.Dp(10)}.Layout(gtx,
				label(c.th, unit.Sp(14), fmtInt(float32(c.player.BPM()))+" BPM", colTxt).Layout)
		}),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.tempoInc, "+", false, false, false) }),
	)
}

func (c *Controller) rollGrid(gtx C) D {
	// Decode last frame's clicks against the stored geometry (position is local to this grid).
	for {
		ev, ok := gtx.Event(pointer.Filter{Target: &c.rollTag, Kinds: pointer.Press})
		if !ok {
			break
		}
		e, ok := ev.(pointer.Event)
		if !ok {
			continue
		}
		g := c.rollGeom
		x, y := int(e.Position.X), int(e.Position.Y)
		if g.cellW > 0 && g.cellH > 0 && x >= g.x0 {
			step := (x - g.x0) / g.cellW
			pitch := g.hi - y/g.cellH
			if step >= 0 && step < g.steps && pitch >= c.player.Lo() && pitch <= c.player.Hi() {
				c.player.Toggle(step, pitch)
			}
		}
	}

	W := gtx.Constraints.Max.X
	rows := c.player.Hi() - c.player.Lo() + 1
	steps := c.player.Steps()
	if rows < 1 || steps < 1 || W <= 0 {
		return D{Size: image.Pt(W, gtx.Constraints.Max.Y)}
	}
	gutter := gtx.Dp(40)
	cellW := (W - gutter) / steps
	availH := gtx.Constraints.Max.Y
	cellH := availH / rows
	if cellH < gtx.Dp(9) {
		cellH = gtx.Dp(9)
	}
	H := cellH * rows
	c.rollGeom = rollGeom{x0: gutter, cellW: cellW, cellH: cellH, steps: steps, hi: c.player.Hi()}

	area := clip.Rect{Max: image.Pt(W, H)}.Push(gtx.Ops)
	event.Op(gtx.Ops, &c.rollTag)
	area.Pop()

	cur := c.player.Cur()
	playing := c.player.Playing()
	for r := 0; r < rows; r++ {
		pitch := c.player.Hi() - r
		y := r * cellH
		bg := colKnobBody
		if isBlackKey(pitch) {
			bg = rgb(0x161B22)
		}
		fillRRect(gtx.Ops, image.Rect(gutter, y, W, y+cellH-1), 0, bg)
		if ((pitch%12)+12)%12 == 0 { // C: label the octave
			off := op.Offset(image.Pt(gtx.Dp(4), y+cellH/2-gtx.Dp(7))).Push(gtx.Ops)
			label(c.th, unit.Sp(10), noteName(pitch), colFaint).Layout(gtx)
			off.Pop()
		}
		for s := 0; s < steps; s++ {
			x := gutter + s*cellW
			on := c.player.On(s, pitch)
			col := colTrack
			switch {
			case s == cur && playing && on:
				col = colAccentB
			case s == cur && playing:
				col = rgba(0x3B8EF5, 0x3A)
			case on:
				col = colAccent
			}
			fillRRect(gtx.Ops, image.Rect(x+1, y+1, x+cellW-1, y+cellH-2), gtx.Dp(2), col)
		}
	}
	return D{Size: image.Pt(W, H)}
}
