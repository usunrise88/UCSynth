#include "synth.h"
#include "dsp_hot.h"
#include <cmath>

namespace {

constexpr int SYNTH_MAX_BLOCK = 128;   // верх диапазона блока (32–128); scratch на стеке рендера
constexpr int STACK_DEPTH     = 10;    // моно: глубина стека зажатых нот (все пальцы)

Voice    s_voices[SYNTH_MAX_VOICES];
uint32_t s_age[SYNTH_MAX_VOICES];      // «возраст» аллокации голоса (для oldest-steal)
uint32_t s_age_ctr = 0;
int      s_next_rr = 0;                 // round-robin для поиска свободного

uint8_t  s_stack[STACK_DEPTH];          // моно: стек зажатых нот (верх = звучит)
int      s_stack_n = 0;

int      s_prev_poly = -1;              // для детекта смены режима polyphony на лету (-1 = первый рендер)

// Голос «живой» (звучит или в релизе): считать в микс и НЕ выдавать как свободный.
// key_down — свежий голос ещё в ENV_IDLE до первого тика, но уже должен звучать (иначе — тишина).
inline bool active(const Voice *v)
{
    return v->key_down || v->latch_held || v->env_amp.stage != ENV_IDLE;
}

void stack_remove(uint8_t note)
{
    for (int i = 0; i < s_stack_n; ++i) {
        if (s_stack[i] == note) {
            for (int k = i; k < s_stack_n - 1; ++k) s_stack[k] = s_stack[k + 1];
            --s_stack_n;
            return;
        }
    }
}

void stack_push(uint8_t note)
{
    if (s_stack_n >= STACK_DEPTH) {                     // переполнение — выкинуть старейшую (низ)
        for (int k = 0; k < STACK_DEPTH - 1; ++k) s_stack[k] = s_stack[k + 1];
        --s_stack_n;
    }
    s_stack[s_stack_n++] = note;
}

// Ближайший по высоте звучащий голос (для poly-glide «по ближайшему»), кроме excl. -1 если нет.
int nearest_sounding(float note, int poly, int excl)
{
    int   best = -1;
    float bestd = 1e9f;
    for (int i = 0; i < poly; ++i) {
        if (i == excl || !active(&s_voices[i])) continue;
        const float d = fabsf(s_voices[i].cur_note - note);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// Выбрать голос под новую ноту: свободный (round-robin) → релиз с мин. уровнем → старейший.
int alloc_voice(int poly)
{
    for (int c = 0; c < poly; ++c) {                   // 1) свободный, round-robin
        const int i = (s_next_rr + c) % poly;
        const Voice *v = &s_voices[i];
        if (!v->key_down && !v->latch_held && v->env_amp.stage == ENV_IDLE) {
            s_next_rr = (i + 1) % poly;
            return i;
        }
    }
    int best = -1;                                     // 2) увести релиз с мин. уровнем
    float bestlvl = 1e9f;
    for (int i = 0; i < poly; ++i) {
        if (s_voices[i].env_amp.stage == ENV_RELEASE && s_voices[i].env_amp.level < bestlvl) {
            bestlvl = s_voices[i].env_amp.level; best = i;
        }
    }
    if (best >= 0) return best;

    int oldest = 0;                                    // 3) увести старейший
    uint32_t bestage = s_age[0];
    for (int i = 1; i < poly; ++i) if (s_age[i] < bestage) { bestage = s_age[i]; oldest = i; }
    return oldest;
}

void all_notes_off(void)
{
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i) { s_voices[i].key_down = false; s_voices[i].latch_held = false; }
    s_stack_n = 0;
}

}  // namespace

void synth_init(void)
{
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i) { voice_init(&s_voices[i], 0x2468ACEu + (uint32_t)i * 0x9E3779B9u); s_age[i] = 0; }
    s_age_ctr = 0; s_next_rr = 0; s_stack_n = 0; s_prev_poly = -1;   // -1 = первый рендер (без сброса)
}

