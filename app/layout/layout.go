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
	{"delay", "Delay"},
	{"reverb", "Reverb"},
	{"lofi", "Lo-fi"},
	{"seq", "Секвенсор"},
	{"arp", "Арпеджиатор"},
	{"engine", "Движок"},
	{"fm", "FM (2-оп)"},
	{"ks", "Karplus"},
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

// FX убран: эффекты считаются один раз после суммы голосов, а матрица пер-голосная — приёмник
// существовал в GUI, но DSP его не читал, и слот тратился молча (см. ModDest в voice.h).
var modDstLabels = []string{"—", "Pitch", "Cutoff", "Res", "Amp", "Wave-поз."}

// этап 7 — подписи enum-контролов арпеджиатора (порядок = ArpMode / деления клока в seq_engine.h).
var arpModeLabels = []string{"Вверх", "Вниз", "Вверх-вниз", "Случайно"}
var arpRateLabels = []string{"1/4", "1/8", "1/16", "1/32"}

// этап 12 — тип осц-слота (OscType) и движок голоса (VoiceEngine) из voice.h.
var oscTypeLabels = []string{"Wavetable", "VA", "Phase Dist"}
var engineLabels = []string{"Classic", "FM", "Karplus"}

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
	"osc1_type":   {"osc1", "Тип", "", oscTypeLabels},
	// osc2 / osc3
	"osc2_wave":   {"osc2", "Форма", "", waveLabels},
	"osc2_level":  {"osc2", "Уровень", "", nil},
	"osc2_detune": {"osc2", "Детюн", "полут.", nil},
	"osc2_type":   {"osc2", "Тип", "", oscTypeLabels},
	"osc3_wave":   {"osc3", "Форма", "", waveLabels},
	"osc3_level":  {"osc3", "Уровень", "", nil},
	"osc3_detune": {"osc3", "Детюн", "полут.", nil},
	"osc3_type":   {"osc3", "Тип", "", oscTypeLabels},
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
	// этап 5.2 — delay (стерео)
	"delay_on":       {"delay", "Вкл", "", nil},
	"delay_time":     {"delay", "Время", "мс", nil},
	"delay_feedback": {"delay", "Feedback", "", nil},
	"delay_damp":     {"delay", "Damp", "", nil},
	"delay_mix":      {"delay", "Mix", "", nil},
	// этап 5.3 — reverb (Freeverb)
	"reverb_on":    {"reverb", "Вкл", "", nil},
	"reverb_size":  {"reverb", "Size", "", nil},
	"reverb_damp":  {"reverb", "Damp", "", nil},
	"reverb_width": {"reverb", "Width", "", nil},
	"reverb_mix":   {"reverb", "Mix", "", nil},
	// этап 6 — модуляция длины гребёнок (против звона)
	"reverb_moddepth": {"reverb", "Mod Depth", "", nil},
	"reverb_modrate":  {"reverb", "Mod Rate", "Гц", nil},
	// матрица (mtx1..8 × {src,dst,depth}) добавляется в init() ниже
	// этап 7 — секвенсор/арпеджиатор (движок на устройстве; сетку/транспорт рисует вкладка «Секвенсор»,
	// а эти скаляры появляются в рэке обычными контролами через LIST)
	"seq_bpm":     {"seq", "Темп", "BPM", nil},
	"seq_swing":   {"seq", "Swing", "", nil},
	"seq_playing": {"seq", "Играть", "", nil},
	"seq_on":      {"seq", "Секв. вкл", "", nil},
	"arp_on":      {"arp", "Вкл", "", nil},
	"arp_mode":    {"arp", "Режим", "", arpModeLabels},
	"arp_octaves": {"arp", "Октавы", "", nil},
	"arp_rate":    {"arp", "Скорость", "", arpRateLabels},
	"arp_hold":    {"arp", "Hold", "", nil},
	// этап 12 — движок голоса. Селектор — в блоке engine (рэк ставит его над осцилляторами). Параметры
	// FM/Karplus — в своих блоках (панели активны только для своего движка). pd_amount формально в engine,
	// но рэк вынимает его и показывает ВНУТРИ осц-панели, если у слота тип Phase Dist (см. rack/oscPanel).
	"voice_engine": {"engine", "Движок", "", engineLabels},
	"pd_amount":    {"engine", "PD глубина", "", nil},
	"fm_ratio":     {"fm", "Ratio", "", nil},
	"fm_index":     {"fm", "Index", "", nil},
	"ks_damp":      {"ks", "Damp", "", nil},
	"ks_decay":     {"ks", "Decay", "", nil},
	"ks_pluck":     {"ks", "Pluck", "", nil},
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
