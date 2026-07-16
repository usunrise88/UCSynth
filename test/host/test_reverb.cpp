// Host-тест reverb (Freeverb): impulse → затухающий хвост (early > late), устойчивость (нет NaN,
// ограничено), байпас (off/mix=0), гард малого буфера → реверб отключён. Буфер — malloc (в прошивке PSRAM).
#include "fx.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

int main()
{
    const int nbuf = fx_reverb_bufsize();
    check(nbuf > 20000 && nbuf < 40000, "bufsize в разумных пределах (~27–28к сэмплов)");
    float *buf = (float *)malloc(nbuf * sizeof(float));

    // impulse → хвост: энергия ранних окон > поздних (затухание)
    FxState fx{};
    fx_reverb_init(&fx, buf, nbuf);
    FxParams p{};
    p.reverb_on = true; p.reverb_size = 0.7f; p.reverb_damp = 0.3f; p.reverb_width = 1.0f; p.reverb_mix = 1.0f;
    const int N = 64;
    float l[64], r[64];
    double e_early = 0.0, e_late = 0.0;
    bool fin = true; float mx = 0.0f;
    const int blocks = 48000 * 3 / N;   // ~3 с
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < N; ++i) { const float v = (b == 0 && i == 0) ? 1.0f : 0.0f; l[i] = v; r[i] = v; }
        fx_reverb(&fx, &p, l, r, N);
        double be = 0.0;
        for (int i = 0; i < N; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) fin = false;
            be += (double)l[i] * l[i] + (double)r[i] * r[i];
            const float a = std::fabs(l[i]); if (a > mx) mx = a;
        }
        const double t = (double)(b * N) / 48000.0;
        if (t < 0.5)                 e_early += be;
        else if (t >= 1.0 && t < 1.5) e_late += be;
    }
    check(fin, "без NaN/inf");
    check(mx < 10.0f, "выход ограничен");
    check(e_early > 0.0, "impulse → есть реверб-хвост");
    check(e_late < e_early, "энергия затухает (early > late)");

    // --- усиление: нормированный вход (пост. 1.0). mix=0.01 → ≈ сухой; mix=1 → умеренный wet, И уровень
    //     ~НЕЗАВИСИМ от size (fb-компенсация: большая комната = длиннее хвост, а НЕ громче — фикс «очень громко»).
    {
        float a[64], b[64];
        // подать постоянный вход blocks×, вернуть установившийся выход (левый канал)
        auto settle = [&](FxParams &pp, int blocks) -> float {
            fx_reverb_init(&fx, buf, nbuf);
            float last = 0.0f;
            for (int blk = 0; blk < blocks; ++blk) { for (int i = 0; i < N; ++i) { a[i] = 1.0f; b[i] = 1.0f; } fx_reverb(&fx, &pp, a, b, N); last = a[N - 1]; }
            return last;
        };
        FxParams p01{}; p01.reverb_on = true; p01.reverb_size = 0.5f; p01.reverb_damp = 0.0f; p01.reverb_width = 1.0f; p01.reverb_mix = 0.01f;
        check(settle(p01, 3000) > 0.85f && settle(p01, 3000) < 1.15f, "reverb mix=0.01 → ≈ сухой");

        FxParams sm{}; sm.reverb_on = true; sm.reverb_size = 0.3f; sm.reverb_damp = 0.0f; sm.reverb_width = 1.0f; sm.reverb_mix = 1.0f;
        FxParams big = sm; big.reverb_size = 0.9f;                 // большая комната
        const float wsmall = settle(sm, 3000);
        const float wbig   = settle(big, 8000);                   // больше fb → дольше устаканивается
        check(wsmall > 0.2f && wsmall < 1.0f, "reverb mix=1 малая комната → умеренный wet (~0.5)");
        check(wbig   > 0.2f && wbig   < 1.0f, "reverb mix=1 большая комната → умеренный wet (НЕ громче малой)");
        check(std::fabs(wbig - wsmall) < 0.25f, "уровень wet ~независим от size (fb-компенсация)");
    }

    // off → dry
    fx_reverb_init(&fx, buf, nbuf);
    FxParams off{}; off.reverb_on = false;
    {
        float a[3] = {0.5f, -0.3f, 0.2f}, b2[3] = {0.5f, -0.3f, 0.2f};
        fx_reverb(&fx, &off, a, b2, 3);
        check(a[0] == 0.5f && a[1] == -0.3f && a[2] == 0.2f, "off → dry");
    }

    // mix=0 → dry (хвост считается, но выход сухой)
    fx_reverb_init(&fx, buf, nbuf);
    FxParams m0{}; m0.reverb_on = true; m0.reverb_size = 0.7f; m0.reverb_mix = 0.0f;
    {
        float a[3] = {0.5f, -0.3f, 0.2f}, b2[3] = {0.5f, -0.3f, 0.2f};
        fx_reverb(&fx, &m0, a, b2, 3);
        check(a[0] == 0.5f && a[1] == -0.3f && a[2] == 0.2f, "mix=0 → dry");
    }

    // малый буфер → реверб отключён (no-op, dry)
    {
        FxState fx2{};
        fx_reverb_init(&fx2, buf, nbuf - 1);   // nsamples < нужного → отключено
        FxParams pp{}; pp.reverb_on = true; pp.reverb_size = 0.7f; pp.reverb_mix = 1.0f;
        float a[2] = {0.4f, 0.6f}, b2[2] = {0.4f, 0.6f};
        fx_reverb(&fx2, &pp, a, b2, 2);
        check(a[0] == 0.4f && a[1] == 0.6f, "малый буфер → реверб отключён (dry)");
    }

    free(buf);
    if (g_fail == 0) printf("OK: reverb — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
