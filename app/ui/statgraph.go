package ui

import (
	"image"
	"image/color"

	"gioui.org/f32"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget/material"

	"ucsynth/app/proto"
)

const statCap = 120 // samples kept (~1 min at the 2 Hz STAT poll)

// statRing keeps a rolling window of STAT-derived series for the history graphs. Pure logic —
// unit-tested. Underruns is a monotonic counter, so we store the per-sample delta.
type statRing struct {
	cpu, heap, und []float32
	lastUnd        uint32
	haveUnd        bool
}

func (r *statRing) push(s proto.Stat) {
	var d float32
	if r.haveUnd && s.Underruns >= r.lastUnd {
		d = float32(s.Underruns - r.lastUnd)
	}
	r.lastUnd, r.haveUnd = s.Underruns, true
	r.cpu = appendCap(r.cpu, float32(s.CPUPermille)/10)
	r.heap = appendCap(r.heap, float32(s.Heap)/1048576)
	r.und = appendCap(r.und, d)
}

func appendCap(s []float32, v float32) []float32 {
	s = append(s, v)
	if len(s) > statCap {
		s = s[len(s)-statCap:]
	}
	return s
}

func lastOf(s []float32) float32 {
	if len(s) == 0 {
		return 0
	}
	return s[len(s)-1]
}

func maxOf(s []float32, floor float32) float32 {
	m := floor
	for _, v := range s {
		if v > m {
			m = v
		}
	}
	return m
}

// graphCard draws the three history sparklines (CPU %, heap МБ, underruns/с) — the "графики по клику".
func graphCard(gtx C, th *material.Theme, r *statRing) D {
	row := func(name, val string, samples []float32, lo, hi float32, col color.NRGBA) layout.FlexChild {
		return layout.Rigid(func(gtx C) D {
			return layout.Inset{Top: unit.Dp(3), Bottom: unit.Dp(3)}.Layout(gtx, func(gtx C) D {
				return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
					layout.Rigid(func(gtx C) D {
						return fixedW(gtx, gtx.Dp(120), func(gtx C) D {
							return layout.Flex{Axis: layout.Horizontal}.Layout(gtx,
								layout.Rigid(label(th, unit.Sp(12), name, colMuted).Layout),
								layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
								layout.Rigid(label(th, unit.Sp(12), val, colTxt).Layout),
							)
						})
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
					layout.Flexed(1, func(gtx C) D { return sparkline(gtx, samples, lo, hi, col) }),
				)
			})
		})
	}
	return card(gtx, colLine, colPanel, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			row("CPU", fmtPct(lastOf(r.cpu)), r.cpu, 0, 100, colAccentB),
			row("Heap МБ", fmtF1(lastOf(r.heap)), r.heap, 0, maxOf(r.heap, 1), colOk),
			row("Underruns/с", fmtInt(lastOf(r.und)), r.und, 0, maxOf(r.und, 1), colWarn),
		)
	})
}

func sparkline(gtx C, samples []float32, lo, hi float32, col color.NRGBA) D {
	w := gtx.Constraints.Max.X
	if w <= 0 {
		w = gtx.Dp(200)
	}
	h := gtx.Dp(34)
	sz := image.Pt(w, h)
	fillRRect(gtx.Ops, image.Rect(0, 0, w, h), gtx.Dp(4), colKnobBody)
	if len(samples) >= 2 && hi > lo {
		var p clip.Path
		p.Begin(gtx.Ops)
		n := len(samples)
		for i, v := range samples {
			x := float32(i) / float32(n-1) * float32(w)
			y := float32(h) - clamp01((v-lo)/(hi-lo))*float32(h)
			pt := f32.Pt(x, y)
			if i == 0 {
				p.MoveTo(pt)
			} else {
				p.LineTo(pt)
			}
		}
		paint.FillShape(gtx.Ops, col, clip.Stroke{Path: p.End(), Width: float32(gtx.Dp(1.5))}.Op())
	}
	return D{Size: sz}
}
