#include "filter.h"
#include "dsp_hot.h"
#include <cmath>

namespace { constexpr float PI = 3.14159265f; }   // не M_PI — тот double (софтфлоат на S3)

void filter_reset(Filter *f)
{
    f->ic1eq = 0.0f;
    f->ic2eq = 0.0f;
}

FiltCoef filter_coef(float cutoff_hz, float resonance, float sr, uint8_t mode)
{
    const float fmax = 0.45f * sr;                 // верх — стабильность tanf
    float fc = cutoff_hz;
    if (fc < 20.0f) fc = 20.0f;                    // низ — против денормалов g
    if (fc > fmax)  fc = fmax;

    const float g  = tanf(PI * fc / sr);
    const float k  = 2.0f - 1.98f * resonance;     // k∈[0.02,2] (floor запечён: self-osc без разноса)
    const float a1 = 1.0f / (1.0f + g * (g + k));  // реципрок — раз в блок, не в семпловом цикле
    const float a2 = g * a1;
    return FiltCoef{ g, k, a1, a2, mode };
}

float AUDIO_HOT filter_process(Filter *f, float in, const FiltCoef *c)
{
    if (c->mode == FILT_OFF) return in;            // сквозной байпас

    const float v3 = in - f->ic2eq;
    const float v1 = c->a1 * f->ic1eq + c->a2 * v3;
    const float v2 = f->ic2eq + c->g * v1;
    f->ic1eq = 2.0f * v1 - f->ic1eq;
    f->ic2eq = 2.0f * v2 - f->ic2eq;

    // Flush денормалов (хвосты release / тишина на входе) — LX7 обрабатывает их медленно.
    if (fabsf(f->ic1eq) < 1e-15f) f->ic1eq = 0.0f;
    if (fabsf(f->ic2eq) < 1e-15f) f->ic2eq = 0.0f;

    switch (c->mode) {
        case FILT_HP: return in - c->k * v1 - v2;
        case FILT_BP: return v1;
        case FILT_LP:
        default:      return v2;
    }
}
