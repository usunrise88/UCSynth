#include "voice.h"
#include "wavetable.h"
#include <cmath>

namespace {
constexpr float OCT = 6.0f;   // полный flt_env_amt×env = ±6 октав модуляции cutoff

// MIDI-нота → частота (12-TET, A4=69=440 Гц). exp2f/деление на 12.0f — всё float (FPU S3).
inline float note_to_hz(uint8_t note)
{
    return 440.0f * exp2f(((int)note - 69) * (1.0f / 12.0f));
}

// xorshift32 → шум [-1,1). Множитель 2^-31 (не 2147483648.0 — тот double).
inline float noise_next(uint32_t &s)
{
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (float)s * 4.6566129e-10f - 1.0f;
}
}  // namespace

void voice_init(Voice *v, uint32_t seed)
{
    v->phase[0] = v->phase[1] = v->phase[2] = 0.0f;
    env_reset(&v->env_amp);
    env_reset(&v->env_flt);
    waveenv_reset(&v->env_wave);
    filter_reset(&v->filt);
    v->cur_note = v->target_note = 69.0f;   // A4
    v->note       = 255;
    v->key_down   = false;
    v->latch_held = false;
    v->rng        = seed ? seed : 0x1234567u;
    v->amp_prev   = 0.0f;
    v->velocity   = 1.0f;                   // до первого note-on — полная (нейтрально для матрицы)
}

void voice_note_on(Voice *v, uint8_t note, uint8_t vel, bool latch, bool glide)
{
    v->velocity    = (float)vel * (1.0f / 127.0f);   // источник матрицы (этап 4); напрямую в VCA не идёт
    v->note        = note;
    v->target_note = (float)note;
    if (!glide) v->cur_note = (float)note;   // снап (свежий/idle голос или glide выкл)
    v->key_down    = true;
    if (latch) v->latch_held = true;         // защёлка дрона взводится по note-on
    env_trigger(&v->env_amp);                // ретригер ОБЕИХ огибающих
    env_trigger(&v->env_flt);
    waveenv_reset(&v->env_wave);             // wave-огибающая стартует заново на note-on
}

void voice_slide(Voice *v, uint8_t note, bool glide)
{
    v->note        = note;                   // legato: только цель высоты, без ретригера
    v->target_note = (float)note;
    if (!glide) v->cur_note = (float)note;
}

void voice_note_off(Voice *v, uint8_t note)
{
    if (note == v->note) v->key_down = false;   // гасим только текущую ноту
}

