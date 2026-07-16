package ui

import (
	"image"
	"strconv"

	"gioui.org/font"
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

// control is the editable widget for one parameter. Widget state persists across frames; the
// param metadata (p) is refreshed from the device snapshot each frame.
type control struct {
	p   proto.Param
	fld blk.Field

	flt widget.Float      // float
	sw  widget.Bool       // bool
	seg []widget.Clickable // enum options (one per index)
	dec widget.Clickable  // int −
	inc widget.Clickable  // int +
}

func newControl(p proto.Param) *control {
	c := &control{p: p, fld: blk.For(p.Name)}
	if p.Type == proto.TypeEnum {
		n := int(p.Max-p.Min) + 1
		if n < 1 {
			n = 1
		}
		c.seg = make([]widget.Clickable, n)
	}
	return c
}

// row lays out the label + control + value for one parameter and emits SETs on user change.
func (c *control) row(gtx C, th *material.Theme, set setFunc) D {
	switch c.p.Type {
	case proto.TypeBool:
		return c.rowBool(gtx, th, set)
	case proto.TypeEnum:
		return c.rowEnum(gtx, th, set)
	case proto.TypeInt:
		return c.rowInt(gtx, th, set)
	default:
		return c.rowFloat(gtx, th, set)
	}
}

func (c *control) rowFloat(gtx C, th *material.Theme, set setFunc) D {
	min, max := c.p.Min, c.p.Max
	span := max - min
	// Reflect the device value into the slider UNLESS the user is dragging (anti snap-back).
	if !c.flt.Dragging() && span != 0 {
		c.flt.Value = (c.p.Cur - min) / span
	}
	before := c.flt.Value

	live := min + c.flt.Value*span
	dims := layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(c.label(th)),
		layout.Flexed(1, func(gtx C) D {
			return layout.Inset{Left: unit.Dp(8), Right: unit.Dp(8)}.Layout(gtx,
				material.Slider(th, &c.flt).Layout)
		}),
		layout.Rigid(valueLabel(th, fmtVal(live, c.fld.Unit))),
	)

	if after := c.flt.Value; after != before {
		set(c.p.ID, min+after*span) // device coalesces the drag stream
	}
	return dims
}

func (c *control) rowBool(gtx C, th *material.Theme, set setFunc) D {
	c.sw.Value = c.p.Cur > 0.5 // reflect device state
	before := c.sw.Value
	dims := layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(c.label(th)),
		layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
		layout.Rigid(material.Switch(th, &c.sw, c.fld.Label).Layout),
	)
	if c.sw.Value != before {
		set(c.p.ID, boolf(c.sw.Value))
	}
	return dims
}

func (c *control) rowEnum(gtx C, th *material.Theme, set setFunc) D {
	cur := int(c.p.Cur + 0.5)
	// Emit SET on click before drawing (so the highlight updates same frame via device echo/optimism).
	for i := range c.seg {
		if c.seg[i].Clicked(gtx) {
			set(c.p.ID, float32(i))
			cur = i
		}
	}
	segs := make([]layout.FlexChild, 0, len(c.seg))
	for i := range c.seg {
		i := i
		segs = append(segs, layout.Rigid(func(gtx C) D {
			b := material.Button(th, &c.seg[i], c.fld.EnumLabel(i))
			b.TextSize = unit.Sp(13)
			b.Inset = layout.UniformInset(unit.Dp(4))
			if i == cur {
				b.Background = th.ContrastBg
			} else {
				b.Background = th.Bg
				b.Color = th.Fg
			}
			return layout.Inset{Right: unit.Dp(4)}.Layout(gtx, b.Layout)
		}))
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(c.label(th)),
		layout.Flexed(1, func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal}.Layout(gtx, segs...)
		}),
	)
}

func (c *control) rowInt(gtx C, th *material.Theme, set setFunc) D {
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
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(c.label(th)),
		layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
		layout.Rigid(material.Button(th, &c.dec, "−").Layout),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(10), Right: unit.Dp(10)}.Layout(gtx,
				valueLabel(th, fmtVal(float32(cur), c.fld.Unit)))
		}),
		layout.Rigid(material.Button(th, &c.inc, "+").Layout),
	)
}

func (c *control) label(th *material.Theme) layout.Widget {
	return func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Dp(140)
		l := material.Body2(th, c.fld.Label)
		return l.Layout(gtx)
	}
}

func valueLabel(th *material.Theme, s string) layout.Widget {
	return func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Dp(96)
		l := material.Body2(th, s)
		l.Font.Weight = font.Medium
		l.Alignment = 2 // text.End
		return l.Layout(gtx)
	}
}

func fmtVal(v float32, unit string) string {
	var s string
	switch {
	case v == float32(int64(v)):
		s = strconv.FormatInt(int64(v), 10)
	case unit == "с":
		s = strconv.FormatFloat(float64(v), 'f', 3, 32)
	case v >= 100 || v <= -100:
		s = strconv.FormatFloat(float64(v), 'f', 0, 32)
	default:
		s = strconv.FormatFloat(float64(v), 'f', 2, 32)
	}
	if unit != "" {
		s += " " + unit
	}
	return s
}

func boolf(b bool) float32 {
	if b {
		return 1
	}
	return 0
}
