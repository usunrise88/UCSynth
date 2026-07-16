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
    filter_reset(&v->filt);
    v->note_hz    = 440.0f;
    v->note       = 255;
    v->key_down   = false;
    v->latch_held = false;
    v->rng        = seed ? seed : 0x1234567u;
    v->amp_prev   = 0.0f;
}

void voice_note_on(Voice *v, uint8_t note, uint8_t vel, bool latch)
{
    (void)vel;                          // velocity → глубина VCA: задел (3.1 без вел-чувствительности)
    v->note     = note;
    v->note_hz  = note_to_hz(note);
    v->key_down = true;
    if (latch) v->latch_held = true;    // защёлка дрона взводится по note-on
}

void voice_note_off(Voice *v, uint8_t note)
{
    if (note == v->note) v->key_down = false;   // моно: гасим только текущую ноту
}

void voice_render(Voice *v, const VoiceParams *p, float sr, float *out, int n)
{
    if (!p->latch) v->latch_held = false;       // снятие параметра latch отпускает дрон
    const bool gate = v->key_down || v->latch_held;

    // control-rate: огибающие, коэфф. фильтра, инкременты фаз.
    const float dt    = (float)n / sr;
    const float env_a = env_tick(&v->env_amp, &p->amp_env, gate, dt);
    const float env_f = env_tick(&v->env_flt, &p->flt_env, gate, dt);

    const float cutoff = p->cutoff_hz * exp2f(p->flt_env_amt * env_f * OCT);   // кламп — в filter_coef
    const FiltCoef fc  = filter_coef(cutoff, p->resonance, sr, p->filt_mode);

    float inc[3];
    int   mip[3];
    for (int j = 0; j < 3; ++j) {
        const float f = v->note_hz * exp2f(p->osc[j].detune_semi * (1.0f / 12.0f));
        inc[j] = f / sr;
        mip[j] = p->lofi ? 0 : wavetable_mip(f);   // lofi = mip0 → алиасинг = фича (3.4)
    }

    float       amp      = v->amp_prev;
    const float amp_step = (env_a - v->amp_prev) / (float)n;   // лерп амплитуды по блоку (анти-зиппер)
    const float q        = p->lofi ? exp2f((float)(p->lofi_bits - 1)) : 1.0f;
    const float qinv     = 1.0f / q;

    for (int i = 0; i < n; ++i) {
        const float o0 = wavetable_sample(p->osc[0].wave, v->phase[0], mip[0]);
        const float o1 = wavetable_sample(p->osc[1].wave, v->phase[1], mip[1]);
        const float o2 = wavetable_sample(p->osc[2].wave, v->phase[2], mip[2]);

        float mix = o0 * p->osc[0].level + o1 * p->osc[1].level + o2 * p->osc[2].level;
        mix += noise_next(v->rng) * p->noise_level;
        mix += (o0 * o1) * p->ring_level;          // ring mod = осц1×осц2 (raw, до уровней)

        float y = filter_process(&v->filt, mix, &fc);
        y = y / (1.0f + fabsf(y));                  // soft-clip: headroom под сумму голосов (3.5)

        float s = y * amp;
        if (p->lofi) {                              // bit-crush (клампим, потом квантуем)
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            s = roundf(s * q) * qinv;
        }
        out[i] = s;

        amp += amp_step;
        for (int j = 0; j < 3; ++j) {
            v->phase[j] += inc[j];
            if (v->phase[j] >= 1.0f) v->phase[j] -= 1.0f;
        }
    }
    v->amp_prev = env_a;
}
