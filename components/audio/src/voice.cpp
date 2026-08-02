#include "voice.h"
#include "wavetable.h"
#include "osc_types.h"
#include "dsp_hot.h"
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

// Один осц-слот Classic-движка: диспетчер по типу (этап 12). Wavetable сохраняет прежнее поведение
// (морф по wave-позиции); VA — PolyBLEP по фазе и inc (ширина коррекции); PD — синус искажённой фазы.
inline float osc_slot_sample(const OscSlot &o, float pos, bool morph,
                             float phase, float inc, int mip, float pd)
{
    switch (o.type) {
    case OSC_VA: return va_sample(o.wave, phase, inc);
    case OSC_PD: return wavetable_sample(WAVE_SINE, pd_warp(phase, pd), mip);
    default:     return morph ? wavetable_sample_morph(pos, phase, mip)
                              : wavetable_sample(o.wave, phase, mip);
    }
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

void AUDIO_HOT voice_render(Voice *v, const VoiceParams *p, float sr, float *out, int n)
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
    // Кламп суммы матрицы обязателен: 8 слотов × depth ±1 дают mod[PITCH] до ±8, то есть ±192
    // полутона. Плюс детюн ±24 — и частота осциллятора уходит далеко за Найквист, где inc > 1 и
    // фазовый аккумулятор перестаёт заворачиваться (см. заворот в конце функции). Ограничиваем
    // одним слотом на полную глубину: ±2 октавы, как и обещает документация приёмника.
    float pitch_mod = mod[MOD_DST_PITCH];
    if (pitch_mod < -1.0f) pitch_mod = -1.0f; else if (pitch_mod > 1.0f) pitch_mod = 1.0f;
    const float note_f  = v->cur_note + pitch_mod * 24.0f;   // ±2 окт на всю глубину
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

    // Тракт после генератора — общий для всех движков: фильтр → soft-clip → VCA → (lo-fi) → out.
    // Заворот фаз через floorf, а не одним вычитанием: при inc > 1 (частота выше sample rate) вычесть
    // единицу недостаточно, фаза растёт со скоростью inc−1 и не возвращается в [0,1); дальше теряется
    // точность float (на ~1.7e7 дробная часть залипает — тишина), а выше 2^31 конверсия (int)phase — UB.
    auto emit = [&](float mix, int i) {
        float y = filter_process(&v->filt, mix, &fc);
        y = y / (1.0f + fabsf(y));                  // soft-clip: headroom под сумму голосов (3.5)
        float s = y * amp;
        if (p->lofi) {                              // bit-crush (клампим, потом квантуем)
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            s = roundf(s * q) * qinv;
        }
        out[i] = s;
        amp += amp_step;
    };

    if (p->engine == ENG_FM) {
        // 2-оператора (этап 12.3): несущая (phase[0], высота ноты) + модулятор (phase[1], ratio·несущая),
        // фазовая модуляция (PM ≈ FM). Индекс — глубина. Тракт голоса (фильтр/VCA) сохраняется.
        const float inc_c = inc[0];
        const float inc_m = inc[0] * p->fm_ratio;
        const int   mp    = mip[0];
        for (int i = 0; i < n; ++i) {
            const float m = wavetable_sample(WAVE_SINE, v->phase[1], mp);
            float cph = v->phase[0] + p->fm_index * m;
            cph -= floorf(cph);
            emit(wavetable_sample(WAVE_SINE, cph, mp), i);
            v->phase[0] += inc_c; v->phase[0] -= floorf(v->phase[0]);
            v->phase[1] += inc_m; v->phase[1] -= floorf(v->phase[1]);
        }
    } else {
        // Classic: 3 осц-слота (тип на слот) → микшер (+шум +ring). ENG_KS сюда же до 12.4.
        for (int i = 0; i < n; ++i) {
            const float o0 = need0 ? osc_slot_sample(p->osc[0], pos0, morph, v->phase[0], inc[0], mip[0], p->pd_amount) : 0.0f;
            const float o1 = need1 ? osc_slot_sample(p->osc[1], pos1, morph, v->phase[1], inc[1], mip[1], p->pd_amount) : 0.0f;
            const float o2 = need2 ? osc_slot_sample(p->osc[2], pos2, morph, v->phase[2], inc[2], mip[2], p->pd_amount) : 0.0f;

            float mix = o0 * p->osc[0].level + o1 * p->osc[1].level + o2 * p->osc[2].level;
            if (need_noise) mix += noise_next(v->rng) * p->noise_level;
            if (need_ring)  mix += (o0 * o1) * p->ring_level;   // ring mod = осц1×осц2 (raw, до уровней)

            emit(mix, i);
            v->phase[0] += inc[0]; v->phase[0] -= floorf(v->phase[0]);   // фазы всегда двигаем
            v->phase[1] += inc[1]; v->phase[1] -= floorf(v->phase[1]);   //   (свободнобегущие)
            v->phase[2] += inc[2]; v->phase[2] -= floorf(v->phase[2]);
        }
    }
    v->amp_prev = amp_target;
}
