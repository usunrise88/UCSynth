// fx — реализация эффектов. overdrive (5.1) → delay (5.2) → reverb (5.3).
#include "fx.h"
#include "dsp_hot.h"

#include <cmath>

float AUDIO_HOT fx_overdrive(float x, const FxParams *p)
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

void AUDIO_HOT fx_delay(FxState *fx, const FxParams *p, float *l, float *r, int n, float sr)
{
    if (!p->delay_on || fx->dl_l == nullptr || fx->dl_r == nullptr) return;   // off/нет буфера → dry

    const int len = fx->dl_len;
    int d = (int)(p->delay_time * 0.001f * sr);          // мс → сэмплы; ЕДИНОЕ время обоих каналов
    if (d < 1) d = 1; else if (d > len - 1) d = len - 1;

    float fb = p->delay_feedback;
    if (fb < 0.0f) fb = 0.0f; else if (fb > 0.95f) fb = 0.95f;   // <1 — устойчивость (кольцо fb² на 2·d)
    const float coef = 1.0f - p->delay_damp * 0.9f;      // damp 0→1 (ярко), 1→0.1 (темно)
    const float mix = p->delay_mix;

    int wr = fx->dl_wr;
    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        int rd = wr - d; if (rd < 0) rd += len;          // общий отсчёт чтения для обеих линий
        const float dL = fx->dl_l[rd];
        const float dR = fx->dl_r[rd];

        fx->dl_lp_l += coef * (dL - fx->dl_lp_l);        // one-pole LP в цепи ОС (damp) на каждую линию
        fx->dl_lp_r += coef * (dR - fx->dl_lp_r);
        if (fabsf(fx->dl_lp_l) < 1e-20f) fx->dl_lp_l = 0.0f;   // денормал-флаш (LX7 медленно)
        if (fabsf(fx->dl_lp_r) < 1e-20f) fx->dl_lp_r = 0.0f;

        // Пинг-понг: вход (моно-сумма) идёт в ЛЕВУЮ линию; обратная связь ПЕРЕКРЁСТНАЯ (левый хвост кормит
        // правую линию, правый — левую) → ОДНО эхо на delay_time, скачущее L↔R (и суммируясь в моно — чистый
        // ряд эхо). Раньше были две НЕзависимые линии с разным временем (dlL=0.75·dlR) → на моно-входе
        // слышались два эха сразу = «лишние срабатывания».
        const float in = (inL + inR) * 0.5f;
        fx->dl_l[wr] = in + fx->dl_lp_r * fb;            // левая линия: вход + затухший ПРАВЫЙ хвост
        fx->dl_r[wr] =      fx->dl_lp_l * fb;            // правая линия: только затухший ЛЕВЫЙ хвост

        l[i] = inL + (dL - inL) * mix;                   // dry (по центру) + wet: левый хвост на L,
        r[i] = inR + (dR - inR) * mix;                   //                       правый хвост на R
        if (++wr >= len) wr = 0;
    }
    fx->dl_wr = wr;
}

// ---- reverb (Freeverb) ---------------------------------------------------------------------------
namespace {
// Тюнинг Freeverb (Jezar), пересчитан на 48 кГц (×48000/44100). R-канал смещён на RV_SPREAD → стерео.
const int RV_COMB_LEN[RV_NCOMB] = {1214, 1293, 1390, 1476, 1548, 1623, 1695, 1760};
const int RV_AP_LEN[RV_NAP]     = {605, 480, 371, 245};
constexpr int   RV_SPREAD      = 25;
constexpr float RV_FIXEDGAIN   = 0.015f;   // масштаб входа в гребёнки (чтобы не перегружать)
constexpr float RV_ROOM_SCALE  = 0.28f;    // size → feedback ∈ [0.7, 0.98]
constexpr float RV_ROOM_OFFSET = 0.7f;
constexpr float RV_DAMP_SCALE  = 0.4f;     // damp → damp1 ∈ [0, 0.4]

inline float comb_tick(Comb &c, float in, float fb, float damp) {
    const float out = c.buf[c.idx];
    c.store = out * (1.0f - damp) + c.store * damp;      // one-pole LP в цепи ОС
    if (fabsf(c.store) < 1e-20f) c.store = 0.0f;         // денормал-флаш
    c.buf[c.idx] = in + c.store * fb;
    if (++c.idx >= c.len) c.idx = 0;
    return out;
}
inline float allpass_tick(Allpass &a, float in) {
    float bufout = a.buf[a.idx];
    if (fabsf(bufout) < 1e-20f) bufout = 0.0f;
    const float out = -in + bufout;
    a.buf[a.idx] = in + bufout * 0.5f;                   // фикс. allpass feedback
    if (++a.idx >= a.len) a.idx = 0;
    return out;
}
}  // namespace

