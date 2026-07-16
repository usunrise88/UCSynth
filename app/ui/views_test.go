package ui

import (
	"image"
	"testing"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"

	"ucsynth/app/proto"
)

func TestStatRing(t *testing.T) {
	var r statRing
	// underruns is monotonic → stored as per-sample delta; heap → MB; cpu → %.
	r.push(proto.Stat{CPUPermille: 125, Heap: 8 << 20, Underruns: 0})
	r.push(proto.Stat{CPUPermille: 200, Heap: 8 << 20, Underruns: 3})
	if got := r.cpu[len(r.cpu)-1]; got != 20 {
		t.Fatalf("cpu last = %v, want 20", got)
	}
	if got := r.und[len(r.und)-1]; got != 3 {
		t.Fatalf("underrun delta = %v, want 3", got)
	}
	// cap: push many, expect trim to statCap
	for i := 0; i < statCap+50; i++ {
		r.push(proto.Stat{Underruns: uint32(i)})
	}
	if len(r.cpu) != statCap {
		t.Fatalf("ring not capped: len=%d want %d", len(r.cpu), statCap)
	}
}

// TestControllerTabsSmoke lays out every tab (disconnected) + the graph overlay — a panic guard for
// the top bar, tabs, sequencer grid, patch view, MIDI row, and keyboard.
func TestControllerTabsSmoke(t *testing.T) {
	c := New(func() {})
	c.showGraphs = true
	c.hist.push(proto.Stat{CPUPermille: 100, Heap: 8 << 20, Underruns: 1})
	c.hist.push(proto.Stat{CPUPermille: 150, Heap: 8 << 20, Underruns: 1})
	c.enumMidi() // no devices off Windows → empty, must not panic

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	for tab := 0; tab < 3; tab++ {
		c.tab = tab
		for i := 0; i < 3; i++ {
			gtx.Reset()
			gtx.Metric = testMetric
			gtx.Constraints = layout.Exact(image.Pt(1100, 760))
			c.Layout(gtx)
			r.Frame(gtx.Ops)
		}
	}
}

func TestPianoRollClickToggles(t *testing.T) {
	c := New(func() {})
	var r input.Router
	const W, H = 800, 400
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(W, H))
		c.rollGrid(gtx)
		r.Frame(gtx.Ops)
	}
	frame() // establishes rollGeom + input area
	g := c.rollGeom
	if g.cellW == 0 || g.cellH == 0 {
		t.Fatal("grid geometry not set")
	}
	step, pitch := 2, c.player.Hi() // top row
	x := g.x0 + step*g.cellW + g.cellW/2
	y := g.cellH / 2 // row 0 = highest pitch
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(float32(x), float32(y)), PointerID: 1})
	frame() // decodes the click → toggles the cell

	if !c.player.On(step, pitch) {
		t.Fatalf("click at step %d, pitch %d did not toggle the cell", step, pitch)
	}
}
