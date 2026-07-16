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

// Мод-матрица (этап 4). Источники — скаляры [-1,1] раз в блок: глобальные (LFO, mod-wheel) кладёт
// audio.cpp в mod_src[]; пер-голосные (VCF-env, velocity, wave-env) голос подставляет сам в voice_render.
// TOF (VL53L0X) — источник по спеке, но датчик с этапа 10 → зарезервирован, значение 0.
// НЕ переупорядочивать: enum пишется в патчи параметром mtxN_src/dst, только дописывать перед _COUNT.
enum ModSource : uint8_t {
    MOD_SRC_NONE = 0,
    MOD_SRC_LFO1,
    MOD_SRC_LFO2,
    MOD_SRC_VCF_ENV,
    MOD_SRC_WAVE_ENV,   // этап 4.2 (пока 0)
    MOD_SRC_VELOCITY,
    MOD_SRC_MODWHEEL,
    MOD_SRC_TOF,        // этап 10 (пока 0)
    MOD_SRC_COUNT
};

// Приёмники. Применяются в своих точках в натуральном домене: pitch/cutoff — лог (exp2f),
// res/amp — линейно, wave-pos — морф (4.2), FX — к параметрам эффектов (этап 5, пока нет).
enum ModDest : uint8_t {
    MOD_DST_NONE = 0,
    MOD_DST_PITCH,
    MOD_DST_CUTOFF,
    MOD_DST_RES,
    MOD_DST_AMP,
    MOD_DST_WAVEPOS,    // этап 4.2
    MOD_DST_FX,         // этап 5
    MOD_DST_COUNT
};

static constexpr int MOD_SLOTS = 8;   // гибкая матрица: 8 роутов src→dst с глубиной

// Один слот матрицы: источник, приёмник, глубина [-1,1]. src/dst == NONE → слот выключен.
struct ModSlot {
    uint8_t src;
    uint8_t dst;
    float   depth;
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
    float     mod_src[MOD_SRC_COUNT];   // глобальные источники (LFO, mod-wheel), заполняет audio.cpp
    ModSlot   mtx[MOD_SLOTS];           // мод-матрица: 8 роутов
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
    float    velocity;                  // [0,1] сила нажатия, источник матрицы (взводится на note-on)
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
