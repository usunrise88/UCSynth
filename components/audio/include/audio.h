// audio — звуковой движок: I2S+DMA и DSP на Core 0, высокий приоритет, ничего не блокировать.
// Читает значения ТОЛЬКО через control (get_param). Осцилляторы/фильтр/эффекты — этапы 1–5.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_init(void);

// Нотный путь Core 1 → Core 0 (этап 3.0). comm (Core 1) на NOTE_ON/OFF зовёт эти функции;
// событие кладётся в FreeRTOS-очередь, аудио-задача (Core 0) дренит её в начале блока и
// аллоцирует/освобождает голос. Не блокируются: при переполнении очереди событие теряется.
// note — MIDI-номер (0..127, A4=69=440 Гц), vel — velocity (0..127, задел под VCA этапа 3.1).
void audio_note_on(uint8_t note, uint8_t vel);
void audio_note_off(uint8_t note);

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
