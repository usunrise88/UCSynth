package ui

import (
	"image"
	"image/color"
	"math"

	"gioui.org/f32"
	"gioui.org/io/event"
	"gioui.org/io/pointer"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
)

// Knob is a radial control: vertical drag changes Value in [0,1]. Custom-drawn — a 270° track arc,
// a blue value arc, and a pointer tick. It reports edits via Changed(); the owning control maps
// [0,1] to the parameter range.
type Knob struct {
	Value   float32
	drag    bool
	lastY   float32
	changed bool
}

// Changed reports whether the user moved the knob since the last call, and clears the flag.
func (k *Knob) Changed() bool { c := k.changed; k.changed = false; return c }

// Dragging reports whether the knob is being held (used for anti-echo: don't snap to device value).
func (k *Knob) Dragging() bool { return k.drag }

func (k *Knob) update(gtx C) {
	for {
		ev, ok := gtx.Event(pointer.Filter{Target: k, Kinds: pointer.Press | pointer.Drag | pointer.Release | pointer.Cancel})
		if !ok {
			break
		}
		e, ok := ev.(pointer.Event)
		if !ok {
			continue
		}
		switch e.Kind {
		case pointer.Press:
			k.drag = true
			k.lastY = e.Position.Y
			// Grab the pointer so Drag/Release keep coming even when the cursor leaves the small
			// knob rect (the drag range is far larger than the knob).
			gtx.Execute(pointer.GrabCmd{Tag: k, ID: e.PointerID})
		case pointer.Drag:
			if k.drag {
				k.Value = clamp01(k.Value + (k.lastY-e.Position.Y)/150) // up = increase
				k.lastY = e.Position.Y
				k.changed = true
			}
		case pointer.Release, pointer.Cancel:
			k.drag = false
		}
	}
}

const (
	knobDia   = 50         // dp
	knobStart = -2.3561945 // -135°, min at bottom-left
	knobSweep = 4.712389   // 270°
)

// Layout draws the knob and registers drag input. The owning control draws value + label around it.
func (k *Knob) Layout(gtx C) D {
	k.update(gtx)
	d := gtx.Dp(knobDia)
	sz := image.Pt(d, d)

	// Input area = the knob square.
	area := clip.Rect{Max: sz}.Push(gtx.Ops)
	event.Op(gtx.Ops, k)
	area.Pop()

	pad := gtx.Dp(5)
	body := clip.Ellipse{Min: image.Pt(pad, pad), Max: image.Pt(d-pad, d-pad)}
	paint.FillShape(gtx.Ops, colKnobBody, body.Op(gtx.Ops))

	c := f32.Pt(float32(d)/2, float32(d)/2)
	r := float32(d)/2 - float32(pad)/2
	w := float32(gtx.Dp(4))
	strokeArc(gtx.Ops, c, r, w, knobStart, knobStart+knobSweep, colTrack)
	if k.Value > 0.001 {
		strokeArc(gtx.Ops, c, r, w, knobStart, knobStart+knobSweep*k.Value, colAccent)
	}
	// pointer tick
	ang := knobStart + knobSweep*k.Value
	var p clip.Path
	p.Begin(gtx.Ops)
	p.MoveTo(polar(c, r*0.30, ang))
	p.LineTo(polar(c, r*0.72, ang))
	paint.FillShape(gtx.Ops, colAccentB, clip.Stroke{Path: p.End(), Width: float32(gtx.Dp(2))}.Op())

	return D{Size: sz}
}

// strokeArc strokes a circular arc centred at c, radius r, from angle phi0 to phi1 (radians,
// clockwise from 12 o'clock), as a short polyline — reliable and crisp at knob sizes.
func strokeArc(ops *op.Ops, c f32.Point, r, width, phi0, phi1 float32, col color.NRGBA) {
	var p clip.Path
	p.Begin(ops)
	const seg = 40
	for i := 0; i <= seg; i++ {
		phi := phi0 + (phi1-phi0)*float32(i)/float32(seg)
		pt := polar(c, r, phi)
		if i == 0 {
			p.MoveTo(pt)
		} else {
			p.LineTo(pt)
		}
	}
	paint.FillShape(ops, col, clip.Stroke{Path: p.End(), Width: width}.Op())
}

// polar returns the point at radius r and angle phi (clockwise from 12 o'clock; screen y is down).
func polar(c f32.Point, r, phi float32) f32.Point {
	return f32.Pt(c.X+r*float32(math.Sin(float64(phi))), c.Y-r*float32(math.Cos(float64(phi))))
}

func clamp01(v float32) float32 {
	if v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}
