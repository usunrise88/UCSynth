// wavetable — band-limited осцилляторы через октавные mip-таблицы (закрывает D-008).
// На форму — набор таблиц, по одной на октавный диапазон частот. Таблица содержит гармоники
// только до Найквиста ВЕРХА своего диапазона (худший случай) → нота из диапазона играет без
// алиасинга. Таблицы наполняются аддитивным синтезом (ряд Фурье, готовый метод) на старте.
//
// Стоимость: таблицы сжимаются с октавой (2048→…→8), пул ~64 КБ во внутреннем DRAM.
// Генерация быстрая: sin(kθ) считаем рекуррентой Чебышёва (2·cosθ·s−s'), а не K вызовами sinf.
// Чистый DSP без ESP-IDF → host-тестируем (test/host/test_wavetable.cpp).
#include "wavetable.h"
#include <cmath>

namespace {

constexpr int   MIP_COUNT = 11;      // октавные диапазоны от F0 вверх: F0·2^0 … F0·2^10 (~20 кГц)
constexpr int   L0        = 2048;    // длина таблицы низшего mip (запас гармоник для баса)
constexpr int   MIN_LEN   = 8;       // выше по частоте гармоник единицы — мельче таблицы не нужны
constexpr float F0        = 20.0f;   // низ mip 0; mip m покрывает [F0·2^m, F0·2^{m+1})

constexpr double kTwoPi = 6.283185307179586476925287;

// Длина таблицы mip m: делим L0 пополам на октаву, но не мельче MIN_LEN. constexpr → размер пула
// считается на компиляции (точно, без магических констант).
constexpr int mip_len(int m)
{
    const int l = L0 >> m;
    return l < MIN_LEN ? MIN_LEN : l;
}
constexpr int pool_per_wave()
{
    int s = 0;
    for (int m = 0; m < MIP_COUNT; ++m) s += mip_len(m) + 1;   // +1 — guard-семпл на стык цикла
    return s;
}
constexpr int POOL_FLOATS = pool_per_wave() * WAVE_COUNT;

float  s_pool[POOL_FLOATS];              // все таблицы подряд (внутренний DRAM, ~64 КБ)
float *s_tab[WAVE_COUNT][MIP_COUNT];     // начало таблицы [форма][mip]
int    s_len[MIP_COUNT];                 // длина таблицы mip (без guard)

// Амплитуда k-й гармоники формы (без sin). Пилу строим из всех гармоник (1/k, знак чередуется),
// меандр/треугольник — из нечётных (1/k и 1/k² соответственно); синус — только 1-я гармоника.
double coeff(uint8_t w, int k)
{
    switch (w) {
        case WAVE_SAW:    return (k & 1 ? 1.0 : -1.0) / (double)k;             // ∑ (-1)^{k+1}/k
        case WAVE_SQUARE: return (k & 1) ? 1.0 / (double)k : 0.0;             // нечётные, 1/k
        case WAVE_TRI:    return (k & 1) ? (((k - 1) / 2) & 1 ? -1.0 : 1.0)   // нечётные, 1/k²,
                                           / (double)(k * k) : 0.0;           //   знак чередуется
        case WAVE_SINE:
        default:          return (k == 1) ? 1.0 : 0.0;                        // чистая 1-я
    }
}

}  // namespace

void wavetable_init(float sample_rate)
{
    const double nyquist = 0.5 * (double)sample_rate;

    for (int m = 0; m < MIP_COUNT; ++m) s_len[m] = mip_len(m);

    int off = 0;
    for (int w = 0; w < WAVE_COUNT; ++w) {
        for (int m = 0; m < MIP_COUNT; ++m) {
            const int L = s_len[m];
            float *t = &s_pool[off];
            s_tab[w][m] = t;
            off += L + 1;

            // Гармоник — до Найквиста ВЕРХА диапазона mip (худшая нота диапазона не алиасит),
            // и не выше Найквиста самой таблицы (L/2).
            const double f_hi = (double)F0 * (double)(1 << (m + 1));
            int K = (int)(nyquist / f_hi);
            if (K > L / 2 - 1) K = L / 2 - 1;
            if (K < 1)         K = 1;

            double maxabs = 0.0;
            for (int i = 0; i < L; ++i) {
                const double theta = kTwoPi * (double)i / (double)L;
                const double c2    = 2.0 * cos(theta);
                double s_prev = 0.0;             // sin(0·θ)
                double s_cur  = sin(theta);      // sin(1·θ)
                double acc    = 0.0;
                for (int k = 1; k <= K; ++k) {
                    acc += coeff((uint8_t)w, k) * s_cur;         // s_cur = sin(kθ)
                    const double s_next = c2 * s_cur - s_prev;   // рекуррента Чебышёва
                    s_prev = s_cur;
                    s_cur  = s_next;
                }
                t[i] = (float)acc;
                const double a = acc < 0 ? -acc : acc;
                if (a > maxabs) maxabs = a;
            }

            // Нормировка на пик = 1 (гарантирует [-1,1] и после интерполяции — она не выходит
            // за концы отрезка). Слегка разный пик по mip из-за Гиббса не критичен.
            const float norm = (maxabs > 1e-9) ? (float)(1.0 / maxabs) : 1.0f;
            for (int i = 0; i < L; ++i) t[i] *= norm;
            t[L] = t[0];                                          // guard = первый семпл
        }
    }
}

int wavetable_mip(float freq_hz)
{
    if (freq_hz <= F0) return 0;
    int m = (int)log2f(freq_hz / F0);        // октав над F0, floor (аргумент > 1 → результат ≥ 0)
    if (m < 0)              m = 0;
    if (m >= MIP_COUNT)     m = MIP_COUNT - 1;
    return m;
}

float wavetable_sample(uint8_t w, float phase, int mip)
{
    if (w >= WAVE_COUNT) w = WAVE_SINE;
    if (mip < 0)              mip = 0;
    else if (mip >= MIP_COUNT) mip = MIP_COUNT - 1;
    if (phase < 0.0f)        phase = 0.0f;
    else if (phase >= 1.0f)  phase -= (float)(int)phase;         // страховка: [0,1)

    const int    L    = s_len[mip];
    const float *t    = s_tab[w][mip];
    const float  fpos = phase * (float)L;                        // [0, L)
    const int    i    = (int)fpos;                               // [0, L-1]
    const float  frac = fpos - (float)i;
    return t[i] + (t[i + 1] - t[i]) * frac;                      // t[L] — guard-семпл
}
