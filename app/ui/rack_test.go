package ui

import (
	"image"
	"testing"

	"gioui.org/io/input"
	"gioui.org/layout"
	"gioui.org/op"

	"ucsynth/app/proto"
)

// representative params: one of every kind, across several blocks, plus an unknown one (→ Прочее).
var smokeParams = []proto.Param{
	{ID: 0, Name: "master_volume", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0.8},
	{ID: 1, Name: "poly_voices", Type: proto.TypeInt, Min: 1, Max: 8, Cur: 8},
	{ID: 2, Name: "legato", Type: proto.TypeBool, Min: 0, Max: 1, Cur: 0},
	{ID: 3, Name: "glide_time", Type: proto.TypeFloat, Min: 0, Max: 2, Cur: 0.2},
	{ID: 4, Name: "waveform", Type: proto.TypeEnum, Min: 0, Max: 3, Cur: 1},
	{ID: 5, Name: "osc1_level", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0.8},
	{ID: 6, Name: "osc1_detune", Type: proto.TypeFloat, Min: -24, Max: 24, Cur: 0},
	{ID: 7, Name: "cutoff", Type: proto.TypeFloat, Min: 20, Max: 20000, Cur: 2400},
	{ID: 8, Name: "filter_mode", Type: proto.TypeEnum, Min: 0, Max: 3, Cur: 0},
	{ID: 9, Name: "flt_env_amt", Type: proto.TypeFloat, Min: -1, Max: 1, Cur: 0.4},
	{ID: 10, Name: "amp_attack", Type: proto.TypeFloat, Min: 0, Max: 2, Cur: 0.1},
	{ID: 11, Name: "amp_sustain", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0.7},
	{ID: 12, Name: "latch", Type: proto.TypeBool, Min: 0, Max: 1, Cur: 0},
	{ID: 13, Name: "lofi_bits", Type: proto.TypeInt, Min: 1, Max: 16, Cur: 12},
	{ID: 14, Name: "future_param", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0.5}, // unknown → Прочее
	// этап 4: modulation blocks (LFO, wave-env, mod-matrix) — exercise their render paths
	{ID: 15, Name: "lfo1_shape", Type: proto.TypeEnum, Min: 0, Max: 4, Cur: 0},
	{ID: 16, Name: "lfo1_rate", Type: proto.TypeFloat, Min: 0.05, Max: 30, Cur: 2},
	{ID: 17, Name: "waveenv_p1", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0},
	{ID: 18, Name: "waveenv_p2", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0.5},
	{ID: 19, Name: "waveenv_rate", Type: proto.TypeFloat, Min: 0.05, Max: 20, Cur: 1},
	{ID: 20, Name: "waveenv_loop", Type: proto.TypeBool, Min: 0, Max: 1, Cur: 1},
	{ID: 21, Name: "mod_wheel", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0},
	{ID: 22, Name: "mtx1_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 1},
	{ID: 23, Name: "mtx1_dst", Type: proto.TypeEnum, Min: 0, Max: 6, Cur: 2},
	{ID: 24, Name: "mtx1_depth", Type: proto.TypeFloat, Min: -1, Max: 1, Cur: 0.5},
	{ID: 25, Name: "mtx2_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 0},
	{ID: 26, Name: "mtx2_dst", Type: proto.TypeEnum, Min: 0, Max: 6, Cur: 0},
	{ID: 27, Name: "mtx2_depth", Type: proto.TypeFloat, Min: -1, Max: 1, Cur: 0},
}

// TestUnlistedBlocksCatchAll pins the rack catch-all: a block with controls that isn't in rackCols
// must surface (so it lands in the last column), never be silently dropped.
func TestUnlistedBlocksCatchAll(t *testing.T) {
	// all-listed blocks → no extras
	listedOnly := map[string][]*control{"osc1": {nil}, "filter": {nil}, "modmatrix": {nil}, "reverb": {nil}}
	if got := unlistedBlocks(listedOnly); len(got) != 0 {
		t.Fatalf("all-listed → extras %v, want none", got)
	}
	// an unlisted block → returned (sorted), so rack() appends it rather than dropping it
	mixed := map[string][]*control{"filter": {nil}, "zzz_future": {nil}, "aaa_future": {nil}}
	got := unlistedBlocks(mixed)
	if len(got) != 2 || got[0] != "aaa_future" || got[1] != "zzz_future" {
		t.Fatalf("unlisted blocks → %v, want [aaa_future zzz_future]", got)
	}
}

// TestRackLayoutSmoke lays out the panel rack with every control kind — a headless guard against
// panics / nil-derefs, since the visual result can't be observed in CI.
func TestRackLayoutSmoke(t *testing.T) {
	c := New(func() {})
	for _, p := range smokeParams {
		c.controls = append(c.controls, newControl(p))
	}
	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	for i := 0; i < 3; i++ {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(1100, 520))
		d := c.rack(gtx)
		r.Frame(gtx.Ops)
		if d.Size.X == 0 {
			t.Fatal("rack produced zero width")
		}
	}
}

// TestControllerLayoutSmoke lays out the whole controller (disconnected) — top bar, empty-rack
// message, keyboard, footer — to catch panics in those paths.
func TestControllerLayoutSmoke(t *testing.T) {
	c := New(func() {})
	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	for i := 0; i < 3; i++ {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(1100, 760))
		c.Layout(gtx)
		r.Frame(gtx.Ops)
	}
}
