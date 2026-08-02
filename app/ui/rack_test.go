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
	// этап 12 — типы осц + движки (тип-слота как ENUM-сегмент, движок-блок как кнобы)
	{ID: 28, Name: "osc1_type", Type: proto.TypeEnum, Min: 0, Max: 2, Cur: 1},
	{ID: 29, Name: "voice_engine", Type: proto.TypeEnum, Min: 0, Max: 2, Cur: 0},
	{ID: 30, Name: "fm_ratio", Type: proto.TypeFloat, Min: 0.5, Max: 8, Cur: 1},
	{ID: 31, Name: "ks_decay", Type: proto.TypeFloat, Min: 0.8, Max: 0.999, Cur: 0.99},
	{ID: 32, Name: "pd_amount", Type: proto.TypeFloat, Min: 0, Max: 1, Cur: 0},
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

// TestBlockEnabled pins which rack blocks are active per voice engine: осц/микшер только в Classic,
// FM/Karplus — свои блоки, всё остальное (фильтр/огибающие/…) активно всегда.
func TestBlockEnabled(t *testing.T) {
	cases := []struct {
		block  string
		engine int
		want   bool
	}{
		{"osc1", 0, true}, {"osc1", 1, false}, {"osc2", 2, false}, {"mixer", 0, true}, {"mixer", 1, false},
		{"fm", 1, true}, {"fm", 0, false}, {"fm", 2, false},
		{"ks", 2, true}, {"ks", 0, false}, {"ks", 1, false},
		{"filter", 0, true}, {"filter", 1, true}, {"ampenv", 2, true}, {"lfo1", 1, true},
	}
	for _, tc := range cases {
		if got := blockEnabled(tc.block, tc.engine); got != tc.want {
			t.Errorf("blockEnabled(%q, %d) = %v, want %v", tc.block, tc.engine, got, tc.want)
		}
	}
}

// TestSlotIsPD checks the oscillator-slot PD detection that drives the contextual pd_amount knob.
func TestSlotIsPD(t *testing.T) {
	mk := func(typeCur float32) []*control {
		return []*control{
			newControl(proto.Param{ID: 1, Name: "osc1_type", Type: proto.TypeEnum, Min: 0, Max: 2, Cur: typeCur}),
			newControl(proto.Param{ID: 2, Name: "osc1_level", Type: proto.TypeFloat, Max: 1, Cur: 1}),
		}
	}
	if slotIsPD(mk(0)) || slotIsPD(mk(1)) {
		t.Fatal("wavetable/VA slot must not report PD")
	}
	if !slotIsPD(mk(2)) {
		t.Fatal("PD-typed slot must report PD")
	}
}

// TestRackEngineModes renders the rack with each engine selected — the greying, oscPanel and scrim
// paths must lay out without panics for Classic/FM/Karplus.
func TestRackEngineModes(t *testing.T) {
	for engine := 0; engine <= 2; engine++ {
		c := New(func() {})
		for _, p := range smokeParams {
			c.controls = append(c.controls, newControl(p))
		}
		for _, ct := range c.controls {
			if ct.p.Name == "voice_engine" {
				ct.p.Cur = float32(engine)
			}
			if ct.p.Name == "osc1_type" {
				ct.p.Cur = 2 // PD → exercise the contextual pd_amount branch
			}
		}
		var r input.Router
		gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
		for i := 0; i < 2; i++ {
			gtx.Reset()
			gtx.Metric = testMetric
			gtx.Constraints = layout.Exact(image.Pt(1100, 520))
			if d := c.rack(gtx); d.Size.X == 0 {
				t.Fatalf("engine %d: rack produced zero width", engine)
			}
			r.Frame(gtx.Ops)
		}
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

// The mod-matrix is the one block with a hand-written renderer, so it is also the one place a new
// firmware parameter can vanish from the UI while patches keep saving and loading it (they walk the
// snapshot, not the UI). Anything the slot-name pattern doesn't recognise must still be rendered.
func TestMatrixPanelRendersUnrecognisedParams(t *testing.T) {
	c := New(func() {})
	cs := []*control{
		newControl(proto.Param{ID: 1, Name: "mtx1_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 0}),
		newControl(proto.Param{ID: 2, Name: "mtx1_dst", Type: proto.TypeEnum, Min: 0, Max: 5, Cur: 0}),
		newControl(proto.Param{ID: 3, Name: "mtx1_depth", Type: proto.TypeFloat, Min: -1, Max: 1, Cur: 0}),
		// A 9th slot and a brand-new enum: neither fits the [1..8]×{src,dst,depth} pattern.
		newControl(proto.Param{ID: 4, Name: "mtx9_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 0}),
		newControl(proto.Param{ID: 5, Name: "mod_wheel_curve", Type: proto.TypeEnum, Min: 0, Max: 2, Cur: 0}),
		// An incomplete slot (dst/depth missing) — its src is still a real parameter.
		newControl(proto.Param{ID: 6, Name: "mtx4_src", Type: proto.TypeEnum, Min: 0, Max: 7, Cur: 0}),
	}

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	render := func() D {
		gtx.Reset()
		gtx.Metric = testMetric
		// Loose constraints so the vertical Flex sizes to its content and the height is meaningful.
		gtx.Constraints = layout.Constraints{Max: image.Pt(420, 4000)}
		d := c.matrixPanel(gtx, cs)
		r.Frame(gtx.Ops)
		return d
	}
	full := render()
	render()

	// Now drop the unrecognised ones: the panel must get measurably shorter, proving they were
	// actually laid out rather than silently skipped.
	cs = cs[:3]
	trimmed := render()
	if full.Size.Y <= trimmed.Size.Y {
		t.Fatalf("unrecognised matrix params were not rendered: height with them %d, without %d",
			full.Size.Y, trimmed.Size.Y)
	}
}
