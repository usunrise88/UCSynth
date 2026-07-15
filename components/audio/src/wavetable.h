// wavetable — одноцикловые таблицы форм волн + осциллятор (фаза-аккумулятор + линейная
// интерполяция). Чистый DSP без ESP-IDF → host-тестируем. Формы соответствуют PARAM_WAVEFORM.
// band-limiting (мип-таблицы против алиасинга на высоких нотах) отложен — см. tech-debt D-008.
#pragma once

#include <cstdint>

enum WaveForm : uint8_t {
    WAVE_SINE   = 0,
    WAVE_SAW    = 1,
    WAVE_SQUARE = 2,
    WAVE_TRI    = 3,
    WAVE_COUNT,
};

static constexpr int WT_LEN = 2048;   // семплов на одноцикловую таблицу

// Сгенерировать таблицы (в RAM). Вызвать один раз до wavetable_sample.
void wavetable_init(void);

// Значение формы w по нормированной фазе [0,1) с линейной интерполяцией. Возврат [-1,1].
float wavetable_sample(uint8_t w, float phase);
