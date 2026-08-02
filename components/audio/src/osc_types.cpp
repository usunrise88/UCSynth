#include "osc_types.h"
#include <cmath>

namespace {
// PolyBLEP: 2-точечная коррекция ступеньки (разрыва значения) шириной dt вокруг границы фазы.
// Известный алгоритм (Välimäki/«Antialiasing Oscillators in Subtractive Synthesis») — не изобретаем.
inline float polyblep(float t, float dt)
{
    if (t < dt) {                    // сразу после разрыва (t→0)
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {             // прямо перед разрывом (t→1)
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
}  // namespace

float va_sample(uint8_t wave, float phase, float dt)
{
    switch (wave) {
    case 1: {  // WAVE_SAW: наивная пила 2·ph−1, минус blep на разрыве 1→0
        return (2.0f * phase - 1.0f) - polyblep(phase, dt);
    }
    case 2: {  // WAVE_SQUARE: ±1 (скваж. 50%), blep на разрывах в 0 и 0.5
        float y = phase < 0.5f ? 1.0f : -1.0f;
        y += polyblep(phase, dt);
        float t2 = phase + 0.5f;
        if (t2 >= 1.0f) t2 -= 1.0f;
        y -= polyblep(t2, dt);
        return y;
    }
    case 3: {  // WAVE_TRI: наивный треугольник (значение непрерывно; алиасинг слаб, 1/k²)
        return phase < 0.5f ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    }
    default:   // WAVE_SINE: разрывов нет — прямой синус (sinf, не sin — FPU S3)
        return sinf(6.2831853f * phase);
    }
}

float pd_warp(float phase, float amount)
{
    if (amount < 0.0f) amount = 0.0f; else if (amount > 1.0f) amount = 1.0f;
    // Casio-CZ: «колено» d двигается 0.5 → ~0.01 с ростом amount. Фаза сжимается в [0,0.5] на [0,d],
    // растягивается в [0.5,1] на [d,1] — синус искривляется к пилообразному (ярче). amount=0 → d=0.5
    // → тождество.
    const float d = 0.5f * (1.0f - amount * 0.98f);
    if (phase < d) return 0.5f * phase / d;
    return 0.5f + 0.5f * (phase - d) / (1.0f - d);
}
