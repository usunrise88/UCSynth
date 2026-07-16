// lfo — глобальный НЧ-осциллятор модуляции, control-rate (шаг = один аудио-блок).
// Формы: sine / tri / saw / square / S&H (sample-and-hold, случайная выборка на цикл). Возврат [-1,1].
// Чистый DSP (float, без ESP-IDF) → host-тестируем. Глубина и маршрутизация — в мод-матрице (voice.h).
#pragma once

#include <cstdint>

enum LfoShape : uint8_t { LFO_SINE = 0, LFO_TRI, LFO_SAW, LFO_SQUARE, LFO_SH, LFO_SHAPE_COUNT };

struct Lfo {
    float    phase;   // [0,1)
    float    sh;      // текущее значение S&H (держится до нового цикла)
    uint32_t rng;     // xorshift32 для S&H
};

// Сброс: фаза 0, посев ГСЧ, стартовое значение S&H.
void lfo_reset(Lfo *l, uint32_t seed);

// Продвинуть фазу на rate_hz·dt и вернуть значение формы [-1,1]. rate_hz — Гц, dt — сек (длина блока).
float lfo_tick(Lfo *l, float rate_hz, float dt, uint8_t shape);
