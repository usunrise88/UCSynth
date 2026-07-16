// Host-тест wavetable-морфа: целая позиция == прямой lookup, pos=0.5 == среднее соседей (фаз-когерентно),
// клампы границ, диапазон [-1,1], изменение формы по позиции.
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b, float e) { return std::fabs(a - b) <= e; }
static const float SR = 48000.0f;

int main()
{
    wavetable_init(SR);
    const int mip = 0;

    // Целая позиция → прямой lookup соответствующей формы.
    for (float ph = 0.0f; ph < 1.0f; ph += 0.05f) {
        check(approx(wavetable_sample_morph(0.0f, ph, mip), wavetable_sample(WAVE_SINE, ph, mip), 1e-6f),
              "pos=0 == sample(sine)");
        check(approx(wavetable_sample_morph(1.0f, ph, mip), wavetable_sample(WAVE_SAW, ph, mip), 1e-6f),
              "pos=1 == sample(saw)");
        check(approx(wavetable_sample_morph(3.0f, ph, mip), wavetable_sample(WAVE_TRI, ph, mip), 1e-6f),
              "pos=3 == sample(tri)");
    }

    // pos=0.5 → среднее соседей на той же фазе (фаз-когерентность: таблицы одной длины на mip).
    for (float ph = 0.0f; ph < 1.0f; ph += 0.05f) {
        const float a = wavetable_sample(WAVE_SINE, ph, mip);
        const float b = wavetable_sample(WAVE_SAW, ph, mip);
        check(approx(wavetable_sample_morph(0.5f, ph, mip), 0.5f * (a + b), 1e-6f),
              "pos=0.5 == среднее sine/saw");
    }

    // Клампы: вне диапазона → крайние формы.
    for (float ph = 0.0f; ph < 1.0f; ph += 0.1f) {
        check(approx(wavetable_sample_morph(-1.0f, ph, mip), wavetable_sample(WAVE_SINE, ph, mip), 1e-6f),
              "pos<0 → sine");
        check(approx(wavetable_sample_morph(9.0f, ph, mip), wavetable_sample(WAVE_TRI, ph, mip), 1e-6f),
              "pos>max → tri");
    }

    // Диапазон [-1,1] и конечность на свипе pos/phase/mip.
    bool fin = true; float mn = 2.0f, mx = -2.0f;
    for (int m = 0; m < 3; ++m)
        for (float pos = 0.0f; pos <= 3.0f; pos += 0.13f)
            for (float ph = 0.0f; ph < 1.0f; ph += 0.07f) {
                const float v = wavetable_sample_morph(pos, ph, m);
                if (!std::isfinite(v)) fin = false;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
    check(fin, "морф конечен");
    check(mn >= -1.001f && mx <= 1.001f, "морф в [-1,1]");

    // Морф реально меняет значение по позиции (не константа) на фиксированной фазе.
    {
        float mn2 = 2.0f, mx2 = -2.0f;
        const float ph = 0.25f;
        for (float pos = 0.0f; pos <= 3.0f; pos += 0.1f) {
            const float v = wavetable_sample_morph(pos, ph, 0);
            if (v < mn2) mn2 = v;
            if (v > mx2) mx2 = v;
        }
        check(mx2 - mn2 > 0.1f, "морф меняет значение по позиции");
    }

    if (g_fail == 0) printf("OK: morph — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
