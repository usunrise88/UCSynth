// audio — звуковой движок: I2S+DMA и DSP на Core 0, высокий приоритет, ничего не блокировать.
// Читает значения ТОЛЬКО через control (get_param). Осцилляторы/фильтр/эффекты — этапы 1–5.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

// Осциллограф для дисплея: снимок формы волны осциллятора (ДО master_volume), int8 [-127..127].
// Писатель — аудио-задача (Core 0), читатель — дисплей (Core 1). out вмещает AUDIO_SCOPE_LEN.
#define AUDIO_SCOPE_LEN 128
void audio_scope_read(int8_t *out);

#ifdef __cplusplus
}
#endif
