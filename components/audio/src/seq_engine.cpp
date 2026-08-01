// seq_engine — темп-клок + степ-секвенсор (этапы 7.1/7.2). Чистый DSP, host-тестируем.
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

inline uint32_t xs(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float rng01(uint32_t &s) { return (float)(xs(s) & 0xFFFFFFu) * (1.0f / 16777216.0f); } // [0,1)

// Снять звучащие ноты в события (release).
void release_sounding(SeqEngine *e, SeqNoteEvent *ev, int evcap, int *evn)
{
    for (int i = 0; i < e->n_sounding; ++i)
        if (*evn < evcap) ev[(*evn)++] = { 0, e->sounding[i], 0 };
    e->n_sounding = 0;
}

// Войти в e->step (уже обновлён): release прошлого шага, выставить p-lock оверрайды, взять ноты нового
// шага (active + trig-probability). p-lock применяются независимо от trig (автоматизация без ноты).
void apply_step(SeqEngine *e, SeqNoteEvent *ev, int evcap, int *evn)
{
    release_sounding(e, ev, evcap, evn);

    const SeqStep &st = e->pattern[e->step];
    e->n_ov = st.n_plocks < SEQ_MAX_PLOCKS ? st.n_plocks : SEQ_MAX_PLOCKS;
    for (int i = 0; i < e->n_ov; ++i) { e->ov_id[i] = st.plocks[i].id; e->ov_val[i] = st.plocks[i].val; }

    if (st.active && (st.trig_prob >= 1.0f || rng01(e->rng) < st.trig_prob)) {
        int k = 0;
        for (int i = 0; i < st.n_notes && k < SEQ_MAX_NOTES; ++i) {
            if (*evn < evcap) ev[(*evn)++] = { 1, st.notes[i], st.velocity };
            e->sounding[k++] = st.notes[i];
        }
        e->n_sounding = (uint8_t)k;
    }
}

}  // namespace

void seq_init(SeqEngine *e)
{
    e->running = false;
    e->step    = -1;
    e->acc     = 0.0;
    e->cur_dur = 0.0;
    for (int s = 0; s < SEQ_STEPS; ++s) e->pattern[s] = SeqStep{};
    e->n_sounding = 0;
    e->n_ov       = 0;
    e->rng        = 0x53510F27u;   // фикс. сид (детерминизм; тест может переопределить e->rng)
}

int seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n, SeqNoteEvent *ev, int evcap)
{
    int evn = 0;

    // Транспорт (клок общий на секвенсор и арп): стоп → снять звучащие, сбросить.
    if (!cfg->playing) {
        if (e->running) { release_sounding(e, ev, evcap, &evn); e->n_ov = 0; }
        e->running = false;
        e->step    = -1;
        e->acc     = 0.0;
        return evn;
    }

    // Продвижение клока (при разумном bpm ≤1 граница за блок; while + guard — страховка).
    bool boundary = false;
    if (!e->running) {                   // play edge → шаг 0
        e->running = true;
        e->step    = 0;
        e->acc     = 0.0;
        e->cur_dur = step_dur(e->step, cfg, sr);
        boundary   = true;
    } else {
        e->acc += (double)n;
        int guard = 0;
        while (e->acc >= e->cur_dur && guard++ < SEQ_STEPS) {
            e->acc    -= e->cur_dur;
            e->step    = (e->step + 1) % SEQ_STEPS;
            e->cur_dur = step_dur(e->step, cfg, sr);
            boundary   = true;
        }
    }

    // Секвенсор потребляет клок при seq_on. Выключен (клок идёт для арпа) → снять его ноты/оверрайды.
    if (cfg->seq_on) {
        if (boundary) apply_step(e, ev, evcap, &evn);
    } else if (e->n_sounding || e->n_ov) {
        release_sounding(e, ev, evcap, &evn);
        e->n_ov = 0;
    }
    // Арпеджиатор — этап 7.3 (тот же клок, отдельный продюсер, деление по arp_rate).
    return evn;
}

bool seq_plock(const SeqEngine *e, uint16_t id, float *out)
{
    for (int i = 0; i < e->n_ov; ++i)
        if (e->ov_id[i] == id) { *out = e->ov_val[i]; return true; }
    return false;
}
