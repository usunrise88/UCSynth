// Host-тест ZDF SVF-фильтра: АЧХ (LP/HP/BP), сквозной OFF, устойчивость при резонансе=1.
#include "filter.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static const float PI = 3.14159265358979f;
static const float SR = 48000.0f;

// Пиковая амплитуда выхода на синусе freq (после сходимости) — прокси АЧХ на этой частоте.
static float gain(uint8_t mode, float cutoff, float res, float freq)
{
    Filter f; filter_reset(&f);
    const FiltCoef c = filter_coef(cutoff, res, SR, mode);
    float ph = 0.0f, peak = 0.0f;
    const float inc = freq / SR;
    const int   N   = (int)(SR / freq * 20.0f);            // 20 периодов
    for (int i = 0; i < N; ++i) {
        const float in = sinf(2.0f * PI * ph);
        ph += inc; if (ph >= 1.0f) ph -= 1.0f;
        const float o = filter_process(&f, in, &c);
        if (i > N / 2) { const float a = std::fabs(o); if (a > peak) peak = a; }   // после переходного
    }
    return peak;
}

int main()
{
    // LP: низ проходит, верх режется
    check(gain(FILT_LP, 5000.0f, 0.0f, 100.0f)   > 0.8f, "LP пропускает низ");
    check(gain(FILT_LP, 500.0f,  0.0f, 10000.0f) < 0.2f, "LP режет верх");
    // HP: наоборот
    check(gain(FILT_HP, 5000.0f, 0.0f, 15000.0f) > 0.7f, "HP пропускает верх");
    check(gain(FILT_HP, 2000.0f, 0.0f, 100.0f)   < 0.2f, "HP режет низ");
    // BP: у cutoff громче, чем существенно ниже
    check(gain(FILT_BP, 1000.0f, 0.5f, 1000.0f) > gain(FILT_BP, 1000.0f, 0.5f, 100.0f),
          "BP: пик у cutoff");
    // OFF: сквозной
    {
        Filter f; filter_reset(&f);
        const FiltCoef c = filter_coef(1000.0f, 0.5f, SR, FILT_OFF);
        check(filter_process(&f, 0.37f, &c) == 0.37f, "OFF — сквозной");
    }
    // Устойчивость при resonance=1 (k≈0.02, Q≈50): белый шум → выход конечен и ограничен
    {
        Filter f; filter_reset(&f);
        const FiltCoef c = filter_coef(2000.0f, 1.0f, SR, FILT_LP);
        uint32_t s = 12345; float mx = 0.0f; bool ok = true;
        for (int i = 0; i < 200000; ++i) {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            const float in = (float)s * 4.6566129e-10f - 1.0f;
            const float o = filter_process(&f, in, &c);
            if (!std::isfinite(o)) { ok = false; break; }
            if (std::fabs(o) > mx) mx = std::fabs(o);
        }
        check(ok, "resonance=1 → без NaN/inf");
        check(mx < 100.0f, "resonance=1 → выход ограничен (не разнос)");
    }

    if (g_fail == 0) printf("OK: filter — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
