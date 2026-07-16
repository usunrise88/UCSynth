package ui

import (
	"image"
	"testing"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
)

// TestKnobDrag verifies a vertical drag changes the value AND keeps tracking after the cursor leaves
// the knob rect — i.e. the pointer.GrabCmd works (drag from y=25 inside to y=-10 outside the 50px box).
func TestKnobDrag(t *testing.T) {
	var r input.Router
	var k Knob
	k.Value = 0.5
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(50, 50))
		k.Layout(gtx)
		r.Frame(gtx.Ops)
	}
	frame() // establish input area
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(25, 25), PointerID: 1})
	frame() // press delivered → grab requested
	// A Move while pressed is delivered to the knob as a Drag (Gio synthesises it); the point is
	// outside the 50px box, so this only reaches the knob because the grab captured the pointer.
	r.Queue(pointer.Event{Kind: pointer.Move, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(25, -10), PointerID: 1})
	frame() // drag delivered

	if k.Value <= 0.5 {
		t.Fatalf("drag up should raise value above 0.5, got %v (grab not working?)", k.Value)
	}
	if !k.Changed() {
		t.Fatal("knob should report Changed after a drag")
	}
}
