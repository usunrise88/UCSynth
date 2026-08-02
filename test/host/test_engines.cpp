// Host-тест движков голоса (этап 12.3/12.4): FM (2-оператора) и Karplus-Strong. Гоняем через
// voice_render (полный тракт голоса), анализируем спектр/затухание DFT-проекцией. Без ESP-IDF.
#include "voice.h"
#include "wavetable.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static constexpr double PI = 3.14159265358979323846;
static constexpr float SR = 48000.0f;

static void check(bool ok, const char *w) { if (!ok) { printf("FAIL: %s\n", w); g_fail++; } }

static double mag_at(const float *x, int N, double cyc)
{
    double re = 0, im = 0;
    for (int n = 0; n < N; ++n) {
        const double a = 2.0 * PI * cyc * (double)n;
        re += x[n] * std::cos(a);
        im -= x[n] * std::sin(a);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)N;
}

static float note_hz(int note) { return 440.0f * std::exp2(((float)note - 69.0f) / 12.0f); }

// Голос с открытым фильтром и ровной огибающей — чтобы мерить чистый спектр генератора.
static VoiceParams engparams()
{
    VoiceParams p{};
    p.osc[0] = { 0, 0.0f, 1.0f, OSC_WAVETABLE };
    p.osc[1] = { 0, 0.0f, 0.0f, OSC_WAVETABLE };
    p.osc[2] = { 0, 0.0f, 0.0f, OSC_WAVETABLE };
    p.cutoff_hz = 20000.0f; p.resonance = 0.0f; p.filt_mode = FILT_LP; p.flt_env_amt = 0.0f;
    p.amp_env = { 0.002f, 0.05f, 1.0f, 0.02f, false };   // attack→sustain=1: ровная амплитуда
    p.flt_env = { 0.002f, 0.05f, 1.0f, 0.02f, false };
    p.fm_ratio = 1.0f; p.fm_index = 0.0f;
    p.ks_damp = 0.3f; p.ks_decay = 0.995f; p.ks_pluck = 0.8f;
    return p;
}

// Прогреть огибающую (несколько блоков), затем снять длинный буфер одним рендером для DFT.
static void capture(Voice *v, const VoiceParams *p, float *big, int N)
{
    float warm[128];
    for (int b = 0; b < 24; ++b) voice_render(v, p, SR, warm, 128);
    voice_render(v, p, SR, big, N);
}

