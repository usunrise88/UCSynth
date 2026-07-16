// dsp_hot — атрибут для горячих DSP-функций (крутятся на аудио-rate, 48000×/сек на Core 0).
// На ESP кладёт код во ВНУТРЕННИЙ RAM (IRAM) → нет столлов кэша флеша (и меньше конкуренции за кэш с
// PSRAM-буферами delay). На host-сборке (тесты) — пусто. Ставить на ОПРЕДЕЛЕНИЯ функций аудио-rate.
#pragma once

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define AUDIO_HOT IRAM_ATTR
#else
#define AUDIO_HOT
#endif
