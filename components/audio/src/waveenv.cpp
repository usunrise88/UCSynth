// waveenv — реализация. Фаза [0,1) продвигается на dt/rate за блок (полный проход всех точек = rate сек).
// loop: 8 сегментов p[i]→p[(i+1)&7], фаза заворачивается — непрерывный цикл. Без loop: 7 сегментов
// p0→p7 за rate сек, затем hold на p7. Всё float (FPU S3 одинарной точности).
#include "waveenv.h"

namespace {
constexpr float MIN_RATE = 0.001f;   // защита от dt/rate → +inf

inline float lerp(float a, float b, float f) { return a + (b - a) * f; }
}  // namespace

void waveenv_reset(WaveEnv *w)
{
    w->phase = 0.0f;
}

float waveenv_tick(WaveEnv *w, const WaveEnvParams *p, float dt)
{
    float rate = p->rate;
    if (rate < MIN_RATE) rate = MIN_RATE;
    w->phase += dt / rate;

    if (p->loop) {
        while (w->phase >= 1.0f) w->phase -= 1.0f;
        if (w->phase < 0.0f) w->phase = 0.0f;                 // защита от отрицательного dt
        const float x = w->phase * (float)WAVEENV_POINTS;     // 8 сегментов (p7→p0 замыкает цикл)
        int i = (int)x;
        if (i > WAVEENV_POINTS - 1) i = WAVEENV_POINTS - 1;
        const int   nxt = (i + 1) & (WAVEENV_POINTS - 1);     // wrap 7→0
        const float f   = x - (float)i;
        return lerp(p->pts[i], p->pts[nxt], f);
    }

    // one-shot: дошли до конца — держим последнюю точку
    if (w->phase >= 1.0f) { w->phase = 1.0f; return p->pts[WAVEENV_POINTS - 1]; }
    if (w->phase < 0.0f)  w->phase = 0.0f;
    const float x = w->phase * (float)(WAVEENV_POINTS - 1);   // 7 сегментов p0→p7
    int i = (int)x;
    if (i > WAVEENV_POINTS - 2) i = WAVEENV_POINTS - 2;
    const float f = x - (float)i;
    return lerp(p->pts[i], p->pts[i + 1], f);
}
