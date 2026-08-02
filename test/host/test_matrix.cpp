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
    p.osc[0] = { 1, 0.0f, 1.0f, OSC_WAVETABLE };            // saw
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

    // --- PITCH: раньше не покрывался вовсе, а именно там жил незаклампленный фазовый аккумулятор ---
    // Сумма матрицы не ограничена: 8 слотов × depth ±1 = ±8, то есть ±192 полутона. Плюс детюн ±24 —
    // частота уходит далеко за Найквист, inc > 1, и заворот «одним вычитанием» перестаёт работать:
    // фаза растёт со скоростью inc−1, теряет точность и осциллятор залипает на константе (тишина).
    // Замер до правки: нота 127 + детюн +24 + MODWHEEL→PITCH depth 1 → через ~2 мин пик 0.0000
    // при phase = 1.84e7.
    {
        VoiceParams p = defparams();
        p.osc[0].detune_semi = 24.0f;
        p.mod_src[MOD_SRC_MODWHEEL] = 1.0f;
        for (int s = 0; s < 8; ++s) p.mtx[s] = { MOD_SRC_MODWHEEL, MOD_DST_PITCH, 1.0f };  // сумма = 8

        Voice v; voice_init(&v, 0x1234u);
        voice_note_on(&v, 127, 127, false, false);
        for (int b = 0; b < 200; ++b) voice_render(&v, &p, SR, buf, N);   // выйти в сустейн

        const float early = peak(&v, &p, buf, N, 50);
        // ~90 с звучания: у сломанного заворота фаза за это время уползает в область, где ulp ≥ 1
        for (int b = 0; b < 68000; ++b) voice_render(&v, &p, SR, buf, N);
        const float late = peak(&v, &p, buf, N, 50);

        check(std::isfinite(early) && std::isfinite(late), "PITCH×8: без NaN");
        check(late > early * 0.25f,
              "PITCH×8: долгая нота не уходит в тишину (фаза заворачивается при inc>1)");
        for (int j = 0; j < 3; ++j) {
            check(v.phase[j] >= 0.0f && v.phase[j] < 1.0f, "PITCH×8: фаза осталась в [0,1)");
        }
    }

    // Кламп pitch-модуляции: даже при сумме матрицы 8 сдвиг не должен превышать заявленные ±2 октавы.
    {
        VoiceParams p1 = defparams();
        p1.mod_src[MOD_SRC_MODWHEEL] = 1.0f;
        p1.mtx[0] = { MOD_SRC_MODWHEEL, MOD_DST_PITCH, 1.0f };            // ровно ±2 окт

        VoiceParams p8 = p1;
        for (int s = 0; s < 8; ++s) p8.mtx[s] = { MOD_SRC_MODWHEEL, MOD_DST_PITCH, 1.0f };  // сумма 8

        // Одна и та же нота: спектр обоих вариантов должен совпасть, т.к. сумма клампится в 1.
        auto rms_at = [&](const VoiceParams *pp) {
            Voice v; voice_init(&v, 0x77u);
            voice_note_on(&v, 48, 100, false, false);
            for (int b = 0; b < 300; ++b) voice_render(&v, pp, SR, buf, N);
            double acc = 0.0;
            for (int b = 0; b < 100; ++b) {
                voice_render(&v, pp, SR, buf, N);
                for (int i = 0; i < N; ++i) acc += (double)buf[i] * buf[i];
            }
            return (float)std::sqrt(acc / (100.0 * N));
        };
        const float r1 = rms_at(&p1), r8 = rms_at(&p8);
        check(std::fabs(r1 - r8) < 0.02f, "PITCH: сумма матрицы клампится (×1 и ×8 звучат одинаково)");
    }

    // --- WAVEPOS: тоже не покрывался. Морф должен менять форму и оставаться в диапазоне. ---
    {
        VoiceParams p = defparams();
        p.mod_src[MOD_SRC_MODWHEEL] = 1.0f;

        Voice a; voice_init(&a, 0x55u); voice_note_on(&a, 60, 100, false, false);
        for (int b = 0; b < 300; ++b) voice_render(&a, &p, SR, buf, N);
        const float plain = peak(&a, &p, buf, N, 40);

        p.mtx[0] = { MOD_SRC_MODWHEEL, MOD_DST_WAVEPOS, 1.0f };
        Voice c; voice_init(&c, 0x55u); voice_note_on(&c, 60, 100, false, false);
        bool fin = true;
        for (int b = 0; b < 300; ++b) {
            voice_render(&c, &p, SR, buf, N);
            for (int i = 0; i < N; ++i) if (!std::isfinite(buf[i]) || std::fabs(buf[i]) > 1.01f) fin = false;
        }
        const float morphed = peak(&c, &p, buf, N, 40);
        check(fin, "WAVEPOS: выход конечен и в [-1,1]");
        check(std::fabs(morphed - plain) > 1e-4f, "WAVEPOS: морф реально меняет форму");
    }

    if (g_fail == 0) printf("OK: matrix — все проверки пройдены\n");
    return g_fail ? 1 : 0;
}
