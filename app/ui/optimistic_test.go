package ui

import (
	"image"
	"testing"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/widget/material"

	"ucsynth/app/proto"
)

// A discrete control steps from the last value it *sent*, not from the last value the device
// *echoed*. p.Cur only updates after the SET flush (≤25 ms at 40 Hz) plus a USB round trip, so at
// 60 fps consecutive clicks land inside that window; combined with the coalescer keeping one value
// per id, several clicks used to produce a single increment.
func TestOptimisticValueStepsFromLastSent(t *testing.T) {
	c := newControl(proto.Param{ID: 31, Name: "poly_voices", Type: proto.TypeInt, Min: 1, Max: 8, Cur: 1})

	if got := c.value(); got != 1 {
		t.Fatalf("with no local edit, value() must be the echo: got %v", got)
	}
	c.commit(2)
	if got := c.value(); got != 2 {
		t.Fatalf("after commit(2), value() must be 2 (echo has not arrived): got %v", got)
	}
	c.commit(3) // second click in the same window steps from 2, not from the stale echo
	if got := c.value(); got != 3 {
		t.Fatalf("after commit(3), value() must be 3: got %v", got)
	}

	c.p.Cur = 3 // echo catches up
	if got := c.value(); got != 3 {
		t.Fatalf("after the echo confirms 3, value() must be 3: got %v", got)
	}
	c.p.Cur = 5 // an external change now wins, since we have no pending edit
	if got := c.value(); got != 5 {
		t.Fatalf("after the local edit resolved, the device must win: got %v", got)
	}
}

// If the firmware clamps or quantizes to something else, its answer wins — the optimistic value must
// not stick around and fight the device forever.
func TestOptimisticValueYieldsToClamp(t *testing.T) {
	c := newControl(proto.Param{ID: 31, Name: "poly_voices", Type: proto.TypeInt, Min: 1, Max: 8, Cur: 8})
	c.commit(9) // out of range; the firmware will clamp to 8... and answer with something != optBase
	if got := c.value(); got != 9 {
		t.Fatalf("before the echo, value() must be our pending edit: got %v", got)
	}
	c.p.Cur = 7 // device says 7
	if got := c.value(); got != 7 {
		t.Fatalf("device answer must win over the pending edit: got %v", got)
	}
	if c.optValid {
		t.Fatal("pending edit should have been cleared once the device answered")
	}
}

// Same thing end-to-end through the matrix caret: two clicks with no echo in between advance by two.
// D-014 explicitly expects the user to click through eight sources, so this is the normal case.
func TestEnumCycleTwoClicksAdvanceTwice(t *testing.T) {
	ctl := newControl(proto.Param{ID: 42, Name: "mtx1_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 1})
	th := material.NewTheme()

	var sent []float32
	set := func(_ uint16, v float32) { sent = append(sent, v) }

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	render := func() D {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Constraints{Max: image.Pt(200, 40)}
		d := ctl.enumCycle(gtx, th, set)
		r.Frame(gtx.Ops)
		return d
	}

	d := render()
	x, y := float32(d.Size.X-6), float32(d.Size.Y/2) // "›" is the rightmost element
	click := func() {
		r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(x, y), PointerID: 1})
		r.Queue(pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(x, y), PointerID: 1})
		render()
	}

	click() // 1 → 2
	click() // 2 → 3, with p.Cur still 1 (no echo yet)

	if len(sent) != 2 {
		t.Fatalf("two caret clicks produced %d sets: %v", len(sent), sent)
	}
	if sent[0] != 2 || sent[1] != 3 {
		t.Fatalf("clicks sent %v, want [2 3] — the second stepped from a stale echo", sent)
	}
}

// depthCell must send the value the knob holds AFTER its Layout has processed the drag. Computing it
// before the layout drops the delta that triggered Changed(), so a quick flick changed nothing and a
// slower drag lagged by one frame and then snapped back.
func TestDepthCellSendsPostDragValue(t *testing.T) {
	ctl := newControl(proto.Param{ID: 7, Name: "mtx1_depth", Type: proto.TypeFloat, Min: -1, Max: 1, Cur: 0})
	th := material.NewTheme()

	var sent []float32
	set := func(_ uint16, v float32) { sent = append(sent, v) }

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Constraints{Max: image.Pt(200, 140)}
		ctl.depthCell(gtx, th, set)
		r.Frame(gtx.Ops)
	}

	frame() // establish the knob's input area (50×50 dp at the top of the cell)
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(25, 25), PointerID: 1})
	frame()
	r.Queue(pointer.Event{Kind: pointer.Move, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(25, -20), PointerID: 1})
	frame() // drag delivered → Changed() → set

	if len(sent) == 0 {
		t.Fatal("dragging the depth knob produced no set")
	}
	span := ctl.p.Max - ctl.p.Min
	want := ctl.p.Min + ctl.knob.Value*span
	if got := sent[len(sent)-1]; got != want {
		t.Fatalf("depthCell sent %v but the knob sits at %v — pre-layout value leaked", got, want)
	}
	if want == ctl.p.Min {
		t.Fatal("knob did not move; test setup is wrong")
	}
}
