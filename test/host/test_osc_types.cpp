// Host-тест типов осц-слота Classic-движка (этап 12.1/12.2): VirtualAnalog (PolyBLEP) и
// Phase Distortion. Проверяем: спектр VA-пилы/меандра (гармоники ~идеальны в полосе), снижение
// алиасинга PolyBLEP против наивного осц на высокой ноте, границы выхода, яркость PD от pd_amount.
// Без ESP-IDF (osc_types.cpp + wavetable.cpp для sine PD).
#include "osc_types.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static constexpr double PI = 3.14159265358979323846;
static constexpr float SR = 48000.0f;

static void check(bool ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}

// Амплитуда синусоиды частоты cyc (циклов/семпл) в буфере x[N] через DFT-проекцию (одностороннюю).
static double mag_at(const float *x, int N, double cyc)
{
    double re = 0, im = 0;
    for (int n = 0; n < N; ++n) {
        const double a = 2.0 * PI * cyc * (double)n;
        re += x[n] * std::cos(a);
        im -= x[n] * std::sin(a);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)N;
}

// Заполнить буфер VA-осц (или наивным при naive=true) на dt=freq/sr.
static void fill_va(float *x, int N, uint8_t wave, float dt, bool naive)
{
    float ph = 0.0f;
    for (int n = 0; n < N; ++n) {
        if (naive) {                             // наивная пила без коррекции (эталон алиасинга)
            x[n] = 2.0f * ph - 1.0f;
        } else {
            x[n] = va_sample(wave, ph, dt);
        }
        ph += dt;
        ph -= std::floor(ph);
    }
}

// Мощность в идеальных гармониках (k·dt < 0.5) и полная — для метрики алиасинга.
static void power_split(const float *x, int N, float dt, double &p_harm, double &p_tot)
{
    p_tot = 0.0;
    for (int n = 0; n < N; ++n) p_tot += (double)x[n] * x[n];
    p_tot /= (double)N;
    p_harm = 0.0;
    for (int k = 1; (double)k * dt < 0.5; ++k) {
        const double a = mag_at(x, N, (double)k * dt);
        p_harm += 0.5 * a * a;                   // мощность синусоиды амплитуды a
    }
}

int main()
{
    wavetable_init(SR);
    const int N = 8192;
    static float buf[8192], buf2[8192];

    // --- VA: границы выхода на прогоне по формам/частотам ---
    bool bounded = true;
    for (int w = 0; w <= 3; ++w) {
        for (float f = 55.0f; f < 8000.0f; f *= 1.5f) {
            const float dt = f / SR;
            float ph = 0.0f;
            for (int n = 0; n < 2000; ++n) {
                const float s = va_sample((uint8_t)w, ph, dt);
                if (s < -1.15f || s > 1.15f) bounded = false;   // PolyBLEP слегка перелетает ±1 у разрыва
                ph += dt; ph -= std::floor(ph);
            }
        }
    }
    check(bounded, "VA: выход в разумных границах (~[-1,1])");

    // --- VA-пила: спектр ~идеальной пилы (гармоники 1/k) на низкой ноте (алиасинг мал) ---
    {
        const float f = 110.0f, dt = f / SR;
        fill_va(buf, N, /*WAVE_SAW*/ 1, dt, false);
        const double m1 = mag_at(buf, N, dt);
        const double m2 = mag_at(buf, N, 2 * dt);
        const double m3 = mag_at(buf, N, 3 * dt);
        check(m1 > 0.3, "VA saw: есть основной тон");
        check(std::fabs(m2 / m1 - 0.5) < 0.1, "VA saw: 2-я гармоника ~1/2");
        check(std::fabs(m3 / m1 - 1.0 / 3.0) < 0.1, "VA saw: 3-я гармоника ~1/3");
    }

    // --- VA-меандр: только нечётные гармоники (2-я ≈ 0, 3-я ≈ 1/3) ---
    {
        const float f = 110.0f, dt = f / SR;
        fill_va(buf, N, /*WAVE_SQUARE*/ 2, dt, false);
        const double m1 = mag_at(buf, N, dt);
        const double m2 = mag_at(buf, N, 2 * dt);
        const double m3 = mag_at(buf, N, 3 * dt);
        check(m1 > 0.5, "VA square: есть основной тон");
        check(m2 / m1 < 0.05, "VA square: 2-я гармоника ~0 (нечётный спектр)");
        check(std::fabs(m3 / m1 - 1.0 / 3.0) < 0.1, "VA square: 3-я гармоника ~1/3");
    }

    // --- PolyBLEP давит алиасинг: на высокой ноте энергия вне идеальных гармоник << наивной ---
    {
        const float f = 3000.0f, dt = f / SR;    // 8 гармоник до Найквиста
        fill_va(buf, N, 1, dt, false);           // PolyBLEP
        fill_va(buf2, N, 1, dt, true);           // наивная
        double ph_blep, tot_blep, ph_naive, tot_naive;
        power_split(buf, N, dt, ph_blep, tot_blep);
        power_split(buf2, N, dt, ph_naive, tot_naive);
        const double alias_blep = tot_blep - ph_blep;
        const double alias_naive = tot_naive - ph_naive;
        check(alias_naive > 1e-4, "наивная пила действительно алиасит (эталон)");
        check(alias_blep < 0.5 * alias_naive, "VA saw: PolyBLEP снижает алиасинг вдвое+ против наивной");
    }

    // --- PD: amount=0 → чистый sine; amount>0 → растут верхние гармоники (яркость) ---
    {
        const float f = 220.0f, dt = f / SR;
        auto fill_pd = [&](float *x, float amount) {
            float ph = 0.0f;
            for (int n = 0; n < N; ++n) {
                x[n] = wavetable_sample(/*WAVE_SINE*/ 0, pd_warp(ph, amount), wavetable_mip(f));
                ph += dt; ph -= std::floor(ph);
            }
        };
        fill_pd(buf, 0.0f);
        const double h3_off = mag_at(buf, N, 3 * dt);
        const double h1_off = mag_at(buf, N, dt);
        check(h3_off / h1_off < 0.02, "PD amount=0: почти чистый sine (нет верхних гармоник)");
        fill_pd(buf2, 0.7f);
        const double h3_on = mag_at(buf2, N, 3 * dt);
        check(h3_on > 10.0 * h3_off + 0.01, "PD amount>0: 3-я гармоника заметно ярче (яркость)");
    }

    // pd_warp: тождество при 0, монотонность/границы
    check(std::fabs(pd_warp(0.25f, 0.0f) - 0.25f) < 1e-6f, "pd_warp(amount=0) — тождество");
    check(pd_warp(0.0f, 0.9f) >= 0.0f && pd_warp(0.999f, 0.9f) < 1.0f, "pd_warp в [0,1)");

    printf(g_fail ? "FAILED (%d)\n" : "OK: osc_types — все проверки пройдены\n", g_fail);
    return g_fail ? 1 : 0;
}
