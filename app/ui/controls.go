package ui

import (
	"strconv"
	"strings"

	"gioui.org/layout"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	blk "ucsynth/app/layout"
	"ucsynth/app/proto"
)

// C, D are the Gio layout context / dimensions, aliased for brevity.
type C = layout.Context
type D = layout.Dimensions

// setFunc sends a parameter value to the device (throttled downstream).
type setFunc func(id uint16, val float32)

type ctlKind int

const (
	kindKnob ctlKind = iota
	kindSlider
	kindSeg
	kindToggle
	kindStepper
)

// control is the editable widget for one parameter. Widget state persists across frames; the param
// metadata (p) is refreshed from the device snapshot each frame.
type control struct {
	p    proto.Param
	fld  blk.Field
	kind ctlKind

	knob Knob             // knob float
	vsl  VSlider          // slider float (ADSR)
	tog  widget.Clickable // bool
	seg  []widget.Clickable
	dec  widget.Clickable // int −
	inc  widget.Clickable // int +
}

func newControl(p proto.Param) *control {
	c := &control{p: p, fld: blk.For(p.Name)}
	switch p.Type {
	case proto.TypeEnum:
		c.kind = kindSeg
		n := int(p.Max-p.Min) + 1
		if n < 1 {
			n = 1
		}
		c.seg = make([]widget.Clickable, n)
	case proto.TypeBool:
		c.kind = kindToggle
	case proto.TypeInt:
		c.kind = kindStepper
	default:
		if blk.IsEnvSlider(p.Name) {
			c.kind = kindSlider
		} else {
			c.kind = kindKnob
		}
	}
	return c
}

// --- cell renderers (each returns one self-contained control cell) --------------------------------

func (c *control) knobCell(gtx C, th *material.Theme, set setFunc) D {
	min, max := c.p.Min, c.p.Max
	span := max - min
	if !c.knob.Dragging() && span != 0 {
		c.knob.Value = clamp01((c.p.Cur - min) / span)
	}
	live := min + c.knob.Value*span
	dims := fixedW(gtx, gtx.Dp(70), func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical, Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(c.knob.Layout),
			layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
			layout.Rigid(label(th, unit.Sp(11), fmtVal(live, c.fld.Unit), colTxt).Layout),
			layout.Rigid(layout.Spacer{Height: unit.Dp(1)}.Layout),
			layout.Rigid(capLabel(th, c.fld.Label, colFaint)),
		)
	})
	if c.knob.Changed() {
		set(c.p.ID, min+c.knob.Value*span)
	}
	return dims
}

func (c *control) sliderCell(gtx C, th *material.Theme, set setFunc) D {
	min, max := c.p.Min, c.p.Max
	span := max - min
	if !c.vsl.Dragging() && span != 0 {
		c.vsl.Value = clamp01((c.p.Cur - min) / span)
	}
	dims := fixedW(gtx, gtx.Dp(34), func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical, Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(c.vsl.Layout),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(capLabel(th, envLetter(c.fld.Label), colMuted)),
		)
	})
	if c.vsl.Changed() {
		set(c.p.ID, min+c.vsl.Value*span)
	}
	return dims
}

func (c *control) segField(gtx C, th *material.Theme, set setFunc) D {
	cur := int(c.p.Cur + 0.5)
	for i := range c.seg { // read clicks before drawing (Clickable.Layout drains them)
		if c.seg[i].Clicked(gtx) {
			set(c.p.ID, float32(i))
			cur = i
		}
	}
	segs := make([]layout.FlexChild, 0, len(c.seg))
	for i := range c.seg {
		i := i
		on := i == cur
		segs = append(segs, layout.Rigid(func(gtx C) D {
			return layout.Inset{Right: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
				return c.seg[i].Layout(gtx, func(gtx C) D {
					return segPill(on).draw(gtx, th, c.fld.EnumLabel(i))
				})
			})
		}))
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(capLabel(th, c.fld.Label, colFaint)),
		layout.Rigid(layout.Spacer{Height: unit.Dp(5)}.Layout),
		layout.Rigid(func(gtx C) D { return layout.Flex{Axis: layout.Horizontal}.Layout(gtx, segs...) }),
	)
}

func (c *control) toggleCell(gtx C, th *material.Theme, set setFunc) D {
	on := c.p.Cur > 0.5
	if c.tog.Clicked(gtx) {
		on = !on
		set(c.p.ID, boolf(on))
	}
	return c.tog.Layout(gtx, func(gtx C) D {
		return togPill(on).draw(gtx, th, c.fld.Label)
	})
}

func (c *control) stepperCell(gtx C, th *material.Theme, set setFunc) D {
	cur := int(c.p.Cur + 0.5)
	lo, hi := int(c.p.Min+0.5), int(c.p.Max+0.5)
	if c.dec.Clicked(gtx) && cur > lo {
		cur--
		set(c.p.ID, float32(cur))
	}
	if c.inc.Clicked(gtx) && cur < hi {
		cur++
		set(c.p.ID, float32(cur))
	}
	step := func(b *widget.Clickable, s string) layout.Widget {
		return func(gtx C) D {
			return b.Layout(gtx, func(gtx C) D {
				return pill{border: colLine2, text: colMuted, size: unit.Sp(15), padX: 8, padY: 2, radius: 6}.draw(gtx, th, s)
			})
		}
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(capLabel(th, c.fld.Label, colFaint)),
		layout.Rigid(layout.Spacer{Height: unit.Dp(5)}.Layout),
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(step(&c.dec, "−")),
				layout.Rigid(func(gtx C) D {
					return layout.Inset{Left: unit.Dp(10), Right: unit.Dp(10)}.Layout(gtx,
						label(th, unit.Sp(14), strconv.Itoa(cur), colTxt).Layout)
				}),
				layout.Rigid(step(&c.inc, "+")),
			)
		}),
	)
}

// --- helpers -------------------------------------------------------------------------------------

// fixedW lays out w with a fixed width so knob/slider cells align in a grid.
func fixedW(gtx C, w int, wdg layout.Widget) D {
	gtx.Constraints.Min.X = w
	gtx.Constraints.Max.X = w
	return wdg(gtx)
}

// envLetter is the single-letter fader label (Attack→A, Decay→D, …).
func envLetter(label string) string {
	r := []rune(label)
	if len(r) == 0 {
		return ""
	}
	return strings.ToUpper(string(r[0]))
}

func fmtVal(v float32, unit string) string {
	var s string
	switch {
	case v == float32(int64(v)):
		s = strconv.FormatInt(int64(v), 10)
	case unit == "с":
		s = strconv.FormatFloat(float64(v), 'f', 3, 32)
	case v >= 1000 || v <= -1000:
		s = strconv.FormatFloat(float64(v)/1000, 'f', 1, 32) + "k"
	case v >= 100 || v <= -100:
		s = strconv.FormatFloat(float64(v), 'f', 0, 32)
	default:
		s = strconv.FormatFloat(float64(v), 'f', 2, 32)
	}
	if unit != "" && !strings.HasSuffix(s, "k") {
		s += " " + unit
	} else if unit != "" {
		s += unit
	}
	return s
}

func boolf(b bool) float32 {
	if b {
		return 1
	}
	return 0
}
