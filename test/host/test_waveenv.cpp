// Host-тест wave-огибающей: старт с первой точки, one-shot доходит и держит последнюю, монотонность
// на рампе, диапазон [0,1], loop гуляет, rate управляет скоростью.
#include "waveenv.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b, float e) { return std::fabs(a - b) <= e; }

// Рампа 0..1 по 8 точкам: 7-сегментная интерполяция должна восстановить прямую (output == phase).
static WaveEnvParams ramp(float rate, bool loop)
{
    WaveEnvParams p;
    for (int i = 0; i < WAVEENV_POINTS; ++i) p.pts[i] = (float)i / (float)(WAVEENV_POINTS - 1);
    p.rate = rate;
    p.loop = loop;
    return p;
}

int main()
{
    WaveEnvParams p = ramp(1.0f, false);

    // reset → фаза 0; тик с dt=0 = первая точка (0).
    WaveEnv w; waveenv_reset(&w);
    check(w.phase == 0.0f, "reset → phase 0");
    check(approx(waveenv_tick(&w, &p, 0.0f), 0.0f, 1e-6f), "старт = точка 1 (0)");

    // one-shot: за > rate сек доходит до конца и держит последнюю точку (1.0).
    waveenv_reset(&w);
    for (int i = 0; i < 20; ++i) waveenv_tick(&w, &p, 0.1f);   // 2.0s > rate=1s
    check(approx(waveenv_tick(&w, &p, 0.1f), 1.0f, 0.02f), "one-shot: hold на последней точке");

    // one-shot: на рампе движение монотонно растёт.
    waveenv_reset(&w);
    float prev = waveenv_tick(&w, &p, 0.0f);
    bool up = true;
    for (int i = 0; i < 10; ++i) {
        const float v = waveenv_tick(&w, &p, 0.1f);
        if (v < prev - 1e-4f) up = false;
        prev = v;
    }
    check(up, "one-shot: рампа растёт монотонно");

    // Диапазон [0,1] и конечность на свипе.
    {
        WaveEnv x; waveenv_reset(&x);
        bool fin = true; float mn = 2.0f, mx = -2.0f;
        for (int i = 0; i < 400; ++i) {
            const float v = waveenv_tick(&x, &p, 0.02f);
            if (!std::isfinite(v)) fin = false;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        check(fin && mn >= -0.001f && mx <= 1.001f, "one-shot: в [0,1], конечно");
    }

    // loop: огибающая гуляет по рампе (не застревает), остаётся в [0,1].
    {
        WaveEnvParams pl = ramp(1.0f, true);
        WaveEnv wl; waveenv_reset(&wl);
        float mn = 2.0f, mx = -2.0f;
        for (int i = 0; i < 400; ++i) {   // rate 1s, dt .02 → 8 циклов
            const float v = waveenv_tick(&wl, &pl, 0.02f);
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        check(mx - mn > 0.5f, "loop: огибающая гуляет по рампе");
        check(mn >= -0.001f && mx <= 1.001f, "loop: в [0,1]");
    }

    // rate: меньше rate → быстрее проходит (за те же 0.5 с ушёл дальше).
    {
        WaveEnvParams slow = ramp(2.0f, false);
        WaveEnv a; waveenv_reset(&a);
        for (int i = 0; i < 5; ++i) waveenv_tick(&a, &slow, 0.1f);   // 0.5s из 2s
        const float vslow = waveenv_tick(&a, &slow, 0.0f);

        WaveEnvParams fast = ramp(0.5f, false);
        WaveEnv b; waveenv_reset(&b);
        for (int i = 0; i < 5; ++i) waveenv_tick(&b, &fast, 0.1f);   // 0.5s из 0.5s → конец
        const float vfast = waveenv_tick(&b, &fast, 0.0f);

        check(vfast > vslow, "rate: меньше rate → быстрее проходит");
    }

    if (g_fail == 0) printf("OK: waveenv — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
