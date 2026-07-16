package ui

import (
	"image"
	"image/color"
	"strings"

	"gioui.org/font"
	"gioui.org/io/event"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// ---- VSlider: vertical fader for ADSR (drag up = increase) --------------------------------------

const (
	vslW = 22 // dp
	vslH = 92 // dp — also the drag scale
)

// VSlider is a thin vertical fader with Value in [0,1].
type VSlider struct {
	Value   float32
	drag    bool
	lastY   float32
	changed bool
}

func (s *VSlider) Changed() bool  { c := s.changed; s.changed = false; return c }
func (s *VSlider) Dragging() bool { return s.drag }

func (s *VSlider) update(gtx C) {
	for {
		ev, ok := gtx.Event(pointer.Filter{Target: s, Kinds: pointer.Press | pointer.Drag | pointer.Release | pointer.Cancel})
		if !ok {
			break
		}
		e, ok := ev.(pointer.Event)
		if !ok {
			continue
		}
		switch e.Kind {
		case pointer.Press:
			s.drag = true
			s.lastY = e.Position.Y
			gtx.Execute(pointer.GrabCmd{Tag: s, ID: e.PointerID})
		case pointer.Drag:
			if s.drag {
				s.Value = clamp01(s.Value + (s.lastY-e.Position.Y)/float32(gtx.Dp(vslH)))
				s.lastY = e.Position.Y
				s.changed = true
			}
		case pointer.Release, pointer.Cancel:
			s.drag = false
		}
	}
}

func (s *VSlider) Layout(gtx C) D {
	s.update(gtx)
	w, h := gtx.Dp(vslW), gtx.Dp(vslH)
	sz := image.Pt(w, h)
	area := clip.Rect{Max: sz}.Push(gtx.Ops)
	event.Op(gtx.Ops, s)
	area.Pop()

	tw := gtx.Dp(3)
	x := (w - tw) / 2
	fillRRect(gtx.Ops, image.Rect(x, 0, x+tw, h), tw/2, colTrack)
	fillH := int(float32(h) * s.Value)
	if fillH > 0 {
		fillRRect(gtx.Ops, image.Rect(x, h-fillH, x+tw, h), tw/2, colAccent)
	}
	// handle
	hh, hw := gtx.Dp(9), gtx.Dp(17)
	cy := h - fillH
	hy := cy - hh/2
	if hy < 0 {
		hy = 0
	}
	if hy > h-hh {
		hy = h - hh
	}
	hx := (w - hw) / 2
	fillRRect(gtx.Ops, image.Rect(hx, hy, hx+hw, hy+hh), gtx.Dp(3), rgb(0xCFD6DF))
	return D{Size: sz}
}

// ---- primitive draw helpers ---------------------------------------------------------------------

func fillRRect(ops *op.Ops, r image.Rectangle, rad int, col color.NRGBA) {
	rr := clip.RRect{Rect: r, SE: rad, SW: rad, NE: rad, NW: rad}
	paint.FillShape(ops, col, rr.Op(ops))
}

func strokeRRect(ops *op.Ops, r image.Rectangle, rad int, width float32, col color.NRGBA) {
	rr := clip.RRect{Rect: r, SE: rad, SW: rad, NE: rad, NW: rad}
	paint.FillShape(ops, col, clip.Stroke{Path: rr.Path(ops), Width: width}.Op())
}

// ---- pills (outlined buttons / segments / toggles) ----------------------------------------------

type pill struct {
	border color.NRGBA
	fill   color.NRGBA // A==0 → no fill
	text   color.NRGBA
	size   unit.Sp
	padX   int
	padY   int
	radius int
	dot    bool // draw a leading status dot (toggles)
	dotCol color.NRGBA
}

// draw renders an outlined pill sized to its label and returns its dimensions. Content on top of a
// fill+border drawn behind (measure-first, like material.Button).
func (p pill) draw(gtx C, th *material.Theme, label string) D {
	macro := op.Record(gtx.Ops)
	dims := layout.Inset{
		Top: unit.Dp(p.padY), Bottom: unit.Dp(p.padY), Left: unit.Dp(p.padX), Right: unit.Dp(p.padX),
	}.Layout(gtx, func(gtx C) D {
		return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				if !p.dot {
					return D{}
				}
				return layout.Inset{Right: unit.Dp(7)}.Layout(gtx, func(gtx C) D {
					return dotIcon(gtx, p.dotCol, p.fill.A > 0)
				})
			}),
			layout.Rigid(func(gtx C) D {
				l := material.Label(th, p.size, label)
				l.Color = p.text
				l.MaxLines = 1
				return l.Layout(gtx)
			}),
		)
	})
	call := macro.Stop()
	r := image.Rectangle{Max: dims.Size}
	if p.fill.A > 0 {
		fillRRect(gtx.Ops, r, p.radius, p.fill)
	}
	strokeRRect(gtx.Ops, r, p.radius, float32(gtx.Dp(1)), p.border)
	call.Add(gtx.Ops)
	return dims
}

