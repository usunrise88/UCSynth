// seq_engine — темп-клок секвенсора/арпа (этап 7.1). Чистый DSP, host-тестируем.
#include "seq_engine.h"

namespace {

// Длительность шага в семплах. 16-е ноты: base = sr*60/(bpm*4). Swing сдвигает off-beat: чётный шаг
// длиннее, нечётный короче — пара шагов сохраняет темп (сумма = 2·base). k=swing·0.5 (swing=1 → 1.5/0.5).
double step_dur(int step, const SeqConfig *cfg, float sr)
{
    const int    bpm  = cfg->bpm < 1 ? 1 : cfg->bpm;
    const double base = (double)sr * 60.0 / ((double)bpm * 4.0);
    const double k    = (double)cfg->swing * 0.5;
    return (step % 2 == 0) ? base * (1.0 + k) : base * (1.0 - k);
}

}  // namespace

void seq_init(SeqEngine *e)
{
    e->running = false;
    e->step    = -1;
    e->acc     = 0.0;
    e->cur_dur = 0.0;
}

bool seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n)
{
    if (!cfg->playing) {                 // стоп → сброс транспорта
        e->running = false;
        e->step    = -1;
        e->acc     = 0.0;
        return false;
    }
    if (!e->running) {                   // play edge → шаг 0 немедленно
        e->running = true;
        e->step    = 0;
        e->acc     = 0.0;
        e->cur_dur = step_dur(e->step, cfg, sr);
        return true;
    }

    e->acc += (double)n;
    bool stepped = false;
    // При разумном bpm (≤300) за блок (~1.33 мс) пересекается ≤1 граница; while + guard — страховка.
    int guard = 0;
    while (e->acc >= e->cur_dur && guard++ < SEQ_STEPS) {
        e->acc    -= e->cur_dur;
        e->step    = (e->step + 1) % SEQ_STEPS;
        e->cur_dur = step_dur(e->step, cfg, sr);
        stepped    = true;
    }
    return stepped;
}

bool seq_plock(const SeqEngine *e, uint16_t id, float *out)
{
    (void)e; (void)id; (void)out;
    return false;   // этап 7.2 наполнит из активного шага паттерна
}
