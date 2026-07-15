// Host-тест band-limited wavetable (этап 3.0, D-008): формы, диапазон, выбор mip и — главное —
// отсутствие энергии выше расчётного лимита гармоник (band-limit). Без ESP-IDF.
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static constexpr double PI = 3.14159265358979323846;

static void check(bool ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}
static bool approx(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// Величина k-й гармоники таблицы: проецируем один период (снятый в узлах mip 0, L=2048 → без
// ошибки интерполяции) на sin/cos. mip 0 несёт до 600 гармоник — на нём и проверяем спектр.
static double harmonic_mag(uint8_t w, int k)
{
    const int N = 2048;                      // = длине таблицы mip 0: phase=n/N попадает в узлы
    double re = 0.0, im = 0.0;
    for (int n = 0; n < N; ++n) {
        const double x = (double)n / (double)N;
        const double v = wavetable_sample(w, (float)x, /*mip=*/0);
        re += v * std::cos(2.0 * PI * k * x);
        im -= v * std::sin(2.0 * PI * k * x);
    }
    return std::sqrt(re * re + im * im) / (double)N;
}

int main()
{
    wavetable_init(48000.0f);

    // --- Синус: band-limit не трогает 1-ю гармонику, форма точна на любом mip ---
    for (int mip = 0; mip <= 6; ++mip) {
        check(approx(wavetable_sample(WAVE_SINE, 0.00f, mip),  0.0f, 0.02f), "sine(0)=0");
        check(approx(wavetable_sample(WAVE_SINE, 0.25f, mip),  1.0f, 0.02f), "sine(1/4)=+1");
        check(approx(wavetable_sample(WAVE_SINE, 0.50f, mip),  0.0f, 0.02f), "sine(1/2)=0");
        check(approx(wavetable_sample(WAVE_SINE, 0.75f, mip), -1.0f, 0.02f), "sine(3/4)=-1");
    }

    // --- Пила: нечётная относительно 1/2 (saw(1-x) = -saw(x)); saw(1/4) > 0 ---
    check(wavetable_sample(WAVE_SAW, 0.25f, 0) > 0.3f, "saw(1/4) > 0");
    check(approx(wavetable_sample(WAVE_SAW, 0.25f, 0),
                 -wavetable_sample(WAVE_SAW, 0.75f, 0), 0.02f), "saw антисимметрична");

    // --- Меандр: плоские полки ±, полуволновая антисимметрия square(x+1/2) = -square(x) ---
    check(wavetable_sample(WAVE_SQUARE, 0.25f, 0) >  0.5f, "square(1/4) верх");
    check(wavetable_sample(WAVE_SQUARE, 0.75f, 0) < -0.5f, "square(3/4) низ");

    // --- Треугольник: пик на 1/4, ноль на 0 и 1/2 (фаза аддитивного ряда) ---
    check(approx(wavetable_sample(WAVE_TRI, 0.00f, 0),  0.0f, 0.03f), "tri(0)=0");
    check(wavetable_sample(WAVE_TRI, 0.25f, 0) >  0.8f,               "tri(1/4) пик");
    check(approx(wavetable_sample(WAVE_TRI, 0.50f, 0),  0.0f, 0.03f), "tri(1/2)=0");
    check(wavetable_sample(WAVE_TRI, 0.75f, 0) < -0.8f,              "tri(3/4) впадина");

    // --- Диапазон [-1,1]: все формы, вся фаза, все mip (нормировка на пик = 1) ---
    for (int w = 0; w < WAVE_COUNT; ++w) {
        for (int mip = 0; mip < 11; ++mip) {
            for (int k = 0; k < 4096; ++k) {
                const float v = wavetable_sample((uint8_t)w, (float)k / 4096.0f, mip);
                if (v < -1.001f || v > 1.001f) { check(false, "range [-1,1]"); w = mip = k = 99999; }
            }
        }
    }

    // --- Band-limit: на mip 0 гармоники есть в полосе (1..600) и отсутствуют выше (700) ---
    // Пила: 1-я и 2-я гармоники присутствуют (амплитуды ~1/k), выше лимита — тишина.
    const double saw1 = harmonic_mag(WAVE_SAW, 1);
    check(saw1 > 0.1,                                  "saw: 1-я гармоника есть");
    check(harmonic_mag(WAVE_SAW, 2) > 0.3 * saw1,      "saw: 2-я гармоника есть (~1/2)");
    check(harmonic_mag(WAVE_SAW, 700) < 0.01 * saw1,   "saw: выше лимита (700) — тишина");
    // Меандр: только нечётные — 2-я гармоника пуста, 3-я есть.
    const double sq1 = harmonic_mag(WAVE_SQUARE, 1);
    check(sq1 > 0.1,                                   "square: 1-я гармоника есть");
    check(harmonic_mag(WAVE_SQUARE, 2) < 0.01 * sq1,   "square: чётных гармоник нет");
    check(harmonic_mag(WAVE_SQUARE, 3) > 0.2 * sq1,    "square: 3-я гармоника есть");
    check(harmonic_mag(WAVE_SQUARE, 700) < 0.01 * sq1, "square: выше лимита — тишина");

    // --- Выбор mip по частоте: F0 → 0, монотонность, клампы на краях ---
    check(wavetable_mip(20.0f)    == 0,  "mip(20 Гц)=0");
    check(wavetable_mip(440.0f)   >= 3 && wavetable_mip(440.0f) <= 5, "mip(440 Гц) в 3..5");
    check(wavetable_mip(40.0f)    == 1,  "mip(40 Гц)=1 (октава над F0)");
    check(wavetable_mip(19000.0f) >= 9,  "mip(19 кГц) — верхний диапазон");
    check(wavetable_mip(1.0f)     == 0,  "mip(<F0) клампится в 0");
    check(wavetable_mip(1e6f)     == 10, "mip(очень высоко) клампится в верх");

    // --- Неизвестная форма → синус (не падаем, в диапазоне) ---
    check(approx(wavetable_sample(99, 0.25f, 0), 1.0f, 0.02f), "bad waveform -> sine");

    if (g_fail == 0) printf("OK: wavetable (band-limited) — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
