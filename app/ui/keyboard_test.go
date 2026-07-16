package ui

import (
	"image"
	"testing"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/unit"
	"gioui.org/widget"
)

// metric with 1px == 1dp so gtx.Dp() is deterministic in headless tests.
var testMetric = unit.Metric{PxPerDp: 1, PxPerSp: 1}

// TestKeyboardDoesNotShadowButtonClicks is the regression for the reported bug: the port/connect/
// Panic buttons stopped responding to clicks. Cause: Keyboard.handleKeys registered an event.Op
// with no clip, so it attached to the root (unbounded) input area. Recorded last (the keyboard is
// the final child), it was hit first in the router's back-to-front walk for *any* pointer position
// and, being non-pass, jumped straight to the root area — skipping every widget above it. Here a
// button sits above the keyboard exactly like the connection bar; the click must reach it.
func TestKeyboardDoesNotShadowButtonClicks(t *testing.T) {
	var r input.Router
	var btn widget.Clickable
	kb := NewKeyboard(func(uint8, uint8) {}, func(uint8) {})
	th := testTheme()

	const W, H, btnH = 400, 300, 40
	gtx := layout.Context{
		Ops:         new(op.Ops),
		Metric:      testMetric,
		Source:      r.Source(),
		Constraints: layout.Exact(image.Pt(W, H)),
	}
	build := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(W, H))
		layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				return btn.Layout(gtx, func(gtx C) D {
					return D{Size: image.Pt(gtx.Constraints.Max.X, btnH)}
				})
			}),
			layout.Flexed(1, func(gtx C) D { return kb.Layout(gtx, th) }),
		)
	}

	// Frame 1: establish input areas for the router to hit-test against.
	build()
	r.Frame(gtx.Ops)

	// A primary click squarely on the top button — a point that also lies inside the keyboard's
	// former unbounded area.
	pos := f32.Pt(W/2, btnH/2)
	r.Queue(
		pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: pos, PointerID: 1},
		pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: pos, PointerID: 1},
	)

	if !btn.Clicked(gtx) {
		t.Fatal("button above the on-screen keyboard did not receive its click — keyboard input area is shadowing the widgets above it")
	}
}

// TestKeyboardMouseNoteFires guards the other side: bounding the keyboard's area must not break the
// keyboard's own mouse input. A press on the leftmost white key must fire note-on for the base note.
func TestKeyboardMouseNoteFires(t *testing.T) {
	var r input.Router
	gotNote := -1
	kb := NewKeyboard(func(n, _ uint8) { gotNote = int(n) }, func(uint8) {})
	th := testTheme()

	const W, H = 400, 300
	gtx := layout.Context{
		Ops:         new(op.Ops),
		Metric:      testMetric,
		Source:      r.Source(),
		Constraints: layout.Exact(image.Pt(W, H)),
	}
	build := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(W, H))
		kb.Layout(gtx, th)
	}

	// Frame 1: establish the key areas.
	build()
	r.Frame(gtx.Ops)

	// Press (held, no release) inside the leftmost white key. With 2 octaves, ww = 400/14 ≈ 28;
	// the first key spans x∈[1, ww-2]. Height is Dp(150) = 150px, so y=60 is well inside.
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(10, 60), PointerID: 1})

	// Frame 2: the keyboard polls Pressed() during Layout and should fire note-on for the base note.
	build()
	r.Frame(gtx.Ops)

	if gotNote != kb.base {
		t.Fatalf("mouse press on the leftmost key did not fire note-on for the base note %d: got %d", kb.base, gotNote)
	}
}
