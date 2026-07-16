// Host-тест ADSR-огибающей (env): сегменты, фронты gate, ретригер, loop, кламп мин-времени.
#include "env.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b, float e) { return std::fabs(a - b) <= e; }

int main()
{
    const float dt = 0.001f;                       // 1 мс на тик (как control-блок)
    EnvParams p{ 0.01f, 0.01f, 0.5f, 0.01f, false };

    // reset → idle/0
    Env e; env_reset(&e);
    check(e.level == 0.0f && e.stage == ENV_IDLE, "reset → idle/0");

    // attack растёт от 0
    const float l1 = env_tick(&e, &p, true, dt);
    const float l2 = env_tick(&e, &p, true, dt);
    check(l1 > 0.0f && l2 > l1, "attack растёт");

    // держим gate → сустейн 0.5
    for (int i = 0; i < 200; ++i) env_tick(&e, &p, true, dt);
    const float ls = env_tick(&e, &p, true, dt);
    check(approx(ls, 0.5f, 0.02f) && e.stage == ENV_SUSTAIN, "держим gate → sustain 0.5");

    // отпускаем → release к 0, idle
    for (int i = 0; i < 200; ++i) env_tick(&e, &p, false, dt);
    const float lr = env_tick(&e, &p, false, dt);
    check(approx(lr, 0.0f, 0.01f) && e.stage == ENV_IDLE, "release → idle/0");

    // ретригер из release: attack ОТ текущего уровня (не сброс в 0)
    env_reset(&e);
    for (int i = 0; i < 300; ++i) env_tick(&e, &p, true, dt);   // в сустейн
    env_tick(&e, &p, false, dt);                                // один тик release
    const float during_rel = e.level;
    const float retrig = env_tick(&e, &p, true, dt);            // снова gate
    check(retrig > during_rel && retrig > 0.2f && e.stage == ENV_ATTACK,
          "ретригер из release — атака от текущего уровня, без сброса в 0");

    // кламп мин-времени: attack=0 → без NaN, быстро к 1
    EnvParams pf{ 0.0f, 0.01f, 0.5f, 0.01f, false };
    Env e2; env_reset(&e2);
    const float lf = env_tick(&e2, &pf, true, dt);
    check(std::isfinite(lf) && lf > 0.98f, "attack=0 → без NaN, быстро к 1");

    // loop: при удержании огибающая гуляет (цикл A→D→A), не константа
    EnvParams pl{ 0.005f, 0.005f, 0.5f, 0.01f, true };
    Env e3; env_reset(&e3);
    float mn = 2.0f, mx = -2.0f;
    for (int i = 0; i < 400; ++i) {
        const float v = env_tick(&e3, &pl, true, dt);
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    check(mx - mn > 0.1f, "loop → огибающая гуляет (не застревает на sustain)");

    if (g_fail == 0) printf("OK: env — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
