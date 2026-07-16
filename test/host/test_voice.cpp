// Host-тест голоса: нота играет и затухает, headroom (soft-clip), lo-fi квантует, latch-дрон.
#include "voice.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static const float SR = 48000.0f;

// Дефолты «как одиночный осц»: только осц1, фильтр открыт, без модуляции.
static VoiceParams defparams()
{
    VoiceParams p{};
    p.osc[0] = { 1, 0.0f, 1.0f };            // saw
    p.osc[1] = { 0, 0.0f, 0.0f };
    p.osc[2] = { 0, 0.0f, 0.0f };
    p.noise_level = 0.0f; p.ring_level = 0.0f;
    p.cutoff_hz = 20000.0f; p.resonance = 0.0f; p.filt_mode = FILT_LP; p.flt_env_amt = 0.0f;
    p.amp_env = { 0.005f, 0.1f, 1.0f, 0.02f, false };
    p.flt_env = { 0.005f, 0.1f, 1.0f, 0.02f, false };
    p.lofi = false; p.lofi_bits = 16; p.latch = false;
    return p;
}

int main()
{
    wavetable_init(SR);            // голос зовёт wavetable_sample — таблицы нужны до рендера

    const int N = 64;
    float buf[64];

    // Нота играет → звук, конечен, ограничен; после release → тишина
    {
        Voice v; voice_init(&v, 1);
        VoiceParams p = defparams();
        voice_note_on(&v, 69, 100, false);
        float mx = 0.0f; bool fin = true;
        for (int b = 0; b < 50; ++b) {
            voice_render(&v, &p, SR, buf, N);
            for (int i = 0; i < N; ++i) { if (!std::isfinite(buf[i])) fin = false;
                const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
        }
        check(fin, "нота: без NaN");
        check(mx > 0.3f, "нота: есть звук");
        check(mx <= 1.001f, "нота: |out| ≤ 1");

        voice_note_off(&v, 69);
        for (int b = 0; b < 400; ++b) voice_render(&v, &p, SR, buf, N);
        float mx2 = 0.0f;
        for (int i = 0; i < N; ++i) { const float a = std::fabs(buf[i]); if (a > mx2) mx2 = a; }
        check(mx2 < 0.01f, "после release → тишина");
    }

    // Headroom: все осц + шум + ring на максимум → soft-clip держит |out| ≤ 1
    {
        Voice v; voice_init(&v, 7);
        VoiceParams p = defparams();
        p.osc[0] = { 1, 0.0f, 1.0f }; p.osc[1] = { 2, 0.1f, 1.0f }; p.osc[2] = { 3, -0.1f, 1.0f };
        p.noise_level = 1.0f; p.ring_level = 1.0f;
        voice_note_on(&v, 60, 127, false);
        float mx = 0.0f; bool fin = true;
        for (int b = 0; b < 50; ++b) {
            voice_render(&v, &p, SR, buf, N);
            for (int i = 0; i < N; ++i) { if (!std::isfinite(buf[i])) fin = false;
                const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
        }
        check(fin, "headroom: без NaN");
        check(mx <= 1.001f, "headroom: |out| ≤ 1 (soft-clip)");
    }

    // Lo-fi: 2 бита → мало уникальных уровней
    {
        Voice v; voice_init(&v, 3);
        VoiceParams p = defparams();
        p.lofi = true; p.lofi_bits = 2;
        voice_note_on(&v, 72, 100, false);
        for (int b = 0; b < 20; ++b) voice_render(&v, &p, SR, buf, N);   // прогрев в сустейн
        voice_render(&v, &p, SR, buf, N);
        float uniq[64]; int nu = 0;
        for (int i = 0; i < N; ++i) {
            bool seen = false;
            for (int k = 0; k < nu; ++k) if (std::fabs(uniq[k] - buf[i]) < 1e-6f) { seen = true; break; }
            if (!seen && nu < 64) uniq[nu++] = buf[i];
        }
        check(nu <= 8, "lofi 2 бита → мало уровней (≤8)");
    }

    // Latch: note-off не гасит дрон; снятие latch → релиз
    {
        Voice v; voice_init(&v, 9);
        VoiceParams p = defparams();
        p.latch = true;
        voice_note_on(&v, 64, 100, true);
        for (int b = 0; b < 200; ++b) voice_render(&v, &p, SR, buf, N);   // в сустейн
        voice_note_off(&v, 64);
        for (int b = 0; b < 200; ++b) voice_render(&v, &p, SR, buf, N);   // latch держит
        float mx = 0.0f;
        for (int i = 0; i < N; ++i) { const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
        check(mx > 0.3f, "latch: дрон держится после note-off");

        p.latch = false;
        for (int b = 0; b < 400; ++b) voice_render(&v, &p, SR, buf, N);   // снятие latch → релиз
        float mx2 = 0.0f;
        for (int i = 0; i < N; ++i) { const float a = std::fabs(buf[i]); if (a > mx2) mx2 = a; }
        check(mx2 < 0.01f, "снятие latch → релиз в тишину");
    }

    if (g_fail == 0) printf("OK: voice — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
