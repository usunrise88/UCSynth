// Host-тест delay: эхо на delay_time, затухание по feedback, байпас (off/mix=0), устойчивость при
// большом feedback. Буфер — обычный malloc (в прошивке delay-буферы живут в PSRAM).
#include "fx.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

int main()
{
    const float sr  = 48000.0f;
    const int   len = 48000;               // 1 с
    float *bl = (float *)malloc(len * sizeof(float));
    float *br = (float *)malloc(len * sizeof(float));
    const int N = 64;
    float l[64], r[64];

    // --- эхо на delay_time (полный wet), затухание второго эха по feedback ---
    FxState fx;
    fx_delay_init(&fx, bl, br, len);
    FxParams p{};
    p.delay_on = true; p.delay_time = 10.0f; p.delay_feedback = 0.5f; p.delay_damp = 0.0f; p.delay_mix = 1.0f;
    const int dlR   = (int)(p.delay_time * 0.001f * sr);   // 480
    const int total = dlR * 3 + 300;
    float *outR = (float *)calloc(total, sizeof(float));
    for (int gi = 0; gi < total;) {
        for (int i = 0; i < N; ++i) { const float v = (gi + i == 0) ? 1.0f : 0.0f; l[i] = v; r[i] = v; }
        fx_delay(&fx, &p, l, r, N, sr);
        for (int i = 0; i < N && gi + i < total; ++i) outR[gi + i] = r[i];
        gi += N;
    }
    int pk = -1; float pkv = 0.0f;
    for (int i = 1; i < dlR + dlR / 2; ++i) { const float a = std::fabs(outR[i]); if (a > pkv) { pkv = a; pk = i; } }
    check(pk >= dlR - 2 && pk <= dlR + 2, "эхо на delay_time");
    check(pkv > 0.9f, "эхо ~ полной амплитуды при mix=1");
    float e2 = 0.0f;
    for (int i = 2 * dlR - 3; i <= 2 * dlR + 3 && i < total; ++i) { const float a = std::fabs(outR[i]); if (a > e2) e2 = a; }
    check(e2 > 0.1f && e2 < pkv, "второе эхо затухает (feedback)");

    // --- усиление: нормированный вход (пост. 1.0). mix=0.01 → ≈ сухой (не раздут); mix=1 → wet ограничен ---
    {
        float a[64], b[64]; float last = 0.0f;
        fx_delay_init(&fx, bl, br, len);
        FxParams lo{}; lo.delay_on = true; lo.delay_time = 5.0f; lo.delay_feedback = 0.35f; lo.delay_damp = 0.0f; lo.delay_mix = 0.01f;
        for (int blk = 0; blk < 1000; ++blk) { for (int i = 0; i < N; ++i) { a[i] = 1.0f; b[i] = 1.0f; } fx_delay(&fx, &lo, a, b, N, sr); last = a[N - 1]; }
        check(last > 0.9f && last < 1.15f, "delay mix=0.01 → выход ≈ сухой (не раздувается)");

        fx_delay_init(&fx, bl, br, len);
        FxParams hi1{}; hi1.delay_on = true; hi1.delay_time = 5.0f; hi1.delay_feedback = 0.35f; hi1.delay_damp = 0.0f; hi1.delay_mix = 1.0f;
        for (int blk = 0; blk < 1000; ++blk) { for (int i = 0; i < N; ++i) { a[i] = 1.0f; b[i] = 1.0f; } fx_delay(&fx, &hi1, a, b, N, sr); last = a[N - 1]; }
        check(last > 1.0f && last < 3.0f, "delay mix=1 → wet ограничен (~1/(1-fb))");
    }

    // --- off → dry passthrough ---
    fx_delay_init(&fx, bl, br, len);
    FxParams off{}; off.delay_on = false; off.delay_time = 100.0f; off.delay_mix = 1.0f;
    {
        float a[4] = {0.3f, -0.7f, 0.5f, 0.1f}, b[4] = {0.3f, -0.7f, 0.5f, 0.1f};
        fx_delay(&fx, &off, a, b, 4, sr);
        check(a[0] == 0.3f && a[1] == -0.7f && a[2] == 0.5f && a[3] == 0.1f, "off → dry passthrough");
    }

    // --- mix=0 → dry (буфер пишется, но выход сухой) ---
    fx_delay_init(&fx, bl, br, len);
    FxParams m0{}; m0.delay_on = true; m0.delay_time = 100.0f; m0.delay_feedback = 0.5f; m0.delay_mix = 0.0f;
    {
        float a[3] = {0.4f, -0.2f, 0.6f}, b[3] = {0.4f, -0.2f, 0.6f};
        fx_delay(&fx, &m0, a, b, 3, sr);
        check(a[0] == 0.4f && a[1] == -0.2f && a[2] == 0.6f, "mix=0 → dry passthrough");
    }

    // --- устойчивость: feedback 0.95, длинный прогон — ограничено и конечно ---
    fx_delay_init(&fx, bl, br, len);
    FxParams hi{}; hi.delay_on = true; hi.delay_time = 5.0f; hi.delay_feedback = 0.95f; hi.delay_damp = 0.5f; hi.delay_mix = 0.5f;
    {
        float mx = 0.0f; bool fin = true;
        for (int blk = 0; blk < 4000; ++blk) {   // ~5.3 с
            for (int i = 0; i < N; ++i) { const float v = (blk == 0 && i == 0) ? 1.0f : 0.0f; l[i] = v; r[i] = v; }
            fx_delay(&fx, &hi, l, r, N, sr);
            for (int i = 0; i < N; ++i) {
                if (!std::isfinite(l[i]) || !std::isfinite(r[i])) fin = false;
                const float a = std::fabs(l[i]); if (a > mx) mx = a;
            }
        }
        check(fin, "устойчивость: без NaN/inf");
        check(mx < 10.0f, "устойчивость: выход ограничен при feedback 0.95");
    }

    free(bl); free(br); free(outR);
    if (g_fail == 0) printf("OK: delay — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