void synth_note_on(const SynthParams *sp, uint8_t note, uint8_t vel)
{
    const int   poly    = sp->poly_voices < 1 ? 1 : (sp->poly_voices > SYNTH_MAX_VOICES ? SYNTH_MAX_VOICES : sp->poly_voices);
    const bool  latch   = sp->voice.latch;
    const bool  do_gl   = sp->voice.glide_time > 0.0f;

    if (poly == 1) {                                   // --- моно: стек нот ---
        const bool had = s_stack_n > 0;
        stack_remove(note);
        stack_push(note);
        Voice *v = &s_voices[0];
        if (sp->legato && had) {                       // legato-перекрытие: скольжение без ретригера
            voice_slide(v, note, do_gl);
            v->key_down = true;
            if (latch) v->latch_held = true;
        } else {
            voice_note_on(v, note, vel, latch, do_gl && had);   // первая нота — снап (had=false)
        }
        return;
    }

    // --- поли ---
    for (int i = 0; i < poly; ++i) {                   // reuse: голос уже играет эту ноту → ретригер
        if (s_voices[i].note == note && active(&s_voices[i]) && s_voices[i].key_down) {
            voice_note_on(&s_voices[i], note, vel, latch, false);   // та же высота → снап
            s_age[i] = ++s_age_ctr;
            return;
        }
    }
    const int i = alloc_voice(poly);
    bool glide = false;
    if (do_gl) {                                       // poly-glide: засеять высоту от ближайшего звучащего
        const int nb = nearest_sounding((float)note, poly, i);
        if (nb >= 0) { s_voices[i].cur_note = s_voices[nb].cur_note; glide = true; }
    }
    voice_note_on(&s_voices[i], note, vel, latch, glide);
    s_age[i] = ++s_age_ctr;
}

void synth_note_off(const SynthParams *sp, uint8_t note)
{
    const int  poly  = sp->poly_voices < 1 ? 1 : (sp->poly_voices > SYNTH_MAX_VOICES ? SYNTH_MAX_VOICES : sp->poly_voices);
    const bool do_gl = sp->voice.glide_time > 0.0f;

    if (poly == 1) {                                   // моно: убрать ноту, вернуться на верх стека
        stack_remove(note);
        Voice *v = &s_voices[0];
        if (s_stack_n > 0) voice_slide(v, s_stack[s_stack_n - 1], do_gl);   // возврат — без ретригера
        else               v->key_down = false;        // стек пуст → gate off (latch держит до снятия)
        return;
    }
    for (int i = 0; i < poly; ++i)                     // поли: отпустить голос(а) с этой нотой
        if (s_voices[i].note == note && s_voices[i].key_down) voice_note_off(&s_voices[i], note);
}

void AUDIO_HOT synth_render(const SynthParams *sp, float sr, float *out, int n)
{
    int poly = sp->poly_voices < 1 ? 1 : (sp->poly_voices > SYNTH_MAX_VOICES ? SYNTH_MAX_VOICES : sp->poly_voices);
    if (poly != s_prev_poly) {                          // смена режима polyphony на лету → сброс нот
        if (s_prev_poly >= 0) all_notes_off();          // но не на первом рендере (сентинел -1)
        s_prev_poly = poly;
    }

    for (int k = 0; k < n; ++k) out[k] = 0.0f;

    float tmp[SYNTH_MAX_BLOCK];
    if (n > SYNTH_MAX_BLOCK) n = SYNTH_MAX_BLOCK;      // страховка (аудио-блок ≤ 128)
    for (int i = 0; i < poly; ++i) {                   // только активные голоса (CPU ~ числу нот)
        if (!active(&s_voices[i])) continue;
        voice_render(&s_voices[i], &sp->voice, sr, tmp, n);
        for (int k = 0; k < n; ++k) out[k] += tmp[k];
    }
}

int synth_active_count(void)
{
    int c = 0;
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i) if (active(&s_voices[i])) ++c;
    return c;
}
