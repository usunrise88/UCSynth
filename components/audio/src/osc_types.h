// osc_types — типы осц-слота Classic-движка сверх wavetable (этап 12): VirtualAnalog (PolyBLEP) и
// Phase Distortion. Чистый DSP (float, без ESP-IDF), host-тестируем. voice.cpp диспетчеризует по
// OscSlot.type; wavetable-путь остаётся в voice.cpp/wavetable.cpp (тут его нет).
#pragma once

#include <cstdint>

// VirtualAnalog: пила/меандр без алиасинга (PolyBLEP-коррекция разрывов), треугольник — наивный
// (спад 1/k², алиасинг слаб — см. tech-debt), sine — прямой sinf. `wave` = WaveForm (0=sine 1=saw
// 2=square 3=tri). `dt` — инкремент фазы (cycles/sample), он же ширина коррекции разрыва.
float va_sample(uint8_t wave, float phase, float dt);

// Phase Distortion (Casio CZ): искажение фазы перед чтением синуса. Возвращает искажённую фазу
// [0,1); `amount`=0 → тождество (чистый sine), →1 → яркий «саw-подобный» спектр. voice.cpp читает
// `wavetable_sample(WAVE_SINE, pd_warp(phase, amount), mip)`.
float pd_warp(float phase, float amount);
