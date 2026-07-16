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
	{"lfo1", "LFO 1"},
	{"lfo2", "LFO 2"},
	{"waveenv", "Wave-огибающая"},
	{"modmatrix", "Мод-матрица"},
	{"overdrive", "Overdrive"},
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

// этап 4.1 — подписи форм LFO и источников/приёмников мод-матрицы (порядок = enum в прошивке:
// LfoShape, ModSource, ModDest в voice.h / control.h). Индекс вне диапазона → голое число (см. EnumLabel).
var lfoShapeLabels = []string{"Sine", "Tri", "Saw", "Sqr", "S&H"}
var modSrcLabels = []string{"—", "LFO1", "LFO2", "VCF-огиб.", "Wave-огиб.", "Velocity", "Mod-wheel", "ToF"}
var modDstLabels = []string{"—", "Pitch", "Cutoff", "Res", "Amp", "Wave-поз.", "FX"}

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
	// этап 4.1 — LFO×2 (глубина и маршрут — в мод-матрице)
	"lfo1_shape": {"lfo1", "Форма", "", lfoShapeLabels},
	"lfo1_rate":  {"lfo1", "Частота", "Гц", nil},
	"lfo2_shape": {"lfo2", "Форма", "", lfoShapeLabels},
	"lfo2_rate":  {"lfo2", "Частота", "Гц", nil},
	// mod-wheel — ручной источник модуляции (маршрут — в матрице)
	"mod_wheel": {"modmatrix", "Mod-wheel", "", nil},
	// этап 4.2 — wave-огибающая (8 точек-слайдеров + rate + loop; источник WAVE_ENV матрицы)
	"waveenv_p1":   {"waveenv", "1", "", nil},
	"waveenv_p2":   {"waveenv", "2", "", nil},
	"waveenv_p3":   {"waveenv", "3", "", nil},
	"waveenv_p4":   {"waveenv", "4", "", nil},
	"waveenv_p5":   {"waveenv", "5", "", nil},
	"waveenv_p6":   {"waveenv", "6", "", nil},
	"waveenv_p7":   {"waveenv", "7", "", nil},
	"waveenv_p8":   {"waveenv", "8", "", nil},
	"waveenv_rate": {"waveenv", "Rate", "с", nil},
	"waveenv_loop": {"waveenv", "Loop", "", nil},
	// этап 5.1 — overdrive
	"od_on":    {"overdrive", "Вкл", "", nil},
	"od_drive": {"overdrive", "Драйв", "", nil},
	"od_mix":   {"overdrive", "Mix", "", nil},
	// матрица (mtx1..8 × {src,dst,depth}) добавляется в init() ниже
	// debug
	"test_tone":    {"debug", "Тест-тон", "", nil},
	"test_tone_hz": {"debug", "Частота тона", "Гц", nil},
}

// init заполняет 8 слотов мод-матрицы (src/dst — enum с подписями, depth — знаковый кноб).
// Панель полирнётся в 4.3; пока — обычные контролы в блоке «Мод-матрица».
func init() {
	for s := 1; s <= 8; s++ {
		n := strconv.Itoa(s)
		byName["mtx"+n+"_src"] = Field{"modmatrix", n + ": ист.", "", modSrcLabels}
		byName["mtx"+n+"_dst"] = Field{"modmatrix", n + ": назн.", "", modDstLabels}
		byName["mtx"+n+"_depth"] = Field{"modmatrix", n + ": глуб.", "", nil}
	}
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
// ADSR stages (…_attack/_decay/_sustain/_release) and the 8 wave-envelope breakpoints
// (waveenv_p1..8), which side-by-side read as a small wave-shape editor.
func IsEnvSlider(name string) bool {
	if strings.HasPrefix(name, "waveenv_p") {
		return true
	}
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
