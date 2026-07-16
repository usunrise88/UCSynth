package layout

import "testing"

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
	f := For("lfo1_rate") // a param the firmware might add later
	if f.Block != "misc" || f.Label != "lfo1_rate" {
		t.Fatalf("unknown param → %+v, want misc/raw-name", f)
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
