// fx — реализация эффектов. Начинаем с overdrive (5.1); delay/reverb добавятся в 5.2/5.3.
#include "fx.h"

#include <cmath>

float fx_overdrive(float x, const FxParams *p)
{
    if (!p->od_on || p->od_mix <= 0.0f) return x;   // выкл / полностью dry → байпас
    const float gain = 1.0f + p->od_drive * 11.0f;  // drive 0..1 → гейн 1..12× в шейпер
    const float wet  = tanhf(x * gain);             // мягкое насыщение, всегда [-1,1]
    return x + (wet - x) * p->od_mix;               // wet/dry (mix=1 → чистый wet, ограничен)
}
