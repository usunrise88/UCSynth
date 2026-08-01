// seq_engine — device-side темп-клок + степ-секвенсор + арпеджиатор (этап 7). Чистый DSP (без ESP-IDF),
// host-тестируем. Тикается раз в аудио-блок из audio_task (Core 0), считает семплы → границы шагов.
// 7.1: клок/транспорт. 7.2: паттерн 16 шагов (аккорды + p-locks + trig-probability). 7.3: арпеджиатор.
#pragma once

#include <cstdint>

static constexpr int SEQ_STEPS        = 16;
static constexpr int SEQ_MAX_NOTES    = 4;    // нот на шаг (аккорд)
static constexpr int SEQ_MAX_PLOCKS   = 8;    // p-lock на шаг
static constexpr int SEQ_ARP_MAX_HELD = 12;   // сколько зажатых нот держит арп

enum ArpMode : uint8_t { ARP_UP = 0, ARP_DOWN, ARP_UPDOWN, ARP_RANDOM };

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
    int     bpm;         // 20..300
    float   swing;       // 0..1 (сдвиг off-beat 16-х секвенсора)
    bool    playing;     // транспорт play/stop
    bool    seq_on;      // секвенсор потребляет клок
    bool    arp_on;      // арпеджиатор потребляет клок (из зажатых нот)
    uint8_t arp_mode;    // ArpMode
    int     arp_octaves; // 1..4
    int     arp_rate;    // 0=1/4 1=1/8 2=1/16 3=1/32 (деление клока)
    bool    arp_hold;    // держать аккорд после отпускания клавиш
};

// Событие для аудио-тракта: fire в synth_note_on/off после сборки sp (нота играет с p-lock шага).
struct SeqNoteEvent { uint8_t on; uint8_t note; uint8_t vel; };

struct SeqEngine {
    // клок секвенсора (7.1)
    bool   running;
    int    step;                         // текущий шаг [0..SEQ_STEPS-1], -1 при стопе
    double acc;                          // накопитель семплов в текущем шаге
    double cur_dur;                      // длительность текущего шага, семплы (swing)
    // паттерн (7.2)
    SeqStep pattern[SEQ_STEPS];
    uint8_t  sounding[SEQ_MAX_NOTES];    // ноты, звучащие с текущего шага секвенсора
    uint8_t  n_sounding;
    uint16_t ov_id[SEQ_MAX_PLOCKS];      // активные p-lock оверрайды (текущий шаг)
    float    ov_val[SEQ_MAX_PLOCKS];
    uint8_t  n_ov;
    uint32_t rng;                        // xorshift32 для trig-probability
    // арпеджиатор (7.3)
    bool     arp_running;
    double   arp_acc, arp_dur;           // собственный клок арпа (деление по arp_rate)
    uint8_t  held[SEQ_ARP_MAX_HELD];     // зажатые ноты (источник арпа)
    uint8_t  n_held;
    int      phys_down;                  // сколько клавиш физически зажато (для hold)
    int      arp_pos;                    // позиция в арп-последовательности
    int      arp_dir;                    // +1/-1 для up-down
    uint8_t  arp_sounding;               // текущая нота арпа (255 = нет)
    uint32_t arp_rng;                    // xorshift32 для random-арпа
};

// Инициализация: транспорт «стоп», паттерн пустой, held пуст, RNG засеян.
void seq_init(SeqEngine *e);

// Продвинуть клок на n семплов при sr. Секвенсор (границы шагов, seq_on) и арпеджиатор (свой клок,
// arp_on, из held) пишут ноты в ev (до evcap); возврат — их число. Транспорт stop → снять всё звучащее.
int seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n, SeqNoteEvent *ev, int evcap);

// Обновить множество зажатых нот для арпа (клавиша/MIDI). on=true — нажали, false — отпустили.
// hold=true — держать аккорд после отпускания (новое нажатие после полного отпускания начинает заново).
void seq_arp_note(SeqEngine *e, uint8_t note, bool on, bool hold);

// P-lock: есть ли оверрайд параметра id на текущем шаге? true → *out = значение оверрайда.
bool seq_plock(const SeqEngine *e, uint16_t id, float *out);
