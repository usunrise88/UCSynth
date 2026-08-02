// voice — самодостаточный синт-голос: 3 осц-слота → микшер (+шум +ring mod) → ZDF-фильтр → VCA,
// с двумя ADSR (VCA и VCF→cutoff). Чистый DSP (float, без ESP-IDF): audio.cpp читает control и
// передаёт VoiceParams по const-ref — голос сам НЕ трогает реестр. Для полифонии (3.5) — массив Voice.
#pragma once

#include <cstdint>
#include "env.h"
#include "waveenv.h"
#include "filter.h"

// Тип осц-слота Classic-движка (этап 12). Wavetable — как раньше (морф по wave-позиции); VA/PD —
// см. osc_types.h. НЕ переупорядочивать: пишется в патчи параметром oscN_type, дописывать перед _COUNT.
enum OscType : uint8_t {
    OSC_WAVETABLE = 0,
    OSC_VA,          // VirtualAnalog (PolyBLEP)
    OSC_PD,          // Phase Distortion (Casio CZ)
    OSC_TYPE_COUNT
};

// Движок голоса (этап 12). Classic — 3 осц-слота (тип на слот); FM — 2-оператора; Karplus — щипок.
// НЕ переупорядочивать: пишется в патчи параметром voice_engine, дописывать перед _COUNT.
enum VoiceEngine : uint8_t {
    ENG_CLASSIC = 0,
    ENG_FM,          // 2-операторная FM (12.3)
    ENG_KS,          // Karplus-Strong (12.4)
    ENG_COUNT
};

// Осц-слот: форма (enum WaveForm), детюн в полутонах (дробные = центы), уровень в микшере [0,1],
// тип (OscType). VA использует `wave` как выбор пилы/меандра/тр-ка; PD игнорирует `wave` (всегда sine).
struct OscSlot {
    uint8_t wave;
    float   detune_semi;
    float   level;
    uint8_t type;
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
// res/amp — линейно, wave-pos — морф (4.2).
//
// FX-приёмника здесь НЕТ сознательно. Эффекты считаются в audio.cpp один раз после суммы голосов,
// а матрица — пер-голосная: «LFO → reverb size» из восьми голосов означало бы восемь разных
// значений одного глобального параметра. Раньше MOD_DST_FX существовал, выбирался в GUI, копился в
// mod[] и никем не читался: пользователь молча тратил слот. Если делать модуляцию FX — это отдельная
// глобальная матрица на control-rate, а не строка в этой; до тех пор приёмника нет и в GUI
// (mtxN_dst max = MOD_DST_COUNT-1).
enum ModDest : uint8_t {
    MOD_DST_NONE = 0,
    MOD_DST_PITCH,
    MOD_DST_CUTOFF,
    MOD_DST_RES,
    MOD_DST_AMP,
    MOD_DST_WAVEPOS,    // этап 4.2
    MOD_DST_COUNT
};

static constexpr int MOD_SLOTS = 8;   // гибкая матрица: 8 роутов src→dst с глубиной

// Karplus-Strong (этап 12.4): длина линии задержки = sr/freq. Макс под низшую ноту KS ≈ sr/KS_MAX
// (48к/1536 ≈ 31 Гц ≈ MIDI 23). Буфер пер-голосный во внутренней RAM: 8×1536×4 ≈ 48 КБ (дефицитный
// пул — замерить free-internal-RAM; при нехватке урезать KS_MAX/диапазон/полифонию, см. tech-debt).
static constexpr int KS_MAX = 1536;

// Один слот матрицы: источник, приёмник, глубина [-1,1]. src/dst == NONE → слот выключен.
struct ModSlot {
    uint8_t src;
    uint8_t dst;
    float   depth;
};

// Параметры голоса — читаются раз в блок из control, общие для всех голосов (const).
struct VoiceParams {
    OscSlot   osc[3];
    uint8_t   engine;                    // этап 12: VoiceEngine (Classic/FM/Karplus)
    float     pd_amount;                 // этап 12: глубина Phase Distortion (общая для PD-слотов) 0..1
    float     fm_ratio, fm_index;        // этап 12.3: FM — отношение частот, индекс модуляции
    float     ks_damp, ks_decay, ks_pluck; // этап 12.4: Karplus — затухание петли, спад, характер щипка
    float     noise_level, ring_level;
    float     cutoff_hz, resonance;
    uint8_t   filt_mode;
    float     flt_env_amt;              // модуляция cutoff огибающей VCF, [-1,1] (октавы)
    EnvParams amp_env, flt_env;
    bool      lofi;
    int       lofi_bits;
    bool      latch;                    // дрон-защёлка: gate держится после отпускания клавиши
    float     glide_time;               // портаменто: время скольжения высоты, с (0 = мгновенно)
    float        mod_src[MOD_SRC_COUNT]; // глобальные источники (LFO, mod-wheel), заполняет audio.cpp
    ModSlot      mtx[MOD_SLOTS];         // мод-матрица: 8 роутов
    WaveEnvParams wave_env;             // wave-огибающая (источник WAVE_ENV), обычно → морф wavetable
};

struct Voice {
    float    phase[3];                  // свободнобегущие (анти-клик на легато)
    Env      env_amp, env_flt;
    WaveEnv  env_wave;                  // третий генератор: wave-огибающая (ретригер на note-on)
    Filter   filt;
    float    cur_note, target_note;     // высота в MIDI-float: cur скользит к target (glide, 3.6)
    uint8_t  note;                      // текущая нота (для матчинга note-off)
    bool     key_down, latch_held;
    uint32_t rng;                       // xorshift32 для шума
    float    amp_prev;                  // амплитуда прошлого блока (для лерпа VCA)
    float    velocity;                  // [0,1] сила нажатия, источник матрицы (взводится на note-on)
    // Karplus-Strong (12.4): линия задержки + one-pole в петле. excite взводится на note-on, буфер
    // заполняется шум-берстом на первом KS-блоке. Для Classic/FM не используется (память статична).
    float    ks_buf[KS_MAX];
    int      ks_len, ks_pos;
    float    ks_last;
    bool     ks_excite;
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
