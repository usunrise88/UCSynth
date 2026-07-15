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

// Метрики аудио-задачи (этап 1.2): cpu_permille — загрузка в ‰ бюджета блока (1000 = впритык
// к realtime); underruns — число блоков, не уложившихся в бюджет (прокси underrun). NULL ок.
void audio_get_stats(uint32_t *cpu_permille, uint32_t *underruns);

#ifdef __cplusplus
}
#endif
