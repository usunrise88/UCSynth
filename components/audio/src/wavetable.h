// wavetable — band-limited осцилляторы: октавные mip-таблицы против алиасинга (D-008).
// На каждую форму — набор таблиц, по одной на октавный диапазон частот (гармоники до Найквиста
// для верха диапазона). Осциллятор выбирает mip по фундаментальной частоте (control-rate),
// а в семпловом цикле делает дешёвый lookup без log2. Чистый DSP без ESP-IDF → host-тестируем.
#pragma once

#include <cstdint>

enum WaveForm : uint8_t {
    WAVE_SINE   = 0,
    WAVE_SAW    = 1,
    WAVE_SQUARE = 2,
    WAVE_TRI    = 3,
    WAVE_COUNT,
};

// Сгенерировать band-limited таблицы под частоту дискретизации. Один раз до использования.
void wavetable_init(float sample_rate);

// Индекс mip по фундаментальной частоте (звать раз в блок, не на каждый семпл).
int wavetable_mip(float freq_hz);

// Значение формы w по фазе [0,1) из выбранного mip, линейная интерполяция. Возврат [-1,1].
float wavetable_sample(uint8_t w, float phase, int mip);
