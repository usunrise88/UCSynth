// seq_engine — device-side темп-клок + степ-секвенсор + арпеджиатор (этап 7). Чистый DSP (без ESP-IDF),
// host-тестируем. Тикается раз в аудио-блок из audio_task (Core 0), считает семплы → границы 16-х шагов.
// 7.1: клок/транспорт. 7.2: паттерн 16 шагов (аккорды + p-locks + trig-probability). 7.3: арпеджиатор.
#pragma once

#include <cstdint>

static constexpr int SEQ_STEPS      = 16;
static constexpr int SEQ_MAX_NOTES  = 4;   // нот на шаг (аккорд)
static constexpr int SEQ_MAX_PLOCKS = 8;   // p-lock на шаг

// P-lock: значение параметра, привязанное к шагу (применяется, пока клок на этом шаге).
struct SeqPlock { uint16_t id; float val; };

// Один шаг паттерна.
struct SeqStep {
    bool     active;                     // триггерит ноты (p-locks применяются независимо)
    uint8_t  n_notes;
    uint8_t  notes[SEQ_MAX_NOTES];       // MIDI-номера (аккорд)
    uint8_t  velocity;                   // 1..127 (velocity — это p-lock, spec.md:87)
    float    trig_prob;                  // 0..1 (1 = всегда играть)
    uint8_t  n_plocks;
    SeqPlock plocks[SEQ_MAX_PLOCKS];
};

// Конфиг из реестра (читается раз в блок). Клок общий на секвенсор и арпеджиатор.
struct SeqConfig {
    int   bpm;       // 20..300
    float swing;     // 0..1 (сдвиг off-beat 16-х)
    bool  playing;   // транспорт play/stop
    bool  seq_on;    // секвенсор потребляет клок
    // арп-поля — этап 7.3
};

// Событие для аудио-тракта: fire в synth_note_on/off после сборки sp (нота играет с p-lock шага).
struct SeqNoteEvent { uint8_t on; uint8_t note; uint8_t vel; };

struct SeqEngine {
    // клок (7.1)
    bool   running;
    int    step;                         // текущий шаг [0..SEQ_STEPS-1], -1 при стопе
    double acc;                          // накопитель семплов в текущем шаге
    double cur_dur;                      // длительность текущего шага, семплы (swing)
    // паттерн (7.2)
    SeqStep pattern[SEQ_STEPS];
    // активное состояние
    uint8_t  sounding[SEQ_MAX_NOTES];    // ноты, звучащие с текущего шага (для release на след.)
    uint8_t  n_sounding;
    uint16_t ov_id[SEQ_MAX_PLOCKS];      // активные p-lock оверрайды (текущий шаг)
    float    ov_val[SEQ_MAX_PLOCKS];
    uint8_t  n_ov;
    uint32_t rng;                        // xorshift32 для trig-probability (сид → детерминизм в тестах)
};

// Инициализация: транспорт «стоп», паттерн пустой, RNG засеян.
void seq_init(SeqEngine *e);

// Продвинуть клок на n семплов при sr. На границе шага: снять ноты прошлого шага, взять новый шаг
// (trig-probability), выставить его p-lock оверрайды. События пишутся в ev (до evcap), возврат — их число.
// Транспорт stop → снять звучащие ноты (события) + очистить оверрайды.
int seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n, SeqNoteEvent *ev, int evcap);

// P-lock: есть ли оверрайд параметра id на текущем шаге? true → *out = значение оверрайда.
bool seq_plock(const SeqEngine *e, uint16_t id, float *out);
