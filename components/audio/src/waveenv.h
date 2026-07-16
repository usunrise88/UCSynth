// waveenv — пер-голосная многоточечная огибающая модуляции (8 брейкпоинтов, зацикливаемая).
// В отличие от ADSR (env.*) — не gate-driven: стартует с note-on, идёт по фазе (control-rate, шаг = блок),
// линейно интерполируя между 8 уровнями. loop → цикл (8-й сегмент p7→p0), иначе один проход и hold p7.
// Ключевая фича характера этапа 4: выход [0,1] → источник мод-матрицы (обычно → позиция wavetable-морфа).
// Чистый DSP (float, без ESP-IDF) → host-тестируем.
#pragma once

#include <cstdint>

static constexpr int WAVEENV_POINTS = 8;

// Параметры: 8 уровней-брейкпоинтов [0,1]; rate — время полного прохода всех точек, сек; loop — зацикливать.
struct WaveEnvParams {
    float pts[WAVEENV_POINTS];
    float rate;
    bool  loop;
};

// Состояние: фаза прохода [0,1). Вся память — здесь (по образцу Env).
struct WaveEnv {
    float phase;
};

void waveenv_reset(WaveEnv *w);   // фаза 0 (ретригер на note-on)

// Продвинуть на dt секунд и вернуть текущий уровень [0,1].
float waveenv_tick(WaveEnv *w, const WaveEnvParams *p, float dt);
