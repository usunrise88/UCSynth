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
    // --- delay (5.2) ---
    bool  delay_on;
    float delay_time;    // мс (≤1000)
    float delay_feedback;// 0..0.95 (клампится <1 для устойчивости)
    float delay_damp;    // 0..1 затухание ВЧ в обратной связи (0 = ярко, 1 = темно)
    float delay_mix;     // 0..1 wet/dry
};

// Состояние эффектов с памятью (delay/reverb). Буферы delay выделяются СНАРУЖИ (PSRAM в прошивке через
// heap_caps_malloc, обычный malloc в host-тесте) и привязываются fx_delay_init — сам fx их не аллоцирует.
struct FxState {
    // --- delay (5.2): стерео кольцо + one-pole damp в обратной связи ---
    float *dl_l, *dl_r;   // кольцевые буферы (nullptr → delay недоступен, эффект пропускается)
    int    dl_len;        // длина буфера, сэмплы (= 1 с)
    int    dl_wr;         // индекс записи
    float  dl_lp_l, dl_lp_r;  // состояние one-pole LP в цепи ОС (damp)
};

// Привязать кольцевые буферы delay (len сэмплов каждый), обнулить их и сбросить состояние.
void fx_delay_init(FxState *fx, float *buf_l, float *buf_r, int len);

// Стерео delay in-place на n сэмплах. off/нет буфера → dry (l/r без изменений). L читается раньше R
// (стерео-ширина). feedback с one-pole damp; денормалы флашатся (LX7 медленно их считает).
void fx_delay(FxState *fx, const FxParams *p, float *l, float *r, int n, float sr);

// Overdrive: waveshaper tanh с входным гейном и wet/dry. Мемори-лесс (без состояния) → чистая функция.
// Без оверсэмплинга (алиасинг — осознанный риск, risks.md). wet = tanh(x·gain) всегда в [-1,1] → выход
// ограничен при mix=1 (в т.ч. приручает сырую сумму голосов >1). Вызывается на моно-семпл до split.
float fx_overdrive(float x, const FxParams *p);
