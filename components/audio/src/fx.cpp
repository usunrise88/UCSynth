// fx — реализация эффектов. overdrive (5.1) → delay (5.2) → reverb (5.3).
#include "fx.h"

#include <cmath>

float fx_overdrive(float x, const FxParams *p)
{
    if (!p->od_on || p->od_mix <= 0.0f) return x;   // выкл / полностью dry → байпас
    const float gain = 1.0f + p->od_drive * 11.0f;  // drive 0..1 → гейн 1..12× в шейпер
    const float wet  = tanhf(x * gain);             // мягкое насыщение, всегда [-1,1]
    return x + (wet - x) * p->od_mix;               // wet/dry (mix=1 → чистый wet, ограничен)
}

void fx_delay_init(FxState *fx, float *buf_l, float *buf_r, int len)
{
    fx->dl_l = buf_l;
    fx->dl_r = buf_r;
    fx->dl_len = len;
    fx->dl_wr = 0;
    fx->dl_lp_l = fx->dl_lp_r = 0.0f;
    if (buf_l && buf_r) {
        for (int i = 0; i < len; ++i) { buf_l[i] = 0.0f; buf_r[i] = 0.0f; }
    }
}

void fx_delay(FxState *fx, const FxParams *p, float *l, float *r, int n, float sr)
{
    if (!p->delay_on || fx->dl_l == nullptr || fx->dl_r == nullptr) return;   // off/нет буфера → dry

    const int len = fx->dl_len;
    int dlR = (int)(p->delay_time * 0.001f * sr);        // мс → сэмплы
    if (dlR < 1) dlR = 1; else if (dlR > len - 1) dlR = len - 1;
    int dlL = (int)(dlR * 0.75f);                        // L раньше R → стерео-ширина
    if (dlL < 1) dlL = 1;

    float fb = p->delay_feedback;
    if (fb < 0.0f) fb = 0.0f; else if (fb > 0.95f) fb = 0.95f;   // <1 — устойчивость
    const float coef = 1.0f - p->delay_damp * 0.9f;      // damp 0→1 (ярко), 1→0.1 (темно)
    const float mix = p->delay_mix;

    int wr = fx->dl_wr;
    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        int rL = wr - dlL; if (rL < 0) rL += len;
        int rR = wr - dlR; if (rR < 0) rR += len;
        const float dL = fx->dl_l[rL];
        const float dR = fx->dl_r[rR];

        fx->dl_lp_l += coef * (dL - fx->dl_lp_l);        // one-pole LP в цепи ОС (damp)
        fx->dl_lp_r += coef * (dR - fx->dl_lp_r);
        if (fabsf(fx->dl_lp_l) < 1e-20f) fx->dl_lp_l = 0.0f;   // денормал-флаш (LX7 медленно)
        if (fabsf(fx->dl_lp_r) < 1e-20f) fx->dl_lp_r = 0.0f;

        fx->dl_l[wr] = inL + fx->dl_lp_l * fb;
        fx->dl_r[wr] = inR + fx->dl_lp_r * fb;

        l[i] = inL + (dL - inL) * mix;                   // wet/dry
        r[i] = inR + (dR - inR) * mix;
        if (++wr >= len) wr = 0;
    }
    fx->dl_wr = wr;
}
