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

    // --- master drive (эффект): байпас, независимость от числа голосов, диапазон ---
    FxParams dOff{}; dOff.drive_on = false; dOff.drive = 0.8f; dOff.drive_mix = 1.0f;
    check(fx_drive(0.9f, &dOff, 4.0f) == 0.9f, "drive off → байпас");
    FxParams dM0{}; dM0.drive_on = true; dM0.drive = 0.8f; dM0.drive_mix = 0.0f;
    check(fx_drive(0.9f, &dM0, 4.0f) == 0.9f, "drive mix=0 → байпас");

    FxParams d{}; d.drive_on = true; d.drive = 0.6f; d.drive_mix = 1.0f;
    // Независимость от полифонии: один голос уровня v при norm=1 и сумма 4·v при norm=4 → одинаковый wet
    // (mix=1 → выход = wet). Характер драйва не зависит от числа голосов.
    for (float v = -0.6f; v <= 0.6f; v += 0.2f) {
        const float w1 = fx_drive(v, &d, 1.0f);
        const float w4 = fx_drive(4.0f * v, &d, 4.0f);
        check(approx(w1, w4, 1e-5f), "drive: характер не зависит от числа голосов");
    }
    // wet ограничен [-1,1] при mix=1 (жёсткий клип), даже на большой сумме
    for (float x = -8.0f; x <= 8.0f; x += 0.5f)
        check(std::fabs(fx_drive(x, &d, 4.0f)) <= 1.001f, "drive: |out| ≤ 1 при mix=1");
    // больше drive → раньше в клип (на одном уровне входа сильнее насыщение)
    FxParams dHi{}; dHi.drive_on = true; dHi.drive = 1.0f; dHi.drive_mix = 1.0f;
    FxParams dLo{}; dLo.drive_on = true; dLo.drive = 0.1f; dLo.drive_mix = 1.0f;
    check(fx_drive(0.15f, &dHi, 1.0f) > fx_drive(0.15f, &dLo, 1.0f), "больше drive → сильнее насыщение");

    // --- master limit: прозрачно ниже порога, ограничено ±1, монотонно ---
    check(fx_master_limit(0.5f) == 0.5f, "limit: прозрачно ниже порога");
    check(fx_master_limit(-0.5f) == -0.5f, "limit: прозрачно ниже порога (−)");
    float lprev = -2.0f; bool lmono = true, lrng = true;
    for (float x = -6.0f; x <= 6.0f; x += 0.05f) {
        const float y = fx_master_limit(x);
        if (y < -1.0f || y > 1.0f) lrng = false;      // строго внутри ±1 (асимптота)
        if (y < lprev - 1e-4f) lmono = false;
        lprev = y;
    }
    check(lrng, "limit: |out| < 1 всегда (без жёсткого клипа)");
    check(lmono, "limit: монотонно");
    check(fx_master_limit(100.0f) > 0.95f && fx_master_limit(100.0f) < 1.0f, "limit: большой вход → ~1");

    if (g_fail == 0) printf("OK: fx — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
