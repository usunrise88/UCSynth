package ui

import (
	"strconv"
	"strings"

	"gioui.org/layout"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"
)

// The mod-matrix panel (этап 4.3): a compact per-slot view — one row each of «‹src› → ‹dst› [depth]» —
// instead of the generic 16 pill-rows + 8 detached knobs. Same LIST-driven params underneath (so it
// still saves in patches); only the render differs. Slot enums use the spare dec/inc clickables of
// each control as ‹/› carets (bidirectional cycle with wrap); depth reuses the control's knob.

// parseMtxSlot splits "mtx<N>_<kind>" → (N, kind). Non-matrix names (e.g. mod_wheel) → (0, "").
func parseMtxSlot(name string) (int, string) {
	if !strings.HasPrefix(name, "mtx") {
		return 0, ""
	}
	rest := name[3:] // "1_src"
	us := strings.IndexByte(rest, '_')
	if us < 1 {
		return 0, ""
	}
	n, err := strconv.Atoi(rest[:us])
	if err != nil {
		return 0, ""
	}
	return n, rest[us+1:]
}

// matrixPanel renders the mod-matrix block: any standalone knobs (mod-wheel) on top, then 8 slot rows.
func (c *Controller) matrixPanel(gtx C, cs []*control) D {
	type slot struct{ src, dst, depth *control }
	var slots [9]slot // 1..8
	var knobs []*control
	var extra []*control // anything this panel's name pattern doesn't recognise
	for _, ct := range cs {
		n, kind := parseMtxSlot(ct.p.Name)
		if n >= 1 && n <= 8 && (kind == "src" || kind == "dst" || kind == "depth") {
			switch kind {
			case "src":
				slots[n].src = ct
			case "dst":
				slots[n].dst = ct
			case "depth":
				slots[n].depth = ct
			}
			continue
		}
		if ct.kind == kindKnob { // mod-wheel (manual source)
			knobs = append(knobs, ct)
			continue
		}
		// This is the one block with a hand-written renderer, so it is also the one place where a
		// new firmware parameter could vanish from the UI entirely — while still being saved and
		// loaded by patches, which walk the snapshot rather than the UI. That is the invariant
		// unlistedBlocks/layout.For→misc and TestUnlistedBlocksCatchAll exist to protect, so
		// anything unrecognised (a 9th+ slot, a new enum in this block) falls through to the
		// generic renderer instead of being dropped.
		extra = append(extra, ct)
	}

	var rows []layout.FlexChild
	first := true
	add := func(w layout.Widget) {
		if !first {
			rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(9)}.Layout))
		}
		first = false
		rows = append(rows, layout.Rigid(w))
	}
	if len(knobs) > 0 {
		add(func(gtx C) D { return c.cellRow(gtx, knobs, false, c.setParam) })
	}
	for i := 1; i <= 8; i++ {
		s := slots[i]
		if s.src == nil || s.dst == nil || s.depth == nil {
			// An incomplete slot still has real parameters behind it — render them generically
			// rather than dropping them.
			for _, ct := range []*control{s.src, s.dst, s.depth} {
				if ct != nil {
					extra = append(extra, ct)
				}
			}
			continue
		}
		i, s := i, s
		add(func(gtx C) D { return c.matrixRow(gtx, i, s.src, s.dst, s.depth) })
	}
	if len(extra) > 0 {
		add(func(gtx C) D { return c.cellRow(gtx, extra, false, c.setParam) })
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
}

// matrixRow lays out one slot: [n]  ‹src› → ‹dst›  [depth].
func (c *Controller) matrixRow(gtx C, n int, src, dst, depth *control) D {
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return fixedW(gtx, gtx.Dp(14), label(c.th, unit.Sp(11), strconv.Itoa(n), colFaint).Layout)
		}),
		layout.Rigid(func(gtx C) D { return src.enumCycle(gtx, c.th, c.setParam) }),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(3), Right: unit.Dp(3)}.Layout(gtx,
				label(c.th, unit.Sp(12), "→", colFaint).Layout)
		}),
		layout.Rigid(func(gtx C) D { return dst.enumCycle(gtx, c.th, c.setParam) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
		layout.Rigid(func(gtx C) D { return depth.depthCell(gtx, c.th, c.setParam) }),
	)
}

// enumCycle renders an enum as «‹ label ›» with bidirectional wrap-around cycling. Reuses the
// control's dec/inc clickables (unused for enums otherwise). NONE (index 0) shows muted.
func (c *control) enumCycle(gtx C, th *material.Theme, set setFunc) D {
	cur := int(c.value() + 0.5) // optimistic: D-014 expects several clicks in a row through 8 sources
	n := int(c.p.Max-c.p.Min) + 1
	if n < 1 {
		n = 1
	}
	if c.dec.Clicked(gtx) { // read before Layout (Clickable.Layout drains clicks)
		cur = (cur - 1 + n) % n
		set(c.p.ID, float32(cur))
		c.commit(float32(cur))
	}
	if c.inc.Clicked(gtx) {
		cur = (cur + 1) % n
		set(c.p.ID, float32(cur))
		c.commit(float32(cur))
	}
	caret := func(b *widget.Clickable, s string) layout.Widget {
		return func(gtx C) D {
			return b.Layout(gtx, func(gtx C) D {
				return pill{border: colLine2, text: colMuted, size: unit.Sp(12), padX: 5, padY: 3, radius: 5}.draw(gtx, th, s)
			})
		}
	}
	lblCol := colMuted
	if cur != 0 {
		lblCol = colAccentB
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(caret(&c.dec, "‹")),
		layout.Rigid(func(gtx C) D {
			return fixedW(gtx, gtx.Dp(52), func(gtx C) D {
				return layout.Center.Layout(gtx, label(th, unit.Sp(11), c.fld.EnumLabel(cur), lblCol).Layout)
			})
		}),
		layout.Rigid(caret(&c.inc, "›")),
	)
}

// depthCell is a compact bipolar knob + value (no caption — the row already names the slot).
func (c *control) depthCell(gtx C, th *material.Theme, set setFunc) D {
	min, max := c.p.Min, c.p.Max
	span := max - min
	if !c.knob.Dragging() && span != 0 {
		c.knob.Value = clamp01((c.p.Cur - min) / span)
	}
	shown := min + c.knob.Value*span // pre-layout value, for the caption (one frame behind, cosmetic)
	dims := fixedW(gtx, gtx.Dp(50), func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical, Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(c.knob.Layout),
			layout.Rigid(label(th, unit.Sp(10), fmtVal(shown, ""), colTxt).Layout),
		)
	})
	// Recompute AFTER the layout: c.knob.Layout is what processes the pointer events and updates
	// knob.Value, so sending the pre-layout value drops the drag delta that triggered Changed() —
	// a quick flick within one frame changed nothing at all. knobCell/sliderCell already do this.
	if c.knob.Changed() {
		set(c.p.ID, min+c.knob.Value*span)
	}
	return dims
}
