// lfo — реализация форм. Фаза [0,1) продвигается на rate·dt за блок; на заворот фазы
// S&H тянет новую случайную выборку (xorshift32). Всё в float — FPU S3 одинарной точности.
#include "lfo.h"

#include <cmath>

// xorshift32 → [-1,1). Дёшево и для S&H достаточно «случайно».
static inline float sh_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)x * 4.6566129e-10f - 1.0f; // x/2^31 - 1
}

void lfo_reset(Lfo *l, uint32_t seed) {
    l->phase = 0.0f;
    l->rng   = seed ? seed : 0x1234567u; // xorshift не терпит нулевого посева
    l->sh    = sh_next(&l->rng);
}

float lfo_tick(Lfo *l, float rate_hz, float dt, uint8_t shape) {
    bool wrapped = false;
    l->phase += rate_hz * dt;
    while (l->phase >= 1.0f) {
        l->phase -= 1.0f;
        wrapped = true;
    }
    if (l->phase < 0.0f) l->phase = 0.0f; // защита от отрицательного rate/dt

    const float ph = l->phase;
    switch (shape) {
        case LFO_TRI:
            return 1.0f - 4.0f * fabsf(ph - 0.5f); // /\ пик +1 в центре (ph 0.5), -1 на краях (ph 0/1)
        case LFO_SAW:
            return 2.0f * ph - 1.0f; // растущая пила -1→+1
        case LFO_SQUARE:
            return ph < 0.5f ? 1.0f : -1.0f;
        case LFO_SH:
            if (wrapped) l->sh = sh_next(&l->rng); // держим значение до конца цикла
            return l->sh;
        case LFO_SINE:
        default:
            return sinf(2.0f * 3.14159265358979f * ph);
    }
}