int fx_reverb_bufsize()
{
    int total = 0;
    for (int i = 0; i < RV_NCOMB; ++i) total += RV_COMB_LEN[i] + (RV_COMB_LEN[i] + RV_SPREAD);
    for (int i = 0; i < RV_NAP; ++i)   total += RV_AP_LEN[i]   + (RV_AP_LEN[i]   + RV_SPREAD);
    return total;
}

void fx_reverb_init(FxState *fx, float *buf, int nsamples)
{
    for (int i = 0; i < RV_NCOMB; ++i) { fx->rv_combL[i] = {nullptr, 0, 0, 0.0f}; fx->rv_combR[i] = {nullptr, 0, 0, 0.0f}; }
    for (int i = 0; i < RV_NAP; ++i)   { fx->rv_apL[i]   = {nullptr, 0, 0};       fx->rv_apR[i]   = {nullptr, 0, 0}; }
    if (!buf || nsamples < fx_reverb_bufsize()) return;   // нет/мал буфер → реверб отключён

    for (int i = 0; i < nsamples; ++i) buf[i] = 0.0f;
    int off = 0;
    auto slice = [&](int n) -> float * { float *p = buf + off; off += n; return p; };
    for (int i = 0; i < RV_NCOMB; ++i) {
        fx->rv_combL[i] = { slice(RV_COMB_LEN[i]),              RV_COMB_LEN[i],              0, 0.0f };
        fx->rv_combR[i] = { slice(RV_COMB_LEN[i] + RV_SPREAD), RV_COMB_LEN[i] + RV_SPREAD, 0, 0.0f };
    }
    for (int i = 0; i < RV_NAP; ++i) {
        fx->rv_apL[i] = { slice(RV_AP_LEN[i]),              RV_AP_LEN[i],              0 };
        fx->rv_apR[i] = { slice(RV_AP_LEN[i] + RV_SPREAD), RV_AP_LEN[i] + RV_SPREAD, 0 };
    }
}

void AUDIO_HOT fx_reverb(FxState *fx, const FxParams *p, float *l, float *r, int n)
{
    if (!p->reverb_on || fx->rv_combL[0].buf == nullptr) return;   // off/нет буфера → dry

    const float fb   = p->reverb_size * RV_ROOM_SCALE + RV_ROOM_OFFSET;
    const float damp = p->reverb_damp * RV_DAMP_SCALE;
    const float wet1 = p->reverb_width * 0.5f + 0.5f;   // ширина: моно (0) → wet1=0.5,wet2=0.5; стерео (1) → 1,0
    const float wet2 = (1.0f - p->reverb_width) * 0.5f;
    const float mix  = p->reverb_mix;

    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        const float input = (inL + inR) * RV_FIXEDGAIN;   // моно-вход в оба банка гребёнок

        float outL = 0.0f, outR = 0.0f;
        for (int k = 0; k < RV_NCOMB; ++k) {              // параллельные гребёнки
            outL += comb_tick(fx->rv_combL[k], input, fb, damp);
            outR += comb_tick(fx->rv_combR[k], input, fb, damp);
        }
        for (int k = 0; k < RV_NAP; ++k) {                // последовательные allpass
            outL = allpass_tick(fx->rv_apL[k], outL);
            outR = allpass_tick(fx->rv_apR[k], outR);
        }
        const float wetL = outL * wet1 + outR * wet2;
        const float wetR = outR * wet1 + outL * wet2;
        l[i] = inL + (wetL - inL) * mix;                  // wet/dry
        r[i] = inR + (wetR - inR) * mix;
    }
}
