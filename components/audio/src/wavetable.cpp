#include "wavetable.h"
#include <cmath>

// Таблицы в RAM (4 формы × 2049 × 4 Б ≈ 32 КБ во внутреннем DRAM). +1 семпл — guard-копия
// первого, чтобы линейная интерполяция на стыке цикла не выходила за границу массива.
// (Хранить во flash можно предрасчётом offline; пока — простейший рабочий вариант.)
static float s_tables[WAVE_COUNT][WT_LEN + 1];

void wavetable_init(void)
{
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (int i = 0; i < WT_LEN; ++i) {
        const float p = (float)i / (float)WT_LEN;             // [0,1)
        s_tables[WAVE_SINE][i]   = sinf(kTwoPi * p);
        s_tables[WAVE_SAW][i]    = 2.0f * p - 1.0f;           // наивная пила (без band-limit)
        s_tables[WAVE_SQUARE][i] = (p < 0.5f) ? 1.0f : -1.0f;
        s_tables[WAVE_TRI][i]    = (p < 0.5f) ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
    }
    for (int w = 0; w < WAVE_COUNT; ++w) s_tables[w][WT_LEN] = s_tables[w][0];
}

float wavetable_sample(uint8_t w, float phase)
{
    if (w >= WAVE_COUNT) w = WAVE_SINE;
    if (phase < 0.0f) phase = 0.0f;
    else if (phase >= 1.0f) phase -= (float)(int)phase;      // страховка: [0,1)

    const float fpos = phase * (float)WT_LEN;                // [0, WT_LEN)
    const int   i    = (int)fpos;                            // [0, WT_LEN-1]
    const float frac = fpos - (float)i;
    const float *t   = s_tables[w];
    return t[i] + (t[i + 1] - t[i]) * frac;                  // i+1 ≤ WT_LEN (guard)
}
