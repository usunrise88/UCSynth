// filter — ZDF/TPT state-variable фильтр (Cytomic/Zavalishin, готовый алгоритм).
// LP/HP/BP + сквозной OFF, cutoff + resonance, устойчив при модуляции cutoff. Коэффициенты —
// раз в блок (реципрок вынесен из семплового цикла: у LX7 нет быстрого fdiv). Чистый DSP (float).
#pragma once

#include <cstdint>

enum FiltMode : uint8_t { FILT_LP = 0, FILT_HP, FILT_BP, FILT_OFF };

// Коэффициенты, рассчитанные раз в блок.
struct FiltCoef {
    float   g, k, a1, a2;
    uint8_t mode;
};

// Состояние интеграторов (персистентно между семплами/нотами).
struct Filter {
    float ic1eq, ic2eq;
};

void filter_reset(Filter *f);

// Рассчитать коэффициенты: cutoff (Гц, клампится в [20, 0.45·sr]), resonance [0,1], sr, режим.
FiltCoef filter_coef(float cutoff_hz, float resonance, float sr, uint8_t mode);

// Обработать один семпл.
float filter_process(Filter *f, float in, const FiltCoef *c);
