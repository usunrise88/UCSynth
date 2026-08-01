// seq_engine — device-side темп-клок + степ-секвенсор + арпеджиатор (этап 7). Чистый DSP (без ESP-IDF),
// host-тестируем. Тикается раз в аудио-блок из audio_task (Core 0), считает семплы → границы 16-х шагов.
// Инкремент 7.1: клок + транспорт + структура p-lock-оверрайдов (паттерн/ноты/арп — 7.2/7.3).
#pragma once

#include <cstdint>

static constexpr int SEQ_STEPS = 16;

// Конфиг из реестра (читается раз в блок). Клок общий на секвенсор и арпеджиатор.
struct SeqConfig {
    int   bpm;       // 20..300
    float swing;     // 0..1 (сдвиг off-beat 16-х)
    bool  playing;   // транспорт play/stop
    bool  seq_on;    // секвенсор потребляет клок (этап 7.2)
    // арп-поля — этап 7.3
};

struct SeqEngine {
    bool   running;  // клок идёт (playing && хоть раз стартовали)
    int    step;     // текущий шаг [0..SEQ_STEPS-1], -1 при стопе
    double acc;      // накопитель семплов в текущем шаге
    double cur_dur;  // длительность текущего шага, семплы (с учётом swing)
    // паттерн + p-lock оверрайды — этап 7.2
};

void seq_init(SeqEngine *e);

// Продвинуть клок на n семплов при sr. Возвращает true, если в этом вызове пересечена граница шага
// (e->step обновлён). Транспорт stop → step=-1, acc сброшен. Старт (play edge) → шаг 0 немедленно.
bool seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n);

// P-lock: есть ли оверрайд параметра id на текущем шаге? true → *out = значение оверрайда.
// Инкремент 7.1 — всегда false; 7.2 наполнит из активного шага паттерна.
bool seq_plock(const SeqEngine *e, uint16_t id, float *out);
