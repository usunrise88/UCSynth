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
		// 6 приёмников: — / Pitch / Cutoff / Res / Amp / Wave-поз. FX убран — он выбирался в GUI,
		// но DSP его не читал (эффекты глобальные, матрица пер-голосная), см. ModDest в voice.h.
		if f := For("mtx" + n + "_dst"); f.Block != "modmatrix" || len(f.EnumLabels) != 6 {
			t.Fatalf("mtx%s_dst → %+v, want modmatrix with 6 dest labels", n, f)
		}
		if f := For("mtx" + n + "_depth"); f.Block != "modmatrix" || f.EnumLabels != nil {
			t.Fatalf("mtx%s_depth → %+v, want modmatrix plain knob", n, f)
		}
	}
}

func TestWaveEnvMapped(t *testing.T) {
	// 8 точек wave-огибающей → блок waveenv, рендер вертикальным слайдером.
	for i := 1; i <= 8; i++ {
		name := "waveenv_p" + strconv.Itoa(i)
		if f := For(name); f.Block != "waveenv" {
			t.Fatalf("%s → %+v, want waveenv", name, f)
		}
		if !IsEnvSlider(name) {
			t.Fatalf("%s should render as vertical slider", name)
		}
	}
	if f := For("waveenv_rate"); f.Block != "waveenv" || f.Unit != "с" {
		t.Fatalf("waveenv_rate → %+v, want waveenv/с", f)
	}
	if f := For("waveenv_loop"); f.Block != "waveenv" {
		t.Fatalf("waveenv_loop → %+v, want waveenv", f)
	}
	if IsEnvSlider("waveenv_rate") {
		t.Fatal("waveenv_rate should be a knob, not a slider")
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

func TestStage12OscTypesAndEngine(t *testing.T) {
	// тип осц-слота — в блоке своего осц, с 3 подписями (Wavetable/VA/Phase Dist)
	for i, blk := range []string{"osc1", "osc2", "osc3"} {
		name := "osc" + strconv.Itoa(i+1) + "_type"
		if f := For(name); f.Block != blk || len(f.EnumLabels) != 3 {
			t.Fatalf("%s → %+v, want %s with 3 type labels", name, f, blk)
		}
	}
	// движок голоса — блок engine, 3 подписи (Classic/FM/Karplus)
	if f := For("voice_engine"); f.Block != "engine" || len(f.EnumLabels) != 3 {
		t.Fatalf("voice_engine → %+v, want engine with 3 labels", f)
	}
	if For("voice_engine").EnumLabel(2) != "Karplus" {
		t.Fatalf("engine enum 2 = %q, want Karplus", For("voice_engine").EnumLabel(2))
	}
	// FM/KS/PD-скаляры — блок engine, обычные кнобы
	for _, name := range []string{"pd_amount", "fm_ratio", "fm_index", "ks_damp", "ks_decay", "ks_pluck"} {
		if f := For(name); f.Block != "engine" || f.EnumLabels != nil {
			t.Fatalf("%s → %+v, want engine plain knob", name, f)
		}
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
