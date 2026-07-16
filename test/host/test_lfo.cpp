// Host-тест LFO: формы (sine/tri/saw/square/S&H), диапазон [-1,1], фаза, S&H держит и меняется.
#include "lfo.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b, float e) { return std::fabs(a - b) <= e; }

int main()
{
    // reset → фаза 0
    Lfo l; lfo_reset(&l, 12345u);
    check(l.phase == 0.0f, "reset → phase 0");

    // sine: rate=1, dt=0.25 → фаза шагает по 0.25; значения в характерных точках
    lfo_reset(&l, 1u);
    const float s1 = lfo_tick(&l, 1.0f, 0.25f, LFO_SINE);   // ph 0.25 → +1
    const float s2 = lfo_tick(&l, 1.0f, 0.25f, LFO_SINE);   // ph 0.50 → 0
    const float s3 = lfo_tick(&l, 1.0f, 0.25f, LFO_SINE);   // ph 0.75 → -1
    check(approx(s1, 1.0f, 0.001f), "sine ph0.25 → +1");
    check(approx(s2, 0.0f, 0.001f), "sine ph0.5 → 0");
    check(approx(s3, -1.0f, 0.001f), "sine ph0.75 → -1");

    // triangle: пик +1 в центре (ph 0.5)
    lfo_reset(&l, 1u);
    lfo_tick(&l, 1.0f, 0.25f, LFO_TRI);                     // ph 0.25 → 0
    const float t2 = lfo_tick(&l, 1.0f, 0.25f, LFO_TRI);   // ph 0.50 → +1
    check(approx(t2, 1.0f, 0.001f), "tri ph0.5 → +1");

    // saw: растёт по фазе (-1→+1)
    lfo_reset(&l, 1u);
    const float w1 = lfo_tick(&l, 1.0f, 0.25f, LFO_SAW);   // ph 0.25 → -0.5
    const float w2 = lfo_tick(&l, 1.0f, 0.25f, LFO_SAW);   // ph 0.50 →  0
    check(w2 > w1 && approx(w1, -0.5f, 0.001f) && approx(w2, 0.0f, 0.001f), "saw растёт");

    // square: +1 в первой половине, -1 во второй
    lfo_reset(&l, 1u);
    const float q1 = lfo_tick(&l, 1.0f, 0.25f, LFO_SQUARE); // ph 0.25 → +1
    const float q2 = lfo_tick(&l, 1.0f, 0.30f, LFO_SQUARE); // ph 0.55 → -1
    check(q1 == 1.0f && q2 == -1.0f, "square ±1 по половинам");

    // все формы: конечны, в диапазоне [-1,1], не константа (свип >6 циклов)
    for (uint8_t sh = 0; sh < LFO_SHAPE_COUNT; ++sh) {
        Lfo x; lfo_reset(&x, 777u + sh);
        float mn = 2.0f, mx = -2.0f; bool finite = true;
        for (int i = 0; i < 200; ++i) {
            const float v = lfo_tick(&x, 3.3f, 0.01f, sh);
            if (!std::isfinite(v)) finite = false;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        check(finite, "форма конечна");
        check(mn >= -1.001f && mx <= 1.001f, "форма в диапазоне [-1,1]");
        check(mx - mn > 0.1f, "форма не константа");
    }

    // S&H: держит значение между заворотами (маленький шаг фазы → то же значение)
    lfo_reset(&l, 42u);
    const float h1 = lfo_tick(&l, 0.1f, 0.1f, LFO_SH);   // ph 0.01, без wrap
    const float h2 = lfo_tick(&l, 0.1f, 0.1f, LFO_SH);   // ph 0.02, без wrap
    check(h1 == h2, "S&H держится между заворотами");

    // S&H: на заворотах цикла тянет новую случайную выборку (rate=1,dt=1 → wrap каждый тик)
    lfo_reset(&l, 42u);
    float prev = lfo_tick(&l, 1.0f, 1.0f, LFO_SH);
    int changes = 0;
    for (int i = 0; i < 30; ++i) {
        const float v = lfo_tick(&l, 1.0f, 1.0f, LFO_SH);
        if (v != prev) changes++;
        prev = v;
    }
    check(changes > 3, "S&H меняется на заворотах цикла");

    if (g_fail == 0) printf("OK: lfo — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
