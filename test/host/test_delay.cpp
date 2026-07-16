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

    // --- пинг-понг: моно-импульс → ЛЕВОЕ эхо на delay_time, ПРАВОЕ на 2·delay_time (скачок), затухание ---
    FxState fx;
    fx_delay_init(&fx, bl, br, len);
    FxParams p{};
    p.delay_on = true; p.delay_time = 10.0f; p.delay_feedback = 0.5f; p.delay_damp = 0.0f; p.delay_mix = 1.0f;
    const int d     = (int)(p.delay_time * 0.001f * sr);   // 480
    const int total = d * 4 + 300;
    float *outL = (float *)calloc(total, sizeof(float));
    float *outR = (float *)calloc(total, sizeof(float));
    for (int gi = 0; gi < total;) {
        for (int i = 0; i < N; ++i) { const float v = (gi + i == 0) ? 1.0f : 0.0f; l[i] = v; r[i] = v; }
        fx_delay(&fx, &p, l, r, N, sr);
        for (int i = 0; i < N && gi + i < total; ++i) { outL[gi + i] = l[i]; outR[gi + i] = r[i]; }
        gi += N;
    }
    // левое эхо на d (полная амплитуда при mix=1); в этот момент правого эха быть НЕ должно (скачок вправо позже)
    int pkL = -1; float pkLv = 0.0f;
    for (int i = 1; i < d + d / 2; ++i) { const float a = std::fabs(outL[i]); if (a > pkLv) { pkLv = a; pkL = i; } }
    check(pkL >= d - 2 && pkL <= d + 2, "пинг-понг: левое эхо на delay_time");
    check(pkLv > 0.9f, "левое эхо ~ полной амплитуды при mix=1");
    float rAtD = 0.0f;
    for (int i = d - 3; i <= d + 3; ++i) { const float a = std::fabs(outR[i]); if (a > rAtD) rAtD = a; }
    check(rAtD < 0.05f, "на delay_time правый канал молчит (эхо ушло в левый)");
    // правое эхо на 2d, амплитуда ~fb (0.5)
    int pkR = -1; float pkRv = 0.0f;
    for (int i = d + d / 2; i < 2 * d + d / 2; ++i) { const float a = std::fabs(outR[i]); if (a > pkRv) { pkRv = a; pkR = i; } }
    check(pkR >= 2 * d - 2 && pkR <= 2 * d + 2, "пинг-понг: правое эхо на 2·delay_time");
    check(pkRv > 0.4f && pkRv < 0.6f, "правое эхо ~ fb (0.5)");
    // левое эхо на 3d затухает относительно эха на d (обратная связь fb²)
    float e3 = 0.0f;
    for (int i = 3 * d - 3; i <= 3 * d + 3 && i < total; ++i) { const float a = std::fabs(outL[i]); if (a > e3) e3 = a; }
    check(e3 > 0.05f && e3 < pkLv, "левое эхо на 3d затухает (feedback)");

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

    free(bl); free(br); free(outL); free(outR);
    if (g_fail == 0) printf("OK: delay — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
