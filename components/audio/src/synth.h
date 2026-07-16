// synth — полифонический менеджер голосов: пул Voice, аллокация (round-robin + oldest-steal),
// моно-стек нот (last-note + legato), цели glide, суммирование активных голосов.
// Чистый DSP (float, без ESP-IDF): audio.cpp читает control → SynthParams и зовёт сюда.
// Состояние — только Core 0 (ноты приходят через FreeRTOS-очередь, как раньше). Host-тестируем.
#pragma once

#include <cstdint>
#include "voice.h"

static constexpr int SYNTH_MAX_VOICES = 8;

// Параметры синта: DSP голоса (общие) + полифония/глайд/легато.
struct SynthParams {
    VoiceParams voice;        // форма/фильтр/огибающие/lofi/glide_time — общие для всех голосов
    int         poly_voices;  // 1..8 (1 = моно со стеком нот)
    bool        legato;       // моно: перекрытие нот не ретригерит огибающую
};

void synth_init(void);
void synth_note_on(const SynthParams *sp, uint8_t note, uint8_t vel);
void synth_note_off(const SynthParams *sp, uint8_t note);

// Суммировать активные голоса в out[n] (сырой микс, может выйти за [-1,1] — мастер клампит в audio).
void synth_render(const SynthParams *sp, float sr, float *out, int n);

// Число активных (звучащих/в релизе) голосов — для тестов и диагностики.
int synth_active_count(void);
