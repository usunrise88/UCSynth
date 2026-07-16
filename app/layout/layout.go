// Package layout is the GUI-side static map from parameters to UI blocks, plus enum-label and
// unit strings — the firmware sends none of that on the wire (only name/type/range/cur), so the
// client supplies it. Unknown/new params fall into the "misc" block so they still appear, honoring
// the firmware's "a new param needs no client change" contract as far as possible.
package layout

import (
	"strconv"
	"strings"
)

// Block groups params in the UI. Display order is the slice order below.
type Block struct {
	Key   string
	Title string
}

// Blocks in display order. A param whose block key isn't here (only "misc" today) is appended last.
var Blocks = []Block{
	{"global", "Глобал"},
	{"osc1", "Осциллятор 1"},
	{"osc2", "Осциллятор 2"},
	{"osc3", "Осциллятор 3"},
	{"mixer", "Микшер"},
	{"filter", "Фильтр"},
	{"ampenv", "Огибающая VCA"},
	{"fltenv", "Огибающая VCF"},
	{"lofi", "Lo-fi"},
	{"debug", "Отладка"},
	{"misc", "Прочее"},
}

// Field is the UI presentation of one param: which block, a friendly label, a unit suffix, and
// (for enums) option labels. EnumLabels nil → render enum indices as bare numbers.
type Field struct {
	Block      string
	Label      string
	Unit       string
	EnumLabels []string
}

var waveLabels = []string{"Sine", "Saw", "Square", "Tri"}
var filterLabels = []string{"LP", "HP", "BP", "OFF"}

// byName maps a firmware param name → its presentation. Names come from control.h (stable).
var byName = map[string]Field{
	// global
	"master_volume": {"global", "Громкость", "", nil},
	"poly_voices":   {"global", "Голоса", "", nil},
	"glide_time":    {"global", "Glide", "с", nil},
	"legato":        {"global", "Legato", "", nil},
	// osc1 (its waveform is the legacy "waveform" param, not "osc1_wave")
	"waveform":    {"osc1", "Форма", "", waveLabels},
	"osc1_level":  {"osc1", "Уровень", "", nil},
	"osc1_detune": {"osc1", "Детюн", "полут.", nil},
	// osc2 / osc3
	"osc2_wave":   {"osc2", "Форма", "", waveLabels},
	"osc2_level":  {"osc2", "Уровень", "", nil},
	"osc2_detune": {"osc2", "Детюн", "полут.", nil},
	"osc3_wave":   {"osc3", "Форма", "", waveLabels},
	"osc3_level":  {"osc3", "Уровень", "", nil},
	"osc3_detune": {"osc3", "Детюн", "полут.", nil},
	// mixer
	"noise_level": {"mixer", "Шум", "", nil},
	"ring_level":  {"mixer", "Ring mod", "", nil},
	// filter (flt_env_amt lives here in the UI — it's the filter's Env→Cutoff knob)
	"cutoff":      {"filter", "Cutoff", "Гц", nil},
	"resonance":   {"filter", "Резонанс", "", nil},
	"filter_mode": {"filter", "Режим", "", filterLabels},
	"flt_env_amt": {"filter", "Env→Cut", "", nil},
	// VCA envelope
	"amp_attack":  {"ampenv", "Attack", "с", nil},
	"amp_decay":   {"ampenv", "Decay", "с", nil},
	"amp_sustain": {"ampenv", "Sustain", "", nil},
	"amp_release": {"ampenv", "Release", "с", nil},
	"latch":       {"ampenv", "Latch (дрон)", "", nil},
	"amp_loop":    {"ampenv", "Loop", "", nil},
	// VCF envelope
	"flt_attack":  {"fltenv", "Attack", "с", nil},
	"flt_decay":   {"fltenv", "Decay", "с", nil},
	"flt_sustain": {"fltenv", "Sustain", "", nil},
	"flt_release": {"fltenv", "Release", "с", nil},
	"flt_loop":    {"fltenv", "Loop", "", nil},
	// lo-fi
	"lofi":      {"lofi", "Lo-fi", "", nil},
	"lofi_bits": {"lofi", "Биты", "", nil},
	// debug
	"test_tone":    {"debug", "Тест-тон", "", nil},
	"test_tone_hz": {"debug", "Частота тона", "Гц", nil},
}

// For returns the presentation of a param by name. Unknown params go to the "misc" block with
// their raw name as the label — so firmware params added later still render.
func For(name string) Field {
	if f, ok := byName[name]; ok {
		return f
	}
	return Field{Block: "misc", Label: name}
}

// EnumLabel returns the label for an enum option index; out-of-range (or no labels) → the number,
// so a firmware-added enum option still shows (as a bare index) without a client change.
func (f Field) EnumLabel(i int) string {
	if i >= 0 && i < len(f.EnumLabels) {
		return f.EnumLabels[i]
	}
	return strconv.Itoa(i)
}

// IsEnvSlider reports whether a param should render as a vertical fader instead of a knob — the
// ADSR stages (…_attack/_decay/_sustain/_release), matching the reference's envelope sliders.
func IsEnvSlider(name string) bool {
	for _, suf := range []string{"_attack", "_decay", "_sustain", "_release"} {
		if strings.HasSuffix(name, suf) {
			return true
		}
	}
	return false
}

// BlockTitle returns the display title for a block key ("Прочее" for unknown keys).
func BlockTitle(key string) string {
	for _, b := range Blocks {
		if b.Key == key {
			return b.Title
		}
	}
	return "Прочее"
}
