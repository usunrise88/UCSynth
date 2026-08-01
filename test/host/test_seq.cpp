// Host-тест секвенсора (этапы 7.1/7.2): темп-клок (bpm/swing/транспорт/врап) + воспроизведение паттерна
// (ноты шага, аккорд, release, p-lock оверрайды, trig-probability). Чистый движок. tools/run-host-tests.sh
#include "seq_engine.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

static SeqNoteEvent EV[16];
static int tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n) { return seq_tick(e, cfg, sr, n, EV, 16); }

// Активировать шаг с одной нотой.
static void set_note(SeqEngine *e, int step, uint8_t note, uint8_t vel, float prob)
{
    e->pattern[step].active    = true;
    e->pattern[step].n_notes   = 1;
    e->pattern[step].notes[0]  = note;
    e->pattern[step].velocity  = vel;
    e->pattern[step].trig_prob = prob;
}

int main()
{
    const float SR = 48000.0f;
    const int   N  = 64;   // аудио-блок

    // ---------- клок (7.1) ----------
    { SeqEngine e; seq_init(&e); SeqConfig c{120, 0.0f, false, true};
      tick(&e, &c, SR, N); check(e.step == -1, "стоп: step=-1"); }

    { SeqEngine e; seq_init(&e); SeqConfig c{120, 0.0f, true, true};
      tick(&e, &c, SR, N); check(e.step == 0, "play edge → шаг 0"); }

    // тайминг: bpm=120 → 6000 семплов/шаг
    { SeqEngine e; seq_init(&e); SeqConfig c{120, 0.0f, true, false};
      tick(&e, &c, SR, N);
      long s = 0; int g = 0; while (e.step == 0 && g++ < 2000) { tick(&e, &c, SR, N); s += N; }
      check(e.step == 1 && std::labs(s - 6000) <= N, "шаг ≈ 6000 семплов @120bpm"); }

    // счёт переходов за 2 c ≈ 16
    { SeqEngine e; seq_init(&e); SeqConfig c{120, 0.0f, true, false};
      tick(&e, &c, SR, N);
      int steps = 0, prev = e.step;
      for (long s = 0; s < 96000; s += N) { tick(&e, &c, SR, N); if (e.step != prev) { steps++; prev = e.step; } }
      check(steps >= 15 && steps <= 16, "≈16 переходов за 2 c @120bpm"); }

    // swing: чётный шаг ≈9000, нечётный ≈3000 (@120 swing=1)
    { SeqEngine e; seq_init(&e); SeqConfig c{120, 1.0f, true, false};
      tick(&e, &c, SR, N);
      long s0 = 0; int g = 0; while (e.step == 0 && g++ < 4000) { tick(&e, &c, SR, N); s0 += N; }
      check(std::labs(s0 - 9000) <= N, "swing: чётный ≈9000");
      long s1 = 0; g = 0; while (e.step == 1 && g++ < 4000) { tick(&e, &c, SR, N); s1 += N; }
      check(std::labs(s1 - 3000) <= N, "swing: нечётный ≈3000"); }

    // врап 15→0
    { SeqEngine e; seq_init(&e); SeqConfig c{240, 0.0f, true, false};
      tick(&e, &c, SR, N);
      bool wrapped = false; int prev = e.step;
      for (int g = 0; g < 200000 / N; ++g) { tick(&e, &c, SR, N); if (prev == SEQ_STEPS - 1 && e.step == 0) wrapped = true; prev = e.step; }
      check(wrapped, "шаг заворачивается 15→0"); }

    // ---------- паттерн (7.2) ----------
    // шаг 0 активен нотой 60 → play edge → note-on 60 vel 100
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f);
      SeqConfig c{120, 0.0f, true, true};
      int nev = tick(&e, &c, SR, N);
      check(nev == 1 && EV[0].on == 1 && EV[0].note == 60 && EV[0].vel == 100, "шаг 0 → note-on 60 vel 100"); }

    // аккорд: шаг 0 три ноты → три note-on
    { SeqEngine e; seq_init(&e);
      e.pattern[0].active = true; e.pattern[0].n_notes = 3;
      e.pattern[0].notes[0] = 60; e.pattern[0].notes[1] = 64; e.pattern[0].notes[2] = 67;
      e.pattern[0].velocity = 90; e.pattern[0].trig_prob = 1.0f;
      SeqConfig c{120, 0.0f, true, true};
      int nev = tick(&e, &c, SR, N);
      int ons = 0; for (int i = 0; i < nev; ++i) if (EV[i].on) ons++;
      check(ons == 3, "аккорд шага → 3 note-on"); }

    // переход 0→1: off прошлой ноты + on новой
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f); set_note(&e, 1, 64, 100, 1.0f);
      SeqConfig c{120, 0.0f, true, true};
      tick(&e, &c, SR, N);   // шаг 0 → on 60
      bool off60 = false, on64 = false; int g = 0;
      while (e.step == 0 && g++ < 2000) { int nev = tick(&e, &c, SR, N);
          for (int i = 0; i < nev; ++i) { if (!EV[i].on && EV[i].note == 60) off60 = true; if (EV[i].on && EV[i].note == 64) on64 = true; } }
      check(e.step == 1 && off60 && on64, "0→1: off 60 + on 64"); }

    // p-lock: шаг 0 переопределяет параметр id=20 → seq_plock даёт значение; на шаге 1 (без p-lock) снят
    { SeqEngine e; seq_init(&e);
      e.pattern[0].n_plocks = 1; e.pattern[0].plocks[0] = { 20, 1234.0f };
      SeqConfig c{120, 0.0f, true, true};
      tick(&e, &c, SR, N);   // шаг 0
      float v = 0.0f;
      check(seq_plock(&e, 20, &v) && v == 1234.0f, "p-lock шага 0 активен");
      check(!seq_plock(&e, 21, &v), "p-lock не задевает другой id");
      int g = 0; while (e.step == 0 && g++ < 2000) tick(&e, &c, SR, N);
      check(e.step == 1 && !seq_plock(&e, 20, &v), "p-lock снят на шаге без него"); }

    // trig-probability: 0 → активный шаг не играет; 1 → играет
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 72, 100, 0.0f);
      SeqConfig c{120, 0.0f, true, true};
      check(tick(&e, &c, SR, N) == 0, "trig_prob=0 → не играет"); }

    // стоп во время звучащей ноты → off
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f);
      SeqConfig run{120, 0.0f, true, true}; tick(&e, &run, SR, N);
      SeqConfig stop{120, 0.0f, false, true};
      int nev = tick(&e, &stop, SR, N);
      check(nev == 1 && !EV[0].on && EV[0].note == 60, "стоп → off звучащей ноты"); }

    // seq_on off (playing on): снять ноту, но клок продолжает идти (для арпа)
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f);
      SeqConfig on{120, 0.0f, true, true}; tick(&e, &on, SR, N);
      SeqConfig off{120, 0.0f, true, false};
      int nev = tick(&e, &off, SR, N);
      check(nev == 1 && !EV[0].on, "seq_on off → снял ноту");
      check(e.step != -1, "seq_on off не останавливает клок"); }

    if (g_fail == 0) { printf("OK: seq — все проверки пройдены\n"); return 0; }
    printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
