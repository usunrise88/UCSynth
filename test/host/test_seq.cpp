// Host-тест темп-клока секвенсора (этап 7.1): bpm→длина шага, swing, транспорт, врап. Чистый движок,
// без ESP-IDF. Запуск: tools/run-host-tests.sh
#include "seq_engine.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

int main()
{
    const float SR = 48000.0f;
    const int   N  = 64;   // аудио-блок

    // стоп → шаг -1, тик ничего не двигает
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{120, 0.0f, false, true};
        check(!seq_tick(&e, &cfg, SR, N), "стоп: нет перехода");
        check(e.step == -1, "стоп: step=-1");
    }

    // play edge → шаг 0 немедленно
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{120, 0.0f, true, true};
        check(seq_tick(&e, &cfg, SR, N), "play edge → переход");
        check(e.step == 0, "play edge → шаг 0");
    }

    // тайминг без свинга: bpm=120 → 6000 семплов/шаг (16-я @120 = 125 мс)
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{120, 0.0f, true, false};
        seq_tick(&e, &cfg, SR, N);            // старт, шаг 0
        long samples = 0; int guard = 0;
        while (e.step == 0 && guard++ < 2000) { seq_tick(&e, &cfg, SR, N); samples += N; }
        check(e.step == 1, "перешёл на шаг 1");
        check(std::labs(samples - 6000) <= N, "шаг ≈ 6000 семплов @120bpm");
    }

    // счёт переходов за 2 c @120bpm ≈ 16 (8 шагов/с)
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{120, 0.0f, true, false};
        seq_tick(&e, &cfg, SR, N);            // play edge (шаг 0), не считаем
        int steps = 0;
        for (long s = 0; s < 96000; s += N) if (seq_tick(&e, &cfg, SR, N)) steps++;
        check(steps >= 15 && steps <= 16, "≈16 переходов за 2 c @120bpm");
    }

    // свинг: bpm=120, swing=1 → чётный шаг 9000, нечётный 3000 семплов (пара = 12000 = 2·base)
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{120, 1.0f, true, false};
        seq_tick(&e, &cfg, SR, N);            // шаг 0
        long s0 = 0; int g = 0;
        while (e.step == 0 && g++ < 4000) { seq_tick(&e, &cfg, SR, N); s0 += N; }
        check(std::labs(s0 - 9000) <= N, "swing: чётный шаг ≈ 9000");
        long s1 = 0; g = 0;
        while (e.step == 1 && g++ < 4000) { seq_tick(&e, &cfg, SR, N); s1 += N; }
        check(std::labs(s1 - 3000) <= N, "swing: нечётный шаг ≈ 3000");
    }

    // врап 15→0
    {
        SeqEngine e; seq_init(&e);
        SeqConfig cfg{240, 0.0f, true, false};
        seq_tick(&e, &cfg, SR, N);
        bool wrapped = false; int prev = e.step; int g = 0;
        while (g++ < 200000 / N) {
            if (seq_tick(&e, &cfg, SR, N)) { if (prev == SEQ_STEPS - 1 && e.step == 0) wrapped = true; prev = e.step; }
        }
        check(wrapped, "шаг заворачивается 15→0");
    }

    // стоп посреди воспроизведения → step=-1
    {
        SeqEngine e; seq_init(&e);
        SeqConfig run{120, 0.0f, true, false};
        seq_tick(&e, &run, SR, N); seq_tick(&e, &run, SR, N);
        SeqConfig stop{120, 0.0f, false, false};
        seq_tick(&e, &stop, SR, N);
        check(e.step == -1, "стоп посреди → step=-1");
    }

    // 7.1: p-lock-оверрайдов ещё нет → seq_plock всегда false
    {
        SeqEngine e; seq_init(&e);
        float v = 42.0f;
        check(!seq_plock(&e, 0, &v), "7.1: seq_plock ещё пуст");
    }

    if (g_fail == 0) { printf("OK: seq — все проверки пройдены\n"); return 0; }
    printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
