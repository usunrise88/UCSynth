// fx — глобальная эффект-секция (этап 5), применяется в хвосте audio_task ПОСЛЕ суммы голосов.
// Тракт: overdrive (моно, 5.1) → split L/R → delay (стерео, 5.2) → reverb (стерео, 5.3). Растёт по
// под-этапам. Чистый DSP (float, без ESP-IDF) → host-тестируем. Буферы delay/reverb — только PSRAM
// (в прошивке), в host-тестах — обычная память. Никакого double/M_PI (FPU S3 одинарной точности).
#pragma once

#include <cstdint>

// Параметры эффектов — читаются раз в блок из реестра (в build_synth_params), применяются в audio_task.
struct FxParams {
    // --- overdrive (5.1) ---
    bool  od_on;
    float od_drive;      // 0..1 → входной гейн в шейпер (1..12×)
    float od_mix;        // 0..1 wet/dry
    // --- master drive (эффект «грязи» из hard-clamp суммы голосов; норм. по числу голосов → характер
    //     не зависит от полифонии). Моно-сумма до split, после overdrive. ---
    bool  drive_on;
    float drive;         // 0..1 глубина сатурации (входной гейн 1..10× после нормализации)
    float drive_mix;     // 0..1 wet/dry
    // --- delay (5.2) ---
    bool  delay_on;
    float delay_time;    // мс (≤1000)
    float delay_feedback;// 0..0.95 (клампится <1 для устойчивости)
    float delay_damp;    // 0..1 затухание ВЧ в обратной связи (0 = ярко, 1 = темно)
    float delay_mix;     // 0..1 wet/dry
    // --- reverb (5.3, Freeverb) ---
    bool  reverb_on;
    float reverb_size;   // 0..1 размер комнаты (feedback гребёнок)
    float reverb_damp;   // 0..1 затухание ВЧ в гребёнках
    float reverb_width;  // 0..1 стерео-ширина
    float reverb_mix;    // 0..1 wet/dry
    float reverb_moddepth; // 0..1 глубина модуляции длины гребёнок (0 = выкл → звон как есть, дефолт)
    float reverb_modrate;  // Гц скорость модуляции (медленный LFO на каждую гребёнку)
};

// Freeverb (Schroeder/Moorer): 8 гребёнок + 4 allpass на канал. Готовый алгоритм (роадмап «бери готовое»).
static constexpr int RV_NCOMB = 8;
static constexpr int RV_NAP   = 4;

struct Comb {    // гребёнка с затуханием ВЧ (one-pole LP в ОС)
    float *buf;
    int    len, idx;
    float  store;
    float  mod_phase;   // фаза LFO модуляции длины гребёнки [0,1) (этап 6: comb-mod против звона)
};
struct Allpass { // allpass (фикс. feedback 0.5)
    float *buf;
    int    len, idx;
};

// Состояние эффектов с памятью (delay/reverb). Буферы delay выделяются СНАРУЖИ (PSRAM в прошивке через
// heap_caps_malloc, обычный malloc в host-тесте) и привязываются fx_delay_init — сам fx их не аллоцирует.
struct FxState {
    // --- delay (5.2): стерео кольцо + one-pole damp в обратной связи ---
    float *dl_l, *dl_r;   // кольцевые буферы (nullptr → delay недоступен, эффект пропускается)
    int    dl_len;        // длина буфера, сэмплы (= 1 с)
    int    dl_wr;         // индекс записи
    float  dl_lp_l, dl_lp_r;  // состояние one-pole LP в цепи ОС (damp)
    bool   dl_was_on;     // для детекта фронта off→on (чистка кольца, см. fx_delay)
    // --- reverb (5.3): Freeverb, буферы гребёнок/allpass — в едином PSRAM-блоке (nullptr → нет реверба) ---
    Comb    rv_combL[RV_NCOMB], rv_combR[RV_NCOMB];
    Allpass rv_apL[RV_NAP],     rv_apR[RV_NAP];
    bool    rv_was_on;    // для детекта фронта off→on (чистка линий, см. fx_reverb)
};

// Привязать кольцевые буферы delay (len сэмплов каждый), обнулить их и сбросить состояние.
void fx_delay_init(FxState *fx, float *buf_l, float *buf_r, int len);

// Стерео ping-pong delay in-place на n сэмплах. off/нет буфера → dry. Единое время; перекрёстная ОС →
// одно эхо на delay_time, скачущее L↔R (mono-совместимо). feedback с one-pole damp; денормалы флашатся.
void fx_delay(FxState *fx, const FxParams *p, float *l, float *r, int n, float sr);

// Сколько float-сэмплов нужно под все линии задержки реверба (для аллокации одного PSRAM-блока).
int  fx_reverb_bufsize();
// Нарезать buf (nsamples ≥ fx_reverb_bufsize()) на гребёнки/allpass, обнулить, сбросить индексы.
void fx_reverb_init(FxState *fx, float *buf, int nsamples);
// Стерео reverb (Freeverb) in-place на n сэмплах. off/нет буфера → dry. Денормалы флашатся.
void fx_reverb(FxState *fx, const FxParams *p, float *l, float *r, int n);

// Overdrive: waveshaper tanh с входным гейном и wet/dry. Мемори-лесс (без состояния) → чистая функция.
// Без оверсэмплинга (алиасинг — осознанный риск, risks.md). wet = tanh(x·gain) всегда в [-1,1] → выход
// ограничен при mix=1 (в т.ч. приручает сырую сумму голосов >1). Вызывается на моно-семпл до split.
float fx_overdrive(float x, const FxParams *p);

// Master drive: делит сумму на norm (≈уровень одного голоса → характер не зависит от числа голосов),
// гейнит по drive и жёстко клипует (тот самый «перегруз»), затем wet/dry. norm — сглаженное число
// активных голосов, считается в audio.cpp. Мемори-лесс. Без оверсэмплинга (жёсткий клип алиасит — D-026).
float fx_drive(float x, const FxParams *p, float norm);

// Мягкий мастер-лимитер: прозрачен ниже порога t, дальше плавно жмёт к ±1 (замена hard-clamp суммы
// голосов — полифония не даёт грязного клипа). Наклон непрерывен на колене, асимптота ±1. Мемори-лесс.
float fx_master_limit(float x);