int main()
{
    wavetable_init(SR);
    const int N = 8192;
    static float buf[8192];

    // ---------------- FM (12.3) ----------------
    // Несущая на phase[0] и модулятор на phase[1] при ratio=1 фазово синхронны → энергия боковых
    // ложится на нечётные обертоны несущей (m3,m5), а не строго на 2·fc. Поэтому проверяем сам признак
    // FM: с ростом индекса несущая истощается, обертоны растут; отношение частот меняет спектр.
    const int note = 57;                 // A3 = 220 Гц
    const double fc = note_hz(note);
    const double cyc1 = fc / SR;

    // index=0 → почти чистая несущая
    double m1_off, high_off, m3_r1;
    {
        Voice v; voice_init(&v, 1);
        VoiceParams p = engparams();
        p.engine = ENG_FM; p.fm_ratio = 1.0f; p.fm_index = 0.0f;
        voice_note_on(&v, (uint8_t)note, 100, false, false);
        capture(&v, &p, buf, N);
        m1_off = mag_at(buf, N, cyc1);
        high_off = mag_at(buf, N, 3 * cyc1) + mag_at(buf, N, 5 * cyc1);
        check(m1_off > 0.3, "FM index=0: несущая на частоте ноты (note→carrier)");
        double even = mag_at(buf, N, 2 * cyc1) + mag_at(buf, N, 4 * cyc1);
        check((high_off + even) < 0.4 * m1_off, "FM index=0: спектр чистый (мало обертонов)");
    }

    // index>0, ratio=1 → несущая истощается, нечётные обертоны растут
    {
        Voice v; voice_init(&v, 2);
        VoiceParams p = engparams();
        p.engine = ENG_FM; p.fm_ratio = 1.0f; p.fm_index = 3.0f;
        voice_note_on(&v, (uint8_t)note, 100, false, false);
        capture(&v, &p, buf, N);
        bool fin = true;
        for (int i = 0; i < N; ++i) if (!std::isfinite(buf[i])) fin = false;
        check(fin, "FM: без NaN на большом индексе");
        const double m1 = mag_at(buf, N, cyc1);
        m3_r1 = mag_at(buf, N, 3 * cyc1);
        const double high = m3_r1 + mag_at(buf, N, 5 * cyc1);
        check(m1 < 0.5 * m1_off, "FM index>0: несущая истощается (энергия в боковые)");
        check(high > high_off + 0.1, "FM index>0: обертоны/боковые растут");
    }

    // отношение частот меняет спектр (ratio=2 при том же индексе даёт другую раскладку боковых)
    {
        Voice v; voice_init(&v, 3);
        VoiceParams p = engparams();
        p.engine = ENG_FM; p.fm_ratio = 2.0f; p.fm_index = 3.0f;
        voice_note_on(&v, (uint8_t)note, 100, false, false);
        capture(&v, &p, buf, N);
        const double m3_r2 = mag_at(buf, N, 3 * cyc1);
        check(std::fabs(m3_r2 - m3_r1) > 0.05, "FM: fm_ratio меняет спектр");
    }

    // ---------------- Karplus-Strong (12.4) ----------------
    const int knote = 57;                            // 220 Гц
    const double kfc = note_hz(knote);
    const int klag = (int)(SR / kfc + 0.5f);         // длина линии = период фундамента

    // щипок звучит и затухает (ks_decay), без NaN
    {
        Voice v; voice_init(&v, 10);
        VoiceParams p = engparams();
        p.engine = ENG_KS; p.ks_decay = 0.995f; p.ks_damp = 0.3f; p.ks_pluck = 0.8f;
        p.amp_env = { 0.001f, 0.05f, 1.0f, 0.02f, false };   // gate держим — спад от KS, не от release
        voice_note_on(&v, (uint8_t)knote, 100, false, false);
        float blk[128];
        double rms_early = 0, rms_late = 0;
        bool fin = true;
        for (int b = 0; b < 200; ++b) {
            voice_render(&v, &p, SR, blk, 128);
            double e = 0;
            for (int i = 0; i < 128; ++i) { if (!std::isfinite(blk[i])) fin = false; e += blk[i] * blk[i]; }
            e = std::sqrt(e / 128.0);
            if (b == 3) rms_early = e;
            if (b == 150) rms_late = e;
        }
        check(fin, "KS: без NaN");
        check(rms_early > 0.02, "KS: щипок звучит");
        check(rms_late < 0.6 * rms_early, "KS: хвост затухает (ks_decay)");
    }

    // питч = sr/длина: автокорреляция пик на lag = период фундамента
    {
        Voice v; voice_init(&v, 11);
        VoiceParams p = engparams();
        p.engine = ENG_KS; p.ks_decay = 0.999f; p.ks_damp = 0.2f; p.ks_pluck = 0.9f;
        p.amp_env = { 0.001f, 0.05f, 1.0f, 0.02f, false };
        voice_note_on(&v, (uint8_t)knote, 100, false, false);
        float warm[128];
        for (int b = 0; b < 4; ++b) voice_render(&v, &p, SR, warm, 128);
        const int M = 4096;
        static float kb[4096];
        voice_render(&v, &p, SR, kb, M);
        auto ac = [&](int lag) {
            double num = 0, den = 0;
            for (int nn = 0; nn + lag < M; ++nn) num += kb[nn] * kb[nn + lag];
            for (int nn = 0; nn < M; ++nn) den += kb[nn] * kb[nn];
            return den > 0 ? num / den : 0.0;
        };
        const double a_true = ac(klag);
        const double a_half = ac(klag / 2);
        const double a_off  = ac(klag + klag / 3);
        check(a_true > 0.3, "KS: сильная периодичность на lag = sr/freq");
        check(a_true > a_half && a_true > a_off, "KS: питч = sr/длина (пик автокорреляции на нужном lag)");
    }

    printf(g_fail ? "FAILED (%d)\n" : "OK: engines — все проверки пройдены\n", g_fail);
    return g_fail ? 1 : 0;
}
