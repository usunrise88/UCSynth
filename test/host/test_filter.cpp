// Host-тест ZDF SVF-фильтра: АЧХ (LP/HP/BP), сквозной OFF, устойчивость при резонансе=1.
#include "filter.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <initializer_list>

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

    // Устойчивость ПРИ МОДУЛЯЦИИ cutoff — главный риск ZDF: коэффициенты пересчитываются каждый
    // блок, и состояние интеграторов переносится между наборами. Прежняя проверка гоняла шум с
    // фиксированными коэффициентами, то есть этот путь не трогала вовсе. Порог тоже был слишком
    // мягким (mx < 100 при входе ±1 пропустил бы звон +40 дБ).
    // Замеры (вход = белый шум ±1, res=1, Q≈50): фиксированный cutoff даёт пик 7.3×, модулируемый
    // 12.6×. Это законное резонансное усиление, а не разнос — поэтому порог ловит РАСХОЖДЕНИЕ
    // (25×) и отдельно проверяет, что модуляция не ухудшает картину в разы против фиксированного
    // случая. Абсолютный порог наугад тут бесполезен: слишком мягкий пропустит звон, слишком
    // жёсткий будет падать на нормальном резонансе.
    {
        auto sweep = [](bool modulate) {
            Filter f; filter_reset(&f);
            uint32_t s = 777; float mx = 0.0f;
            for (int blk = 0; blk < 4000; ++blk) {
                const float t  = (float)blk / 4000.0f;
                const float fc = modulate
                    ? 20.0f + (20000.0f - 20.0f) * (0.5f + 0.5f * std::sin(t * 200.0f))
                    : 2000.0f;
                const FiltCoef c = filter_coef(fc, 1.0f, SR, FILT_LP);
                for (int i = 0; i < 64; ++i) {
                    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                    const float o = filter_process(&f, (float)s * 4.6566129e-10f - 1.0f, &c);
                    if (!std::isfinite(o)) return -1.0f;
                    if (std::fabs(o) > mx) mx = std::fabs(o);
                }
            }
            return mx;
        };
        const float fixed = sweep(false);
        const float moved = sweep(true);
        check(fixed > 0.0f && moved > 0.0f, "модуляция cutoff при res=1 → без NaN/inf");
        check(moved < 25.0f, "модуляция cutoff при res=1 → выход не расходится");
        check(moved < fixed * 4.0f, "модуляция cutoff не ухудшает пик в разы против фикс. cutoff");
    }

    // cutoff выше Найквиста и ниже нуля должен клампиться внутри filter_coef, а не давать мусорный g.
    {
        for (float fc : {-100.0f, 0.0f, SR, SR * 4.0f, 1e9f}) {
            const FiltCoef c = filter_coef(fc, 0.5f, SR, FILT_LP);
            check(std::isfinite(c.g) && c.g > 0.0f, "cutoff вне диапазона → g конечен и > 0");
            Filter f; filter_reset(&f);
            bool fin = true;
            for (int i = 0; i < 2000; ++i) {
                const float o = filter_process(&f, (i % 2) ? 1.0f : -1.0f, &c);
                if (!std::isfinite(o)) { fin = false; break; }
            }
            check(fin, "cutoff вне диапазона → фильтр не разносит");
        }
    }

    // resonance вне [0,1] и NaN клампится ВНУТРИ filter_coef (контракт filter.h). При res > 1.0101
    // множитель k уходит в минус = отрицательное демпфирование = разнос.
    {
        for (float res : {-1.0f, 1.5f, 100.0f, std::nanf("")}) {
            const FiltCoef c = filter_coef(2000.0f, res, SR, FILT_LP);
            check(std::isfinite(c.k) && c.k >= 0.0f, "resonance вне диапазона → k конечен и ≥ 0");
            Filter f; filter_reset(&f);
            float mx = 0.0f; bool fin = true;
            uint32_t s2 = 4242;
            for (int i = 0; i < 100000; ++i) {
                s2 ^= s2 << 13; s2 ^= s2 >> 17; s2 ^= s2 << 5;
                const float o = filter_process(&f, (float)s2 * 4.6566129e-10f - 1.0f, &c);
                if (!std::isfinite(o)) { fin = false; break; }
                if (std::fabs(o) > mx) mx = std::fabs(o);
            }
            check(fin && mx < 100.0f, "resonance вне диапазона → без разноса");
        }
    }

    // Переключение режима сохраняет состояние и не даёт щелчка в бесконечность.
    {
        Filter f; filter_reset(&f);
        const uint8_t modes[] = { FILT_LP, FILT_HP, FILT_BP, FILT_OFF, FILT_LP };
        bool fin = true;
        for (uint8_t m : modes) {
            const FiltCoef c = filter_coef(1200.0f, 0.9f, SR, m);
            for (int i = 0; i < 5000; ++i) {
                const float o = filter_process(&f, std::sin((float)i * 0.05f), &c);
                if (!std::isfinite(o)) { fin = false; break; }
            }
        }
        check(fin, "смена режима на ходу → без NaN");
    }

    if (g_fail == 0) printf("OK: filter — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
