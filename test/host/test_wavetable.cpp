// Host-тест wavetable: генерация форм, линейная интерполяция, диапазон. Без ESP-IDF.
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;

static void check(bool ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}

static bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

int main()
{
    wavetable_init();

    // Синус: узловые точки.
    check(approx(wavetable_sample(WAVE_SINE, 0.00f),  0.0f, 0.01f), "sine(0)=0");
    check(approx(wavetable_sample(WAVE_SINE, 0.25f),  1.0f, 0.01f), "sine(1/4)=+1");
    check(approx(wavetable_sample(WAVE_SINE, 0.50f),  0.0f, 0.01f), "sine(1/2)=0");
    check(approx(wavetable_sample(WAVE_SINE, 0.75f), -1.0f, 0.01f), "sine(3/4)=-1");

    // Пила: -1 в начале, ~0 в центре.
    check(approx(wavetable_sample(WAVE_SAW, 0.0f), -1.0f, 0.01f), "saw(0)=-1");
    check(approx(wavetable_sample(WAVE_SAW, 0.5f),  0.0f, 0.01f), "saw(1/2)=0");

    // Меандр: +1 в первой половине, -1 во второй.
    check(wavetable_sample(WAVE_SQUARE, 0.25f) > 0.5f,  "square(1/4)=+1");
    check(wavetable_sample(WAVE_SQUARE, 0.75f) < -0.5f, "square(3/4)=-1");

    // Треугольник: -1 → +1 → -1.
    check(approx(wavetable_sample(WAVE_TRI, 0.00f), -1.0f, 0.02f), "tri(0)=-1");
    check(approx(wavetable_sample(WAVE_TRI, 0.25f),  0.0f, 0.02f), "tri(1/4)=0");
    check(approx(wavetable_sample(WAVE_TRI, 0.50f),  1.0f, 0.02f), "tri(1/2)=+1");

    // Диапазон: все формы, вся фаза — в [-1,1].
    for (int w = 0; w < WAVE_COUNT; ++w) {
        for (int k = 0; k < 4096; ++k) {
            const float v = wavetable_sample((uint8_t)w, (float)k / 4096.0f);
            if (v < -1.001f || v > 1.001f) { check(false, "range [-1,1]"); break; }
        }
    }

    // Неизвестная форма → синус (не падаем, в диапазоне).
    check(approx(wavetable_sample(99, 0.25f), 1.0f, 0.01f), "bad waveform -> sine");

    if (g_fail == 0) printf("OK: wavetable — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