func dotIcon(gtx C, col color.NRGBA, filled bool) D {
	d := gtx.Dp(9)
	sz := image.Pt(d, d)
	if filled {
		paint.FillShape(gtx.Ops, col, clip.Ellipse{Max: sz}.Op(gtx.Ops))
	} else {
		e := clip.Ellipse{Max: sz}
		paint.FillShape(gtx.Ops, col, clip.Stroke{Path: e.Path(gtx.Ops), Width: float32(gtx.Dp(1))}.Op())
	}
	return D{Size: sz}
}

// btnPill styles a top-bar / footer outlined button (variants: normal, accent, danger, active).
func btnPill(active bool, accent, danger bool) pill {
	p := pill{border: colLine2, text: colTxt, size: unit.Sp(13), padX: 13, padY: 6, radius: 7}
	switch {
	case danger:
		p.border, p.text = colWarn, colWarn
	case accent:
		p.border, p.text = colAccent, colAccentB
	}
	if active {
		p.border, p.text, p.fill = colAccent, colAccentB, colAccentDim
	}
	return p
}

// segPill styles one enum segment.
func segPill(on bool) pill {
	p := pill{border: colLine2, text: colMuted, size: unit.Sp(11), padX: 9, padY: 4, radius: 6}
	if on {
		p.border, p.text, p.fill = colAccent, colAccentB, colAccentDim
	}
	return p
}

// togPill styles a bool toggle (outlined pill + status dot).
func togPill(on bool) pill {
	p := pill{border: colLine2, text: colMuted, size: unit.Sp(12), padX: 11, padY: 5, radius: 7, dot: true, dotCol: colMuted}
	if on {
		p.border, p.text, p.fill, p.dotCol = colAccent, colAccentB, colAccentDim, colAccentB
	}
	return p
}

// ---- panel + text helpers -----------------------------------------------------------------------

// vstPanel draws a titled bordered card around content (measure-first so the border wraps content).
func vstPanel(gtx C, th *material.Theme, title string, content layout.Widget) D {
	macro := op.Record(gtx.Ops)
	dims := layout.UniformInset(unit.Dp(12)).Layout(gtx, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				l := material.Label(th, unit.Sp(11), strings.ToUpper(title))
				l.Color = colMuted
				l.Font.Weight = font.Medium
				return l.Layout(gtx)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
			layout.Rigid(content),
		)
	})
	call := macro.Stop()
	r := image.Rectangle{Max: dims.Size}
	fillRRect(gtx.Ops, r, gtx.Dp(10), colPanel)
	strokeRRect(gtx.Ops, r, gtx.Dp(10), float32(gtx.Dp(1)), colLine)
	call.Add(gtx.Ops)
	return dims
}

func label(th *material.Theme, size unit.Sp, s string, col color.NRGBA) material.LabelStyle {
	l := material.Label(th, size, s)
	l.Color = col
	l.MaxLines = 1
	return l
}

// capLabel is a small uppercase caption (knob/slider labels).
func capLabel(th *material.Theme, s string, col color.NRGBA) layout.Widget {
	return func(gtx C) D {
		return label(th, unit.Sp(10), strings.ToUpper(s), col).Layout(gtx)
	}
}
