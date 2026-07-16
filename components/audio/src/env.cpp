#include "env.h"
#include <cmath>

namespace {
constexpr float K_COEF   = 4.6f;    // ln(100): сегмент проходит ~99% пути за своё "время"
constexpr float EPS      = 0.005f;  // порог «дошли до цели» → смена стадии
constexpr float MIN_TIME = 0.001f;  // мин. время сегмента (защита от dt/time → +inf)
constexpr float ATK_TGT  = 1.0f;    // цель атаки

// Коэффициент одного полюса к цели за dt при постоянной «времени» time.
inline float seg_coef(float time, float dt)
{
    if (time < MIN_TIME) time = MIN_TIME;
    return 1.0f - expf(-K_COEF * dt / time);
}
}  // namespace

void env_reset(Env *e)
{
    e->stage     = ENV_IDLE;
    e->level     = 0.0f;
    e->prev_gate = false;
}

float env_tick(Env *e, const EnvParams *p, bool gate, float dt)
{
    // Фронты gate: attack идёт ОТ текущего уровня (ретригер из release без щелчка вместе с VCA-лерпом).
    if (gate && !e->prev_gate)      e->stage = ENV_ATTACK;
    else if (!gate && e->prev_gate) e->stage = ENV_RELEASE;
    e->prev_gate = gate;

    switch (e->stage) {
        case ENV_IDLE:
            e->level = 0.0f;
            break;
        case ENV_ATTACK:
            e->level += (ATK_TGT - e->level) * seg_coef(p->a, dt);
            if (e->level >= 1.0f - EPS) { e->level = 1.0f; e->stage = ENV_DECAY; }
            break;
        case ENV_DECAY:
            e->level += (p->s - e->level) * seg_coef(p->d, dt);
            if (fabsf(e->level - p->s) < EPS) {
                e->level = p->s;
                e->stage = p->loop ? ENV_ATTACK : ENV_SUSTAIN;   // loop → цикл A→D→A (дрон)
            }
            break;
        case ENV_SUSTAIN:
            e->level = p->s;
            break;
        case ENV_RELEASE:
            e->level += (0.0f - e->level) * seg_coef(p->r, dt);
            if (e->level < EPS) { e->level = 0.0f; e->stage = ENV_IDLE; }
            break;
    }
    return e->level;
}
