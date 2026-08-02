package ui

import (
	"image"
	"net"
	"testing"
	"time"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"

	"ucsynth/app/device"
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

// TestPianoRollClickToggles clicks a grid cell on a connected editor and checks the note both lands in
// the local working copy and is uploaded to the device (SeqSetStep → SeqGet round-trip).
func TestPianoRollClickToggles(t *testing.T) {
	c := New(func() {})
	c1, c2 := net.Pipe()
	fake := device.NewFake(c2, smokeParams, proto.Stat{})
	go fake.Run()
	dev := device.New(c1, nil)
	dev.Start()
	defer dev.Close()
	c.dev = dev
	c.sink.set(dev)

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
	step, pitch := 2, c.seqHi // top row
	x := g.x0 + step*g.cellW + g.cellW/2
	y := g.cellH / 2 // row 0 = highest pitch
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(float32(x), float32(y)), PointerID: 1})
	frame() // decodes the click → toggles the cell + uploads the step

	if !c.noteOn(step, pitch) {
		t.Fatalf("click at step %d, pitch %d did not toggle the working copy", step, pitch)
	}
	// The step must reach the device: read it back and confirm the pitch is stored there.
	dev.SeqGet()
	deadline := time.Now().Add(2 * time.Second)
	uploaded := func() bool {
		st := dev.Snapshot().SeqPattern[step]
		for _, n := range st.Notes {
			if int(n) == pitch {
				return true
			}
		}
		return false
	}
	for time.Now().Before(deadline) && !uploaded() {
		time.Sleep(5 * time.Millisecond)
	}
	if !uploaded() {
		t.Fatalf("clicked note (step %d, pitch %d) was not uploaded to the device", step, pitch)
	}
}

// TestResetPatch checks the «Сброс» action drives every parameter back to its firmware default via the
// device (SET id→Def, echoed back), moving changed params home.
func TestResetPatch(t *testing.T) {
	c := New(func() {})
	params := []proto.Param{
		{ID: 0, Name: "master_volume", Type: proto.TypeFloat, Min: 0, Max: 1, Def: 0.8, Cur: 0.8},
		{ID: 1, Name: "cutoff", Type: proto.TypeFloat, Min: 20, Max: 20000, Def: 2400, Cur: 2400},
	}
	c1, c2 := net.Pipe()
	fake := device.NewFake(c2, params, proto.Stat{})
	go fake.Run()
	dev := device.New(c1, nil)
	dev.Start()
	defer dev.Close()
	c.dev = dev

	deadline := time.Now().Add(2 * time.Second)
	wait := func(ok func() bool, what string) {
		for time.Now().Before(deadline) {
			if ok() {
				return
			}
			time.Sleep(5 * time.Millisecond)
		}
		t.Fatalf("timeout waiting for %s", what)
	}
	wait(func() bool { return dev.Snapshot().State == device.Synced }, "sync")

	dev.SetParam(0, 0.2) // move both params off their defaults
	dev.SetParam(1, 500)
	wait(func() bool {
		v0, _ := dev.Snapshot().Param(0)
		v1, _ := dev.Snapshot().Param(1)
		return v0.Cur == 0.2 && v1.Cur == 500
	}, "params changed")

	c.resetPatch(dev.Snapshot())
	wait(func() bool {
		v0, _ := dev.Snapshot().Param(0)
		v1, _ := dev.Snapshot().Param(1)
		return v0.Cur == 0.8 && v1.Cur == 2400
	}, "params reset to default")
}

// TestSeqTabPatternBrowser wires a connected fake device into the seq tab and checks the on-device
// pattern directory is auto-listed on connect and rendered (handleSeqBrowser / layoutSeqTree).
func TestSeqTabPatternBrowser(t *testing.T) {
	c := New(func() {})
	c1, c2 := net.Pipe()
	fake := device.NewFake(c2, smokeParams, proto.Stat{})
	go fake.Run()
	dev := device.New(c1, nil)
	dev.Start()
	defer dev.Close()
	c.dev = dev
	c.sink.set(dev)

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(1100, 760))
		c.tab = tabSeq
		c.Layout(gtx) // handleButtons → handleSeq (auto GET+LIST); layoutPianoRoll → layoutSeqTree
		r.Frame(gtx.Ops)
	}
	deadline := time.Now().Add(2 * time.Second)
	synced := func() bool { return dev.Snapshot().State == device.Synced }
	for time.Now().Before(deadline) && !synced() {
		frame()
		time.Sleep(5 * time.Millisecond)
	}
	if !synced() {
		t.Fatal("device never synced")
	}

	dev.SeqSave(proto.PresetSlotNew, "Beats/One")
	got := false
	for time.Now().Before(deadline) {
		frame()
		if pats := dev.Snapshot().Patterns; len(pats) == 1 && pats[0].Path == "Beats/One" && len(c.seqEntryBtns) == 1 {
			got = true
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if !got {
		t.Fatalf("saved pattern not reflected in the sequencer tab (patterns=%v, btns=%d)",
			dev.Snapshot().Patterns, len(c.seqEntryBtns))
	}
}
