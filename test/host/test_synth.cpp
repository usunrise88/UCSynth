// Host-тест менеджера голосов: poly-аллокация, reuse, oldest-steal, моно last-note/remove-anywhere,
// legato (нет ретригера) vs non-legato (ретригер), суммирование. Пул — статический в synth.
#include "synth.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <initializer_list>

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

    // note-on и note-off в ОДНОМ блоке (оба до первого рендера) — реально бывает: audio_task
    // дренит очередь целиком, а comm разбирает до 128 байт входа подряд, так что пара кадров из
    // одного USB-пакета кладётся за микросекунды. Голос обязан отпуститься (регресс B-01).
    { synth_init(); SynthParams sp = defsp(4, 0.0f, false);
      synth_note_on(&sp, 60, 100);
      synth_note_off(&sp, 60);                          // ← без synth_render между ними
      for (int b = 0; b < 400; ++b) synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 0, "poly: note-on+off в одном блоке → голос освободился");
      check(render_peak(&sp, buf, N, 20) < 0.001f, "poly: и не звучит"); }

    { synth_init(); SynthParams sp = defsp(1, 0.0f, false);
      synth_note_on(&sp, 60, 100);
      synth_note_off(&sp, 60);
      for (int b = 0; b < 400; ++b) synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 0, "моно: note-on+off в одном блоке → голос освободился"); }

    // Уменьшение polyphony: голоса выше нового предела обязаны дотикать релиз до IDLE, а не
    // висеть «активными» вечно (регресс B-10 — их огибающие не двигались, т.к. не рендерились).
    { synth_init(); SynthParams sp = defsp(8, 0.0f, false);
      synth_set_poly(8);
      synth_note_on(&sp, 60, 100); synth_note_on(&sp, 64, 100);
      synth_note_on(&sp, 67, 100); synth_note_on(&sp, 71, 100);
      synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 4, "poly-смена: 4 голоса до смены");
      sp.poly_voices = 1;
      synth_set_poly(1);                                // = all-notes-off
      for (int b = 0; b < 4000; ++b) synth_render(&sp, SR, buf, N);   // ~5 с
      check(synth_active_count() == 0, "poly 8→1: голоса выше предела дотикали релиз до IDLE"); }

    // Смена polyphony и note-on в одном блоке: synth_set_poly идёт ДО дренажа очереди, поэтому
    // нота не должна быть съедена вызванным ею all-notes-off (регресс B-32).
    { synth_init(); SynthParams sp = defsp(4, 0.0f, false);
      synth_set_poly(4); synth_render(&sp, SR, buf, N);
      sp.poly_voices = 2;
      synth_set_poly(2);                                // порядок как в audio_task
      synth_note_on(&sp, 60, 100);
      synth_render(&sp, SR, buf, N);
      check(synth_active_count() == 1, "смена poly + note-on в одном блоке → нота не пропала"); }

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
      // Мастер — HARD-CLAMP (D-010), не второй soft-clip. Второй soft-clip сжимал тракт дважды:
      // одиночный голос 0.5 выходил на 0.333 (−9.5 dBFS), а сумма 8 голосов (~4) на 0.8 — аккорд
      // громче одной ноты в 2.4 раза вместо 8.
      const float clamped = mx > 1.0f ? 1.0f : mx;
      check(clamped <= 1.0f, "мастер hard-clamp суммы → ≤1"); }

    // Гейн-стейджинг: мастер прозрачен для одиночного голоса и ограничивает сумму.
    { synth_init(); SynthParams sp = defsp(8, 0.0f, false);
      synth_set_poly(8);
      synth_note_on(&sp, 60, 100);
      for (int b = 0; b < 300; ++b) synth_render(&sp, SR, buf, N);   // в сустейн
      const float one = render_peak(&sp, buf, N, 40);
      // Голос уже мягко клипует сам (voice.cpp), поэтому его пик ≈0.5 — мастер не должен его трогать.
      const float one_master = one > 1.0f ? 1.0f : one;
      check(one > 0.4f && one < 0.6f, "один голос: пик ~0.5 после soft-clip голоса");
      check(std::fabs(one_master - one) < 1e-6f,
            "один голос: мастер hard-clamp прозрачен (не второй soft-clip)");

      for (uint8_t n : { (uint8_t)64, (uint8_t)67, (uint8_t)71, (uint8_t)74,
                         (uint8_t)77, (uint8_t)79, (uint8_t)83 }) synth_note_on(&sp, n, 100);
      for (int b = 0; b < 300; ++b) synth_render(&sp, SR, buf, N);
      const float eight = render_peak(&sp, buf, N, 40);
      check(eight > one * 2.0f, "8 голосов: сырая сумма заметно больше одного голоса");
      const float eight_master = eight > 1.0f ? 1.0f : eight;
      check(eight_master <= 1.0f, "8 голосов: мастер ограничивает в [-1,1] (вход FX калиброван)"); }

    if (g_fail == 0) printf("OK: synth — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
