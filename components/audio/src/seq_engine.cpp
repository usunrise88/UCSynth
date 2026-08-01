// seq_engine — темп-клок + степ-секвенсор + арпеджиатор (этапы 7.1–7.3). Чистый DSP, host-тестируем.
#include "seq_engine.h"

namespace {

// Длительность шага секвенсора (16-е). base = sr*60/(bpm*4). Swing сдвигает off-beat: чётный длиннее,
// нечётный короче — пара сохраняет темп (сумма = 2·base). k=swing·0.5 (swing=1 → 1.5/0.5).
double step_dur(int step, const SeqConfig *cfg, float sr)
{
    const int    bpm  = cfg->bpm < 1 ? 1 : cfg->bpm;
    const double base = (double)sr * 60.0 / ((double)bpm * 4.0);
    const double k    = (double)cfg->swing * 0.5;
    return (step % 2 == 0) ? base * (1.0 + k) : base * (1.0 - k);
}

// Длительность арп-шага. rate: 0=1/4 1=1/8 2=1/16 3=1/32 — относительно четверти (sr*60/bpm).
double arp_dur_samples(const SeqConfig *cfg, float sr)
{
    static const double mul[4] = { 1.0, 0.5, 0.25, 0.125 };
    const int    bpm     = cfg->bpm < 1 ? 1 : cfg->bpm;
    const int    r       = cfg->arp_rate < 0 ? 0 : (cfg->arp_rate > 3 ? 3 : cfg->arp_rate);
    const double quarter = (double)sr * 60.0 / (double)bpm;
    return quarter * mul[r];
}

inline uint32_t xs(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline float rng01(uint32_t &s) { return (float)(xs(s) & 0xFFFFFFu) * (1.0f / 16777216.0f); } // [0,1)

void emit_off(SeqNoteEvent *ev, int evcap, int *evn, uint8_t note)
{
    if (*evn < evcap) ev[(*evn)++] = { 0, note, 0 };
}

void release_sounding(SeqEngine *e, SeqNoteEvent *ev, int evcap, int *evn)
{
    for (int i = 0; i < e->n_sounding; ++i) emit_off(ev, evcap, evn, e->sounding[i]);
    e->n_sounding = 0;
}

// Секвенсор: войти в e->step (уже обновлён) — release прошлого, p-lock оверрайды, ноты нового шага.
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

// Собрать упорядоченный арп-пул из held (по возрастанию) × октавы. Возврат — размер (≤ cap).
int build_arp_pool(const SeqEngine *e, const SeqConfig *cfg, uint8_t *pool, int cap)
{
    uint8_t sorted[SEQ_ARP_MAX_HELD];
    const int m = e->n_held;
    for (int i = 0; i < m; ++i) sorted[i] = e->held[i];
    for (int i = 1; i < m; ++i) {                 // insertion sort (m ≤ 12)
        uint8_t v = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; --j; }
        sorted[j + 1] = v;
    }
    const int oct = cfg->arp_octaves < 1 ? 1 : (cfg->arp_octaves > 4 ? 4 : cfg->arp_octaves);
    int np = 0;
    for (int o = 0; o < oct; ++o)
        for (int i = 0; i < m; ++i) {
            const int nn = sorted[i] + 12 * o;
            if (nn < 128 && np < cap) pool[np++] = (uint8_t)nn;
        }
    return np;
}

// Арпеджиатор: снять прошлую арп-ноту, взять следующую по режиму.
void arp_fire(SeqEngine *e, const SeqConfig *cfg, SeqNoteEvent *ev, int evcap, int *evn)
{
    if (e->arp_sounding != 255) { emit_off(ev, evcap, evn, e->arp_sounding); e->arp_sounding = 255; }

    uint8_t pool[SEQ_ARP_MAX_HELD * 4];
    const int np = build_arp_pool(e, cfg, pool, (int)sizeof(pool));
    if (np == 0) return;

    if (e->arp_pos < 0 || e->arp_pos >= np) e->arp_pos = 0;   // held мог измениться
    uint8_t note;
    switch (cfg->arp_mode) {
        case ARP_DOWN:
            note = pool[np - 1 - e->arp_pos];
            e->arp_pos = (e->arp_pos + 1) % np;
            break;
        case ARP_UPDOWN:
            note = pool[e->arp_pos];
            if (np > 1) {
                e->arp_pos += e->arp_dir;
                if (e->arp_pos >= np - 1) { e->arp_pos = np - 1; e->arp_dir = -1; }
                else if (e->arp_pos <= 0) { e->arp_pos = 0; e->arp_dir = 1; }
            }
            break;
        case ARP_RANDOM:
            note = pool[xs(e->arp_rng) % (uint32_t)np];
            break;
        case ARP_UP:
        default:
            note = pool[e->arp_pos];
            e->arp_pos = (e->arp_pos + 1) % np;
            break;
    }
    if (*evn < evcap) ev[(*evn)++] = { 1, note, 100 };   // велосити арпа фикс. (velocity — p-lock у секв.)
    e->arp_sounding = note;
}

}  // namespace