void voice_render(Voice *v, const VoiceParams *p, float sr, float *out, int n)
{
    if (!p->latch) v->latch_held = false;       // снятие параметра latch отпускает дрон
    const bool gate = v->key_down || v->latch_held;

    // control-rate: огибающие, скольжение высоты, коэфф. фильтра, инкременты фаз.
    const float dt    = (float)n / sr;
    const float env_a = env_tick(&v->env_amp, &p->amp_env, gate, dt);
    const float env_f = env_tick(&v->env_flt, &p->flt_env, gate, dt);
    const float env_w = waveenv_tick(&v->env_wave, &p->wave_env, dt);   // wave-огибающая (0..1)

    // Мод-матрица (control-rate, раз в блок): собрать источники [-1,1], сложить depth·src в приёмники.
    // Глобальные источники (LFO, mod-wheel) уже в p->mod_src; пер-голосные подставляем тут.
    float src[MOD_SRC_COUNT];
    for (int s = 0; s < MOD_SRC_COUNT; ++s) src[s] = p->mod_src[s];
    src[MOD_SRC_NONE]     = 0.0f;
    src[MOD_SRC_VCF_ENV]  = env_f;         // огибающая VCF (0..1)
    src[MOD_SRC_WAVE_ENV] = env_w;         // wave-огибающая (0..1)
    src[MOD_SRC_VELOCITY] = v->velocity;   // сила нажатия (0..1)
    float mod[MOD_DST_COUNT] = {0.0f};
    for (int s = 0; s < MOD_SLOTS; ++s) {
        const ModSlot &m = p->mtx[s];
        // Верхняя граница src/dst — страховка от OOB: кламп в control.cpp сейчас держит индексы в
        // диапазоне, но enum помечен «дописывать перед _COUNT» — если кто-то добавит источник/приёмник
        // и забудет поднять кламп-максимум, здесь не будет чтения/записи за пределами src[]/mod[].
        if (m.src != MOD_SRC_NONE && m.dst != MOD_DST_NONE &&
            m.src < MOD_SRC_COUNT && m.dst < MOD_DST_COUNT)
            mod[m.dst] += m.depth * src[m.src];   // все нули → тракт эквивалентен доматричному
    }

    // Glide: cur_note одним полюсом к target в ЛОГ-высоте (муз. скольжение), затем → Гц.
    if (p->glide_time < 1e-3f) {
        v->cur_note = v->target_note;
    } else {
        const float c = 1.0f - expf(-4.6f * dt / p->glide_time);   // ~99% за glide_time
        v->cur_note += (v->target_note - v->cur_note) * c;
        if (fabsf(v->target_note - v->cur_note) < 0.01f) v->cur_note = v->target_note;
    }
    const float note_f  = v->cur_note + mod[MOD_DST_PITCH] * 24.0f;   // ±2 окт на всю глубину
    const float note_hz = 440.0f * exp2f((note_f - 69.0f) * (1.0f / 12.0f));

    // cutoff: flt_env_amt остаётся быстрым фикс-роутом, матрица добавляется в том же лог-домене (±OCT октав).
    const float cutoff = p->cutoff_hz * exp2f((p->flt_env_amt * env_f + mod[MOD_DST_CUTOFF]) * OCT);
    float reso = p->resonance + mod[MOD_DST_RES];   // линейно; итоговый кламп — в filter_coef
    if (reso < 0.0f) reso = 0.0f; else if (reso > 1.0f) reso = 1.0f;
    const FiltCoef fc  = filter_coef(cutoff, reso, sr, p->filt_mode);

    float inc[3];
    int   mip[3];
    for (int j = 0; j < 3; ++j) {
        const float f = note_hz * exp2f(p->osc[j].detune_semi * (1.0f / 12.0f));
        inc[j] = f / sr;
        mip[j] = p->lofi ? 0 : wavetable_mip(f);   // lofi = mip0 → алиасинг = фича (3.4)
    }

    // Wave-position (морф, этап 4.2): смещение формы от матрицы, общее на 3 осц. При wave-env(0..1) и
    // depth=1 покрывает весь диапазон форм (×(WAVE_COUNT-1)). Нулевое смещение → дешёвый прямой lookup.
    const float wpos  = mod[MOD_DST_WAVEPOS] * (float)(WAVE_COUNT - 1);
    const bool  morph = wpos != 0.0f;
    const float pos0  = (float)p->osc[0].wave + wpos;
    const float pos1  = (float)p->osc[1].wave + wpos;
    const float pos2  = (float)p->osc[2].wave + wpos;

    // CPU-скипы: не считаем осц-слот с нулевым уровнем (o0,o1 нужны при ring), шум/ring при нуле.
    const bool need0 = p->osc[0].level > 0.0f || p->ring_level > 0.0f;
    const bool need1 = p->osc[1].level > 0.0f || p->ring_level > 0.0f;
    const bool need2 = p->osc[2].level > 0.0f;
    const bool need_noise = p->noise_level > 0.0f;
    const bool need_ring  = p->ring_level > 0.0f;

    float amp_target = env_a * (1.0f + mod[MOD_DST_AMP]);   // амп-модуляция (тремоло); depth=1 → 0..2×
    if (amp_target < 0.0f) amp_target = 0.0f;
    float       amp      = v->amp_prev;
    const float amp_step = (amp_target - v->amp_prev) / (float)n;   // лерп амплитуды по блоку (анти-зиппер)
    const float q        = p->lofi ? exp2f((float)(p->lofi_bits - 1)) : 1.0f;
    const float qinv     = 1.0f / q;

    for (int i = 0; i < n; ++i) {
        const float o0 = need0 ? (morph ? wavetable_sample_morph(pos0, v->phase[0], mip[0])
                                        : wavetable_sample(p->osc[0].wave, v->phase[0], mip[0])) : 0.0f;
        const float o1 = need1 ? (morph ? wavetable_sample_morph(pos1, v->phase[1], mip[1])
                                        : wavetable_sample(p->osc[1].wave, v->phase[1], mip[1])) : 0.0f;
        const float o2 = need2 ? (morph ? wavetable_sample_morph(pos2, v->phase[2], mip[2])
                                        : wavetable_sample(p->osc[2].wave, v->phase[2], mip[2])) : 0.0f;

        float mix = o0 * p->osc[0].level + o1 * p->osc[1].level + o2 * p->osc[2].level;
        if (need_noise) mix += noise_next(v->rng) * p->noise_level;
        if (need_ring)  mix += (o0 * o1) * p->ring_level;   // ring mod = осц1×осц2 (raw, до уровней)

        float y = filter_process(&v->filt, mix, &fc);
        y = y / (1.0f + fabsf(y));                  // soft-clip: headroom под сумму голосов (3.5)

        float s = y * amp;
        if (p->lofi) {                              // bit-crush (клампим, потом квантуем)
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            s = roundf(s * q) * qinv;
        }
        out[i] = s;

        amp += amp_step;
        v->phase[0] += inc[0]; if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;   // фазы всегда двигаем
        v->phase[1] += inc[1]; if (v->phase[1] >= 1.0f) v->phase[1] -= 1.0f;   //   (свободнобегущие)
        v->phase[2] += inc[2]; if (v->phase[2] >= 1.0f) v->phase[2] -= 1.0f;
    }
    v->amp_prev = amp_target;
}
