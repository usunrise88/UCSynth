// Host-тест секвенсора/арпа (этапы 7.1–7.3): темп-клок (bpm/swing/транспорт/врап), паттерн (ноты, аккорд,
// release, p-lock, trig-probability), арпеджиатор (up/down/updown/random, октавы, held/hold). Чистый движок.
#include "seq_engine.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

static SeqNoteEvent EV[16];
static int tick(SeqEngine *e, const SeqConfig *cfg, float sr, int n) { return seq_tick(e, cfg, sr, n, EV, 16); }

// Полный билдер конфига (избегаем -Wmissing-field-initializers и повторов).
static SeqConfig CFG(int bpm = 120, float swing = 0.0f, bool playing = true, bool seq_on = true,
                     bool arp_on = false, uint8_t am = ARP_UP, int ao = 1, int ar = 2, bool ah = false)
{
    return SeqConfig{ bpm, swing, playing, seq_on, arp_on, am, ao, ar, ah };
}

static void set_note(SeqEngine *e, int step, uint8_t note, uint8_t vel, float prob)
{
    e->pattern[step].active = true; e->pattern[step].n_notes = 1; e->pattern[step].notes[0] = note;
    e->pattern[step].velocity = vel; e->pattern[step].trig_prob = prob;
}

int main()
{
    const float SR = 48000.0f;
    const int   N  = 64;

    // ---------- клок (7.1) ----------
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(120, 0, false, true);
      tick(&e, &c, SR, N); check(e.step == -1, "стоп: step=-1"); }
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(120, 0, true, true);
      tick(&e, &c, SR, N); check(e.step == 0, "play edge → шаг 0"); }
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(120, 0, true, false);
      tick(&e, &c, SR, N);
      long s = 0; int g = 0; while (e.step == 0 && g++ < 2000) { tick(&e, &c, SR, N); s += N; }
      check(e.step == 1 && std::labs(s - 6000) <= N, "шаг ≈ 6000 семплов @120bpm"); }
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(120, 0, true, false);
      tick(&e, &c, SR, N);
      int steps = 0, prev = e.step;
      for (long s = 0; s < 96000; s += N) { tick(&e, &c, SR, N); if (e.step != prev) { steps++; prev = e.step; } }
      check(steps >= 15 && steps <= 16, "≈16 переходов за 2 c @120bpm"); }
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(120, 1, true, false);
      tick(&e, &c, SR, N);
      long s0 = 0; int g = 0; while (e.step == 0 && g++ < 4000) { tick(&e, &c, SR, N); s0 += N; }
      check(std::labs(s0 - 9000) <= N, "swing: чётный ≈9000");
      long s1 = 0; g = 0; while (e.step == 1 && g++ < 4000) { tick(&e, &c, SR, N); s1 += N; }
      check(std::labs(s1 - 3000) <= N, "swing: нечётный ≈3000"); }
    { SeqEngine e; seq_init(&e); SeqConfig c = CFG(240, 0, true, false);
      tick(&e, &c, SR, N);
      bool wrapped = false; int prev = e.step;
      for (int g = 0; g < 200000 / N; ++g) { tick(&e, &c, SR, N); if (prev == SEQ_STEPS - 1 && e.step == 0) wrapped = true; prev = e.step; }
      check(wrapped, "врап 15→0"); }

    // ---------- паттерн (7.2) ----------
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f); SeqConfig c = CFG(120, 0, true, true);
      int nev = tick(&e, &c, SR, N);
      check(nev == 1 && EV[0].on == 1 && EV[0].note == 60 && EV[0].vel == 100, "шаг 0 → note-on 60 vel 100"); }
    { SeqEngine e; seq_init(&e);
      e.pattern[0].active = true; e.pattern[0].n_notes = 3;
      e.pattern[0].notes[0] = 60; e.pattern[0].notes[1] = 64; e.pattern[0].notes[2] = 67;
      e.pattern[0].velocity = 90; e.pattern[0].trig_prob = 1.0f;
      SeqConfig c = CFG(120, 0, true, true);
      int nev = tick(&e, &c, SR, N); int ons = 0; for (int i = 0; i < nev; ++i) if (EV[i].on) ons++;
      check(ons == 3, "аккорд шага → 3 note-on"); }
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f); set_note(&e, 1, 64, 100, 1.0f);
      SeqConfig c = CFG(120, 0, true, true);
      tick(&e, &c, SR, N);
      bool off60 = false, on64 = false; int g = 0;
      while (e.step == 0 && g++ < 2000) { int nev = tick(&e, &c, SR, N);
          for (int i = 0; i < nev; ++i) { if (!EV[i].on && EV[i].note == 60) off60 = true; if (EV[i].on && EV[i].note == 64) on64 = true; } }
      check(e.step == 1 && off60 && on64, "0→1: off 60 + on 64"); }
    { SeqEngine e; seq_init(&e);
      e.pattern[0].n_plocks = 1; e.pattern[0].plocks[0] = { 20, 1234.0f };
      SeqConfig c = CFG(120, 0, true, true);
      tick(&e, &c, SR, N);
      float v = 0.0f;
      check(seq_plock(&e, 20, &v) && v == 1234.0f, "p-lock шага 0 активен");
      check(!seq_plock(&e, 21, &v), "p-lock не задевает другой id");
      int g = 0; while (e.step == 0 && g++ < 2000) tick(&e, &c, SR, N);
      check(e.step == 1 && !seq_plock(&e, 20, &v), "p-lock снят на шаге без него"); }
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 72, 100, 0.0f); SeqConfig c = CFG(120, 0, true, true);
      check(tick(&e, &c, SR, N) == 0, "trig_prob=0 → не играет"); }
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f);
      SeqConfig run = CFG(120, 0, true, true); tick(&e, &run, SR, N);
      SeqConfig stop = CFG(120, 0, false, true);
      int nev = tick(&e, &stop, SR, N);
      check(nev == 1 && !EV[0].on && EV[0].note == 60, "стоп → off звучащей ноты"); }
    { SeqEngine e; seq_init(&e); set_note(&e, 0, 60, 100, 1.0f);
      SeqConfig on = CFG(120, 0, true, true); tick(&e, &on, SR, N);
      SeqConfig off = CFG(120, 0, true, false);
      int nev = tick(&e, &off, SR, N);
      check(nev == 1 && !EV[0].on, "seq_on off → снял ноту"); check(e.step != -1, "seq_on off не останавливает клок"); }

    // ---------- арпеджиатор (7.3) ----------
    // собрать первые 4 сыгранные арпом ноты
    auto arp_first4 = [&](SeqEngine &e, SeqConfig c, uint8_t out[4]) {
        int nev = tick(&e, &c, SR, N); int k = 0;
        for (int i = 0; i < nev && k < 4; ++i) if (EV[i].on) out[k++] = EV[i].note;
        for (int g = 0; g < 60000 && k < 4; ++g) { int m = tick(&e, &c, SR, N);
            for (int i = 0; i < m && k < 4; ++i) if (EV[i].on) out[k++] = EV[i].note; }
        return k;
    };

    { SeqEngine e; seq_init(&e);
      seq_arp_note(&e, 60, true, false); seq_arp_note(&e, 64, true, false); seq_arp_note(&e, 67, true, false);
      uint8_t g[4]; int k = arp_first4(e, CFG(120, 0, true, false, true, ARP_UP, 1, 2, false), g);
      check(k == 4 && g[0] == 60 && g[1] == 64 && g[2] == 67 && g[3] == 60, "арп up: 60,64,67,60(wrap)"); }

    { SeqEngine e; seq_init(&e);
      seq_arp_note(&e, 60, true, false); seq_arp_note(&e, 64, true, false); seq_arp_note(&e, 67, true, false);
      uint8_t g[4]; int k = arp_first4(e, CFG(120, 0, true, false, true, ARP_DOWN, 1, 2, false), g);
      check(k == 4 && g[0] == 67 && g[1] == 64 && g[2] == 60 && g[3] == 67, "арп down: 67,64,60,67"); }

    { SeqEngine e; seq_init(&e);
      seq_arp_note(&e, 60, true, false); seq_arp_note(&e, 64, true, false);
      uint8_t g[4]; int k = arp_first4(e, CFG(120, 0, true, false, true, ARP_UP, 2, 2, false), g);
      check(k == 4 && g[0] == 60 && g[1] == 64 && g[2] == 72 && g[3] == 76, "арп 2 октавы: 60,64,72,76"); }

    // held/hold: без hold отпускание убирает ноту; с hold — держит, новое нажатие после отпускания заново
    { SeqEngine e; seq_init(&e);
      seq_arp_note(&e, 60, true, false); seq_arp_note(&e, 64, true, false);
      check(e.n_held == 2, "held: 2 нажаты");
      seq_arp_note(&e, 60, false, false);
      check(e.n_held == 1 && e.held[0] == 64, "held без hold: отпустил 60 → остался 64"); }
    { SeqEngine e; seq_init(&e);
      seq_arp_note(&e, 60, true, true); seq_arp_note(&e, 60, false, true);
      check(e.n_held == 1, "hold: нота держится после отпускания");
      seq_arp_note(&e, 64, true, true);
      check(e.n_held == 1 && e.held[0] == 64, "hold: новое нажатие после отпускания — заново"); }

    // арп off снимает звучащую ноту
    { SeqEngine e; seq_init(&e); seq_arp_note(&e, 60, true, false);
      SeqConfig on = CFG(120, 0, true, false, true, ARP_UP, 1, 2, false); tick(&e, &on, SR, N);
      check(e.arp_sounding == 60, "арп играет 60");
      SeqConfig off = CFG(120, 0, true, false, false, ARP_UP, 1, 2, false);
      int nev = tick(&e, &off, SR, N);
      check(e.arp_sounding == 255 && nev >= 1 && !EV[0].on && EV[0].note == 60, "арп off → снял ноту"); }

    if (g_fail == 0) { printf("OK: seq — все проверки пройдены\n"); return 0; }
    printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
