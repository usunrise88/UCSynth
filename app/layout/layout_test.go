package layout

import (
	"strconv"
	"testing"
)

func TestForKnown(t *testing.T) {
	if f := For("cutoff"); f.Block != "filter" || f.Unit != "Гц" {
		t.Fatalf("cutoff → %+v", f)
	}
	if f := For("waveform"); f.Block != "osc1" || len(f.EnumLabels) != 4 {
		t.Fatalf("waveform → %+v", f)
	}
	if f := For("amp_attack"); f.Block != "ampenv" || f.Unit != "с" {
		t.Fatalf("amp_attack → %+v", f)
	}
}

func TestForUnknownFallsToMisc(t *testing.T) {
	f := For("some_future_param") // a param the firmware might add later
	if f.Block != "misc" || f.Label != "some_future_param" {
		t.Fatalf("unknown param → %+v, want misc/raw-name", f)
	}
}

func TestMatrixAndLFOMapped(t *testing.T) {
	if f := For("lfo1_rate"); f.Block != "lfo1" || f.Unit != "Гц" {
		t.Fatalf("lfo1_rate → %+v, want lfo1/Гц", f)
	}
	if f := For("lfo2_shape"); f.Block != "lfo2" || len(f.EnumLabels) != 5 {
		t.Fatalf("lfo2_shape → %+v, want lfo2 with 5 shape labels", f)
	}
	// all 8 matrix slots present with src/dst enum labels and a plain depth knob
	for s := 1; s <= 8; s++ {
		n := strconv.Itoa(s)
		if f := For("mtx" + n + "_src"); f.Block != "modmatrix" || len(f.EnumLabels) != 8 {
			t.Fatalf("mtx%s_src → %+v, want modmatrix with 8 source labels", n, f)
		}
		if f := For("mtx" + n + "_dst"); f.Block != "modmatrix" || len(f.EnumLabels) != 7 {
			t.Fatalf("mtx%s_dst → %+v, want modmatrix with 7 dest labels", n, f)
		}
		if f := For("mtx" + n + "_depth"); f.Block != "modmatrix" || f.EnumLabels != nil {
			t.Fatalf("mtx%s_depth → %+v, want modmatrix plain knob", n, f)
		}
	}
}

func TestEnumLabel(t *testing.T) {
	f := For("waveform")
	if f.EnumLabel(1) != "Saw" {
		t.Fatalf("enum 1 = %q, want Saw", f.EnumLabel(1))
	}
	// out of range (e.g. firmware adds a 5th waveform) → bare number, no crash
	if f.EnumLabel(9) != "9" {
		t.Fatalf("enum 9 = %q, want 9", f.EnumLabel(9))
	}
	// non-enum param → number
	if For("cutoff").EnumLabel(3) != "3" {
		t.Fatalf("non-enum enum label should be numeric")
	}
}

func TestBlockTitle(t *testing.T) {
	if BlockTitle("filter") != "Фильтр" {
		t.Fatal("block title lookup failed")
	}
	if BlockTitle("nope") != "Прочее" {
		t.Fatal("unknown block should fall to Прочее")
	}
}
