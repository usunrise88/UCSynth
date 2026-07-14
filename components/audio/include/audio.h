// audio — звуковой движок: I2S+DMA и DSP на Core 0, высокий приоритет, ничего не блокировать.
// Читает значения ТОЛЬКО через control (get_param). Осцилляторы/фильтр/эффекты — этапы 1–5.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

#ifdef __cplusplus
}
#endif
