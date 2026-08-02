// Host-тест кодека паттерна (этап 7.4): round-trip 16 шагов (ноты, velocity, prob, p-locks), гарды.
#include "seq_codec.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static bool approx(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main()
{
    SeqStep pat[SEQ_STEPS];
    for (int s = 0; s < SEQ_STEPS; ++s) pat[s] = SeqStep{};

    // шаг 0: аккорд 60/64/67, vel 100, prob 1, два p-lock
    pat[0].active = true; pat[0].n_notes = 3;
    pat[0].notes[0] = 60; pat[0].notes[1] = 64; pat[0].notes[2] = 67;
    pat[0].velocity = 100; pat[0].trig_prob = 1.0f;
    pat[0].n_plocks = 2; pat[0].plocks[0] = { 20, 1234.5f }; pat[0].plocks[1] = { 79, 0.25f };
    // шаг 5: одна нота, prob 0.5
    pat[5].active = true; pat[5].n_notes = 1; pat[5].notes[0] = 48; pat[5].velocity = 80; pat[5].trig_prob = 0.5f;

    uint8_t blob[SEQ_BLOB_MAX];
    const size_t len = seq_serialize(blob, sizeof(blob), pat);
    check(len > 0, "serialize → непустой блоб");
    check(blob[0] == SEQ_FMT_VERSION, "версия в заголовке");

    SeqStep out[SEQ_STEPS];
    check(seq_deserialize(blob, len, out), "deserialize ok");
    // шаг 0
    check(out[0].active && out[0].n_notes == 3 && out[0].notes[0] == 60 && out[0].notes[2] == 67, "шаг 0: аккорд");
    check(out[0].velocity == 100 && approx(out[0].trig_prob, 1.0f), "шаг 0: vel/prob");
    check(out[0].n_plocks == 2 && out[0].plocks[0].id == 20 && approx(out[0].plocks[0].val, 1234.5f), "шаг 0: p-lock 0");
    check(out[0].plocks[1].id == 79 && approx(out[0].plocks[1].val, 0.25f), "шаг 0: p-lock 1");
    // шаг 5
    check(out[5].active && out[5].n_notes == 1 && out[5].notes[0] == 48 && approx(out[5].trig_prob, 0.5f), "шаг 5");
    // пустой шаг
    check(!out[1].active && out[1].n_notes == 0 && out[1].n_plocks == 0, "шаг 1 пуст");

    // гарды
    SeqStep g[SEQ_STEPS];
    check(!seq_deserialize(blob, 0, g), "deserialize len=0 → false");
    uint8_t z[8] = {0};
    check(!seq_deserialize(z, sizeof(z), g), "deserialize version=0 → false");
    check(!seq_deserialize(blob, len - 1, g), "deserialize усечённого → false");

    // per-step round-trip (провод шлёт паттерн пошагово — кадр ограничен 255 Б)
    {
        SeqStep st = SeqStep{};
        st.active = true; st.n_notes = 2; st.notes[0] = 60; st.notes[1] = 67;
        st.velocity = 110; st.trig_prob = 0.75f;
        st.n_plocks = 1; st.plocks[0] = { 5, -3.5f };
        uint8_t b[64];
        const size_t n = seq_step_serialize(b, sizeof(b), &st);
        check(n > 0, "step serialize");
        SeqStep so; const size_t c = seq_step_deserialize(b, n, &so);
        check(c == n, "step deserialize потребил ровно n байт");
        check(so.active && so.n_notes == 2 && so.notes[1] == 67 && so.velocity == 110 && approx(so.trig_prob, 0.75f), "step round-trip");
        check(so.n_plocks == 1 && so.plocks[0].id == 5 && approx(so.plocks[0].val, -3.5f), "step p-lock round-trip");
        check(seq_step_deserialize(b, 1, &so) == 0, "step deserialize len<2 → 0");
    }

    // нехватка cap → serialize=0
    uint8_t tiny[4];
    check(seq_serialize(tiny, sizeof(tiny), pat) == 0, "малый буфер → serialize=0");

    if (g_fail == 0) { printf("OK: seq_codec — все проверки пройдены\n"); return 0; }
    printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
