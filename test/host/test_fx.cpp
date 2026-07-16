// Host-тест эффектов. 5.1 — overdrive: байпас (off/mix=0), монотонность, диапазон/насыщение,
// нечётная симметрия, зависимость от драйва.
#include "fx.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b, float e) { return std::fabs(a - b) <= e; }

int main()
{
    // off → байпас (даже с ненулевым drive/mix)
    FxParams off{}; off.od_on = false; off.od_drive = 0.5f; off.od_mix = 1.0f;
    for (float x = -1.0f; x <= 1.0f; x += 0.25f) check(fx_overdrive(x, &off) == x, "off → байпас");

    // mix=0 → байпас
    FxParams m0{}; m0.od_on = true; m0.od_drive = 0.8f; m0.od_mix = 0.0f;
    check(fx_overdrive(0.5f, &m0) == 0.5f, "mix=0 → байпас");

    // вкл, полный wet: монотонность, конечность, диапазон
    FxParams p{}; p.od_on = true; p.od_drive = 0.7f; p.od_mix = 1.0f;
    float prev = -2.0f; bool mono = true, fin = true, rng = true;
    for (float x = -1.5f; x <= 1.5f; x += 0.05f) {
        const float y = fx_overdrive(x, &p);
        if (!std::isfinite(y)) fin = false;
        if (y < prev - 1e-4f)  mono = false;
        if (y < -1.001f || y > 1.001f) rng = false;
        prev = y;
    }
    check(fin, "конечно");
    check(mono, "монотонно возрастает");
    check(rng, "|out| ≤ 1 при mix=1 (в т.ч. приручает вход >1)");

    // насыщение больших входов
    check(std::fabs(fx_overdrive(5.0f, &p)) <= 1.001f && std::fabs(fx_overdrive(-5.0f, &p)) <= 1.001f,
          "насыщение больших входов");

    // нечётная симметрия (tanh нечётна)
    check(approx(fx_overdrive(0.4f, &p), -fx_overdrive(-0.4f, &p), 1e-5f), "нечётная симметрия");

    // waveshaping реально меняет форму (не линия)
    check(!approx(fx_overdrive(0.3f, &p), 0.3f, 0.02f), "drive меняет форму");

    // больше драйва → сильнее подъём среднего уровня к насыщению
    FxParams lo{}; lo.od_on = true; lo.od_drive = 0.1f; lo.od_mix = 1.0f;
    FxParams hi{}; hi.od_on = true; hi.od_drive = 0.9f; hi.od_mix = 1.0f;
    check(fx_overdrive(0.3f, &hi) > fx_overdrive(0.3f, &lo), "больше драйва → сильнее насыщение");

    if (g_fail == 0) printf("OK: fx — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
