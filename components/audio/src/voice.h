// voice — самодостаточный синт-голос: 3 осц-слота → микшер (+шум +ring mod) → ZDF-фильтр → VCA,
// с двумя ADSR (VCA и VCF→cutoff). Чистый DSP (float, без ESP-IDF): audio.cpp читает control и
// передаёт VoiceParams по const-ref — голос сам НЕ трогает реестр. Для полифонии (3.5) — массив Voice.
#pragma once

#include <cstdint>
#include "env.h"
#include "filter.h"

// Осц-слот: форма (enum WaveForm), детюн в полутонах (дробные = центы), уровень в микшере [0,1].
struct OscSlot {
    uint8_t wave;
    float   detune_semi;
    float   level;
};

// Параметры голоса — читаются раз в блок из control, общие для всех голосов (const).
struct VoiceParams {
    OscSlot   osc[3];
    float     noise_level, ring_level;
    float     cutoff_hz, resonance;
    uint8_t   filt_mode;
    float     flt_env_amt;              // модуляция cutoff огибающей VCF, [-1,1] (октавы)
    EnvParams amp_env, flt_env;
    bool      lofi;
    int       lofi_bits;
    bool      latch;                    // дрон-защёлка: gate держится после отпускания клавиши
    float     glide_time;               // портаменто: время скольжения высоты, с (0 = мгновенно)
};

struct Voice {
    float    phase[3];                  // свободнобегущие (анти-клик на легато)
    Env      env_amp, env_flt;
    Filter   filt;
    float    cur_note, target_note;     // высота в MIDI-float: cur скользит к target (glide, 3.6)
    uint8_t  note;                      // текущая нота (для матчинга note-off)
    bool     key_down, latch_held;
    uint32_t rng;                       // xorshift32 для шума
    float    amp_prev;                  // амплитуда прошлого блока (для лерпа VCA)
};

void voice_init(Voice *v, uint32_t seed);

// Взять ноту с ретригером обеих огибающих (обычный note-on: poly-аллокация, mono non-legato).
// glide=false → cur_note снапится к ноте (свежий/idle голос); glide=true → скользит от cur_note.
void voice_note_on(Voice *v, uint8_t note, uint8_t vel, bool latch, bool glide);

// Сменить цель высоты БЕЗ ретригера огибающих (legato-скольжение: mono legato / возврат на note-off).
void voice_slide(Voice *v, uint8_t note, bool glide);

// Отпустить (gate off), если нота совпадает с текущей.
void voice_note_off(Voice *v, uint8_t note);

// Отрендерить n семплов в out (диапазон ~[-1,1], soft-clip). sr — частота дискретизации.
void voice_render(Voice *v, const VoiceParams *p, float sr, float *out, int n);
