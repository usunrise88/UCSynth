// Host-тест мод-матрицы: выбор источника, умножение на глубину, аккумуляция по слотам, гард NONE,
// пер-голосный источник velocity. Проверяем через AMP-приёмник — он даёт чистую проверку на тишину:
// amp_target = env_a·(1 + mod[AMP]); mod[AMP] = -1 → амплитуда 0 → выход ровно 0.
#include "voice.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }
static const float SR = 48000.0f;

// Яркий одиночный осц, фильтр открыт, матрица пуста.
static VoiceParams defparams()
{
    VoiceParams p{};
    p.osc[0] = { 1, 0.0f, 1.0f };            // saw
    p.cutoff_hz = 20000.0f; p.filt_mode = FILT_LP;
    p.amp_env = { 0.005f, 0.1f, 1.0f, 0.02f, false };
    p.flt_env = { 0.005f, 0.1f, 1.0f, 0.02f, false };
    p.lofi_bits = 16;
    return p;
}

// Пиковая |амплитуда| за blocks блоков (после warmup в сустейн).
static float peak(Voice *v, const VoiceParams *p, float *buf, int N, int blocks)
{
    float mx = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        voice_render(v, p, SR, buf, N);
        for (int i = 0; i < N; ++i) { const float a = std::fabs(buf[i]); if (a > mx) mx = a; }
    }
    return mx;
}

int main()
{
    wavetable_init(SR);
    const int N = 64;
    float buf[64];

    // Базлайн: матрица пуста → голос звучит (совместимость с доматричным трактом).
    {
        Voice v; voice_init(&v, 1);
        VoiceParams p = defparams();
        voice_note_on(&v, 69, 100, false, false);
        for (int b = 0; b < 20; ++b) voice_render(&v, &p, SR, buf, N);
        check(peak(&v, &p, buf, N, 10) > 0.3f, "матрица пуста → голос звучит");
    }

    // MODWHEEL→AMP, depth=-1, mod-wheel=1 → amp*0 → тишина. Проверяет источник+приёмник+глубину.
    {
        Voice v; voice_init(&v, 2);
        VoiceParams p = defparams();
        p.mod_src[MOD_SRC_MODWHEEL] = 1.0f;               // как поставил бы audio.cpp
        p.mtx[0] = { MOD_SRC_MODWHEEL, MOD_DST_AMP, -1.0f };
        voice_note_on(&v, 69, 100, false, false);
        check(peak(&v, &p, buf, N, 30) < 0.001f, "MODWHEEL→AMP depth=-1 → тишина");
    }

    // Аккумуляция: два слота MODWHEEL→AMP по -0.5 → суммарно -1 → тишина.
    {
        Voice v; voice_init(&v, 3);
        VoiceParams p = defparams();
        p.mod_src[MOD_SRC_MODWHEEL] = 1.0f;
        p.mtx[0] = { MOD_SRC_MODWHEEL, MOD_DST_AMP, -0.5f };
        p.mtx[3] = { MOD_SRC_MODWHEEL, MOD_DST_AMP, -0.5f };
        voice_note_on(&v, 69, 100, false, false);
        check(peak(&v, &p, buf, N, 30) < 0.001f, "два слота -0.5 суммируются в -1 → тишина");
    }

    // Гард NONE: src=NONE с ненулевой глубиной не действует (голос звучит).
    {
        Voice v; voice_init(&v, 4);
        VoiceParams p = defparams();
        p.mod_src[MOD_SRC_MODWHEEL] = 1.0f;
        p.mtx[0] = { MOD_SRC_NONE, MOD_DST_AMP, -1.0f };   // источник выключен → игнор
        voice_note_on(&v, 69, 100, false, false);
        for (int b = 0; b < 20; ++b) voice_render(&v, &p, SR, buf, N);
        check(peak(&v, &p, buf, N, 10) > 0.3f, "src=NONE игнорируется (звучит)");
    }

    // Пер-голосный источник velocity: vel=127 → velocity=1; VEL→AMP depth=-1 → тишина.
    {
        Voice v; voice_init(&v, 5);
        VoiceParams p = defparams();
        p.mtx[0] = { MOD_SRC_VELOCITY, MOD_DST_AMP, -1.0f };
        voice_note_on(&v, 69, 127, false, false);          // velocity = 1.0
        check(peak(&v, &p, buf, N, 30) < 0.001f, "VELOCITY(127)→AMP depth=-1 → тишина");
    }

    // Приёмник CUTOFF: MODWHEEL→CUTOFF depth<0 закрывает фильтр → яркая пила глушится (пик падает).
    {
        VoiceParams base = defparams();
        base.cutoff_hz = 20000.0f; base.resonance = 0.0f;
        Voice vo; voice_init(&vo, 6);
        voice_note_on(&vo, 69, 100, false, false);
        for (int b = 0; b < 20; ++b) voice_render(&vo, &base, SR, buf, N);
        const float open = peak(&vo, &base, buf, N, 20);

        VoiceParams closed = base;
        closed.mod_src[MOD_SRC_MODWHEEL] = 1.0f;
        closed.mtx[0] = { MOD_SRC_MODWHEEL, MOD_DST_CUTOFF, -1.0f };  // -OCT октав вниз
        Voice vc; voice_init(&vc, 7);
        voice_note_on(&vc, 69, 100, false, false);
        for (int b = 0; b < 20; ++b) voice_render(&vc, &closed, SR, buf, N);
        const float dark = peak(&vc, &closed, buf, N, 20);
        check(dark < open * 0.9f, "MODWHEEL→CUTOFF depth<0 закрывает фильтр (пик падает)");
    }

    if (g_fail == 0) printf("OK: matrix — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
