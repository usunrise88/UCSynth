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

func TestParseMtxSlot(t *testing.T) {
	cases := []struct {
		name string
		n    int
		kind string
	}{
		{"mtx1_src", 1, "src"},
		{"mtx8_depth", 8, "depth"},
		{"mtx3_dst", 3, "dst"},
		{"mod_wheel", 0, ""},   // not a slot
		{"lfo1_rate", 0, ""},   // "mtx" prefix absent
		{"mtx", 0, ""},         // no "_"
		{"mtxX_src", 0, ""},    // non-numeric slot
		{"master_volume", 0, ""},
	}
	for _, c := range cases {
		n, kind := parseMtxSlot(c.name)
		if n != c.n || kind != c.kind {
			t.Fatalf("parseMtxSlot(%q) = (%d,%q), want (%d,%q)", c.name, n, kind, c.n, c.kind)
		}
	}
}

// TestEnumCycleNext clicks the "›" caret and expects the source to advance (with wrap-around),
// headless via input.Router — guards the click→cycle wiring the way the piano-roll test does.
func TestEnumCycleNext(t *testing.T) {
	ctl := newControl(proto.Param{ID: 42, Name: "mtx1_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 1})
	th := material.NewTheme()

	var gotID uint16
	var gotVal float32
	var called bool
	set := func(id uint16, v float32) { gotID, gotVal, called = id, v, true }

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	render := func() D {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Constraints{Max: image.Pt(200, 40)} // loose → flex sizes to content
		d := ctl.enumCycle(gtx, th, set)
		r.Frame(gtx.Ops)
		return d
	}

	d := render() // establish input areas
	x, y := d.Size.X-6, d.Size.Y/2 // "›" is the rightmost element
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(float32(x), float32(y)), PointerID: 1})
	r.Queue(pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(float32(x), float32(y)), PointerID: 1})
	render() // decode the click

	if !called {
		t.Fatal("next caret click produced no set")
	}
	if gotID != 42 || gotVal != 2 { // cur 1 → 2
		t.Fatalf("cycle next: id=%d val=%v, want 42/2", gotID, gotVal)
	}
}
