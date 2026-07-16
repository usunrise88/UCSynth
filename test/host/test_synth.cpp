// Host-тест менеджера голосов: poly-аллокация, reuse, oldest-steal, моно last-note/remove-anywhere,
// legato (нет ретригера) vs non-legato (ретригер), суммирование. Пул — статический в synth.
#include "synth.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static const float SR = 48000.0f;

static SynthParams defsp(int poly, float glide, bool legato)
{
    SynthParams sp{};
    sp.voice.osc[0] = { 0, 0.0f, 1.0f };            // sine — стабильный уровень для замеров
    sp.voice.osc[1] = { 0, 0.0f, 0.0f };
    sp.voice.osc[2] = { 0, 0.0f, 0.0f };
    sp.voice.noise_level = 0.0f; sp.voice.ring_level = 0.0f;
    sp.voice.cutoff_hz = 20000.0f; sp.voice.resonance = 0.0f;
    sp.voice.filt_mode = FILT_LP; sp.voice.flt_env_amt = 0.0f;
    sp.voice.amp_env = { 0.005f, 0.05f, 1.0f, 0.02f, false };
    sp.voice.flt_env = { 0.005f, 0.05f, 1.0f, 0.02f, false };
    sp.voice.lofi = false; sp.voice.lofi_bits = 16; sp.voice.latch = false;
    sp.voice.glide_time = glide;
    sp.poly_voices = poly; sp.legato = legato;
    return sp;
}

static float render_peak(const SynthParams *sp, float *buf, int N, int blocks)
{
    float mx = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        synth_render(sp, SR, buf, N);
        for (int i = 0; i < N; ++i) { const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
    }
    return mx;
}

int main()
{
    wavetable_init(SR);
    const int N = 64;
    float buf[64];

    // Poly-аллокация: 3 ноты → 3 голоса; note-off → 2
    { synth_init(); SynthParams sp = defsp(4, 0.0f, false);
      synth_note_on(&sp, 60, 100); synth_note_on(&sp, 64, 100); synth_note_on(&sp, 67, 100);
      synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 3, "poly: 3 ноты → 3 голоса");
      synth_note_off(&sp, 64);
      for (int b = 0; b < 400; ++b) synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 2, "poly: note-off → 2 голоса"); }

    // Reuse same note: та же нота дважды → один голос
    { synth_init(); SynthParams sp = defsp(4, 0.0f, false);
      synth_note_on(&sp, 60, 100); synth_render(&sp, SR, buf, N);
      synth_note_on(&sp, 60, 100); synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "reuse: та же нота → 1 голос"); }

    // Oldest-steal: poly=2, 3 ноты → 2 голоса
    { synth_init(); SynthParams sp = defsp(2, 0.0f, false);
      synth_note_on(&sp, 60, 100); synth_render(&sp, SR, buf, N);
      synth_note_on(&sp, 64, 100); synth_render(&sp, SR, buf, N);
      synth_note_on(&sp, 67, 100); synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 2, "steal: poly=2, 3 ноты → 2 голоса"); }

    // Моно last-note + возврат
    { synth_init(); SynthParams sp = defsp(1, 0.0f, false);
      synth_note_on(&sp, 60, 100); synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "моно: 1 голос");
      synth_note_on(&sp, 64, 100); synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "моно: перекрытие всё ещё 1 голос");
      synth_note_off(&sp, 64); synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "моно: note-off верхней → возврат (голос звучит)");
      synth_note_off(&sp, 60);
      for (int b = 0; b < 400; ++b) synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 0, "моно: последняя off → тишина"); }

    // Моно remove-anywhere: снятие НЕ верхней ноты не гасит голос
    { synth_init(); SynthParams sp = defsp(1, 0.0f, false);
      synth_note_on(&sp, 60, 100); synth_render(&sp, SR, buf, N);
      synth_note_on(&sp, 64, 100); synth_render(&sp, SR, buf, N);
      synth_note_off(&sp, 60);                        // снять нижнюю (не верх)
      for (int b = 0; b < 50; ++b) synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "моно remove-anywhere: голос жив (верх держится)"); }

    // Legato (sustain 0.5): перекрытие НЕ ретригерит → нет подскока амплитуды.
    // steady меряем ПОСЛЕ отстоя (иначе поймали бы атакующий транзиент, а не сустейн).
    { synth_init(); SynthParams sp = defsp(1, 0.0f, true);
      sp.voice.amp_env.s = 0.5f; sp.voice.amp_env.d = 0.03f;
      synth_note_on(&sp, 60, 100);
      for (int b = 0; b < 200; ++b) synth_render(&sp, SR, buf, N);   // отстой до сустейна
      const float steady = render_peak(&sp, buf, N, 20);
      synth_note_on(&sp, 64, 100);
      const float after = render_peak(&sp, buf, N, 80);
      check(after < steady * 1.25f, "legato: перекрытие без ретригера (нет подскока атаки)"); }

    // Non-legato (sustain 0.5): перекрытие ретригерит → подскок к атаке
    { synth_init(); SynthParams sp = defsp(1, 0.0f, false);
      sp.voice.amp_env.s = 0.5f; sp.voice.amp_env.d = 0.03f;
      synth_note_on(&sp, 60, 100);
      for (int b = 0; b < 200; ++b) synth_render(&sp, SR, buf, N);   // отстой до сустейна
      const float steady = render_peak(&sp, buf, N, 20);
      synth_note_on(&sp, 64, 100);
      const float after = render_peak(&sp, buf, N, 80);
      check(after > steady * 1.3f, "non-legato: перекрытие ретригерит (подскок)"); }

    // Суммирование: 4 голоса → сырой микс заметно >1 голоса и конечен; мастер-софтклип → ≤1
    { synth_init(); SynthParams sp = defsp(4, 0.0f, false);
      synth_note_on(&sp, 48, 100); synth_note_on(&sp, 52, 100);
      synth_note_on(&sp, 55, 100); synth_note_on(&sp, 59, 100);
      float mx = 0.0f; bool fin = true;
      for (int b = 0; b < 80; ++b) {
          synth_render(&sp, SR, buf, N);
          for (int i = 0; i < N; ++i) { if (!std::isfinite(buf[i])) fin = false;
              const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
      }
      check(fin, "сумма: без NaN");
      check(mx > 0.8f, "сумма: 4 голоса складываются (микс заметно > одного голоса ~0.5)");
      const float m = mx / (1.0f + mx);
      check(m <= 1.0f, "мастер софт-клип суммы → ≤1"); }

    if (g_fail == 0) printf("OK: synth — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