void seq_init(SeqEngine *e)
{
    e->running = false; e->step = -1; e->acc = 0.0; e->cur_dur = 0.0;
    for (int s = 0; s < SEQ_STEPS; ++s) e->pattern[s] = SeqStep{};
    e->n_sounding = 0; e->n_ov = 0;
    e->rng = 0x53510F27u;
    // арп
    e->arp_running = false; e->arp_acc = 0.0; e->arp_dur = 0.0;
    e->n_held = 0; e->phys_down = 0;
    e->arp_pos = 0; e->arp_dir = 1; e->arp_sounding = 255;
    e->arp_rng = 0x1BADF00Du;
}

void seq_arp_note(SeqEngine *e, uint8_t note, bool on, bool hold)
{
    if (on) {
        if (hold && e->phys_down == 0) e->n_held = 0;   // новое нажатие после полного отпускания → заново
        bool present = false;
        for (int i = 0; i < e->n_held; ++i) if (e->held[i] == note) present = true;
        if (!present && e->n_held < SEQ_ARP_MAX_HELD) e->held[e->n_held++] = note;
        ++e->phys_down;
    } else {
        if (e->phys_down > 0) --e->phys_down;
        if (!hold) {                                    // без hold — убрать из held
            for (int i = 0; i < e->n_held; ++i)
                if (e->held[i] == note) { for (int j = i; j + 1 < e->n_held; ++j) e->held[j] = e->held[j + 1]; --e->n_held; break; }
        }
    }
}

int seq_tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n, SeqNoteEvent *ev, int evcap)
{
    int evn = 0;

    // Транспорт: стоп → снять всё звучащее (секвенсор + арп), сбросить клоки.
    if (!cfg->playing) {
        if (e->running) { release_sounding(e, ev, evcap, &evn); e->n_ov = 0; }
        if (e->arp_sounding != 255) { emit_off(ev, evcap, &evn, e->arp_sounding); e->arp_sounding = 255; }
        e->running = false; e->step = -1; e->acc = 0.0;
        e->arp_running = false; e->arp_acc = 0.0;
        return evn;
    }

    // --- секвенсор: клок 16-х ---
    bool boundary = false;
    if (!e->running) {
        e->running = true; e->step = 0; e->acc = 0.0; e->cur_dur = step_dur(e->step, cfg, sr);
        boundary = true;
    } else {
        e->acc += (double)n;
        int guard = 0;
        while (e->acc >= e->cur_dur && guard++ < SEQ_STEPS) {
            e->acc -= e->cur_dur; e->step = (e->step + 1) % SEQ_STEPS;
            e->cur_dur = step_dur(e->step, cfg, sr); boundary = true;
        }
    }
    if (cfg->seq_on) {
        if (boundary) apply_step(e, ev, evcap, &evn);
    } else if (e->n_sounding || e->n_ov) {
        release_sounding(e, ev, evcap, &evn); e->n_ov = 0;
    }

    // --- арпеджиатор: собственный клок (деление arp_rate) ---
    if (cfg->arp_on) {
        if (!e->arp_running) {
            e->arp_running = true; e->arp_acc = 0.0; e->arp_dur = arp_dur_samples(cfg, sr);
            e->arp_pos = 0; e->arp_dir = 1;
            arp_fire(e, cfg, ev, evcap, &evn);
        } else {
            e->arp_acc += (double)n;
            int guard = 0;
            while (e->arp_acc >= e->arp_dur && guard++ < 64) {
                e->arp_acc -= e->arp_dur; e->arp_dur = arp_dur_samples(cfg, sr);
                arp_fire(e, cfg, ev, evcap, &evn);
            }
        }
    } else if (e->arp_sounding != 255 || e->arp_running) {
        if (e->arp_sounding != 255) { emit_off(ev, evcap, &evn, e->arp_sounding); e->arp_sounding = 255; }
        e->arp_running = false;
    }

    return evn;
}

bool seq_plock(const SeqEngine *e, uint16_t id, float *out)
{
    for (int i = 0; i < e->n_ov; ++i)
        if (e->ov_id[i] == id) { *out = e->ov_val[i]; return true; }
    return false;
}
