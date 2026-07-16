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
    float od_drive;   // 0..1 → входной гейн в шейпер (1..12×)
    float od_mix;     // 0..1 wet/dry
};

// Overdrive: waveshaper tanh с входным гейном и wet/dry. Мемори-лесс (без состояния) → чистая функция.
// Без оверсэмплинга (алиасинг — осознанный риск, risks.md). wet = tanh(x·gain) всегда в [-1,1] → выход
// ограничен при mix=1 (в т.ч. приручает сырую сумму голосов >1). Вызывается на моно-семпл до split.
float fx_overdrive(float x, const FxParams *p);
