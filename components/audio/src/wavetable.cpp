// wavetable — band-limited осцилляторы через октавные mip-таблицы (закрывает D-008).
// На форму — набор таблиц, по одной на октавный диапазон частот. Таблица содержит гармоники
// только до Найквиста ВЕРХА своего диапазона (худший случай) → нота из диапазона играет без
// алиасинга. Таблицы наполняются аддитивным синтезом (ряд Фурье, готовый метод) на старте.
//
// Стоимость: таблицы сжимаются с октавой (2048→…→8), пул ~64 КБ во внутреннем DRAM.
// Генерация быстрая: sin(kθ) считаем рекуррентой Чебышёва (2·cosθ·s−s'), а не K вызовами sinf.
// Чистый DSP без ESP-IDF → host-тестируем (test/host/test_wavetable.cpp).
#include "wavetable.h"
#include "dsp_hot.h"
#include <cmath>

namespace {

constexpr int   MIP_COUNT = 11;      // октавные диапазоны от F0 вверх: F0·2^0 … F0·2^10 (~20 кГц)
constexpr int   L0        = 2048;    // длина таблицы низшего mip (запас гармоник для баса)
constexpr int   MIN_LEN   = 8;       // выше по частоте гармоник единицы — мельче таблицы не нужны
constexpr float F0        = 20.0f;   // низ mip 0; mip m покрывает [F0·2^m, F0·2^{m+1})

constexpr float TWO_PI = 6.28318530717958648f;

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

// Амплитуда k-й гармоники формы (без sin). Считается ВНЕ горячего цикла (предрасчёт на mip),
// поэтому деление тут не жаль. Пила — все гармоники (1/k, знак чередуется), меандр/треугольник —
// нечётные (1/k и 1/k²); синус — только 1-я гармоника.
float coeff(uint8_t w, int k)
{
    switch (w) {
        case WAVE_SAW:    return (k & 1 ? 1.0f : -1.0f) / (float)k;            // ∑ (-1)^{k+1}/k
        case WAVE_SQUARE: return (k & 1) ? 1.0f / (float)k : 0.0f;            // нечётные, 1/k
        case WAVE_TRI:    return (k & 1) ? (((k - 1) / 2) & 1 ? -1.0f : 1.0f) // нечётные, 1/k²,
                                           / (float)(k * k) : 0.0f;           //   знак чередуется
        case WAVE_SINE:
        default:          return (k == 1) ? 1.0f : 0.0f;                      // чистая 1-я
    }
}

}  // namespace

void wavetable_init(float sample_rate)
{
    // ВСЁ считаем в float. У ESP32-S3 FPU только одинарной точности — double идёт через софт-
    // эмуляцию (__muldf3/__adddf3/__divdf3), из-за чего первая версия генерации отъедала ~15 с на
    // старте и валила task-WDT (IDLE0 голодал). float → аппаратный FPU, генерация < 1 с.
    static float coef[L0 / 2 + 1];              // скретч коэффициентов гармоник (в .bss, не на стеке)
    const float nyquist = 0.5f * sample_rate;

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
            const float f_hi = F0 * (float)(1 << (m + 1));
            int K = (int)(nyquist / f_hi);
            if (K > L / 2 - 1) K = L / 2 - 1;
            if (K < 1)         K = 1;

            // Предрасчёт коэффициентов + верхняя ненулевая гармоника (для синуса цикл короткий,
            // для меандра/треугольника пропускаем хвост нулей за старшей нечётной).
            int kmax = 1;
            for (int k = 1; k <= K; ++k) {
                coef[k] = coeff((uint8_t)w, k);
                if (coef[k] != 0.0f) kmax = k;
            }

            float maxabs = 0.0f;
            for (int i = 0; i < L; ++i) {
                const float theta = TWO_PI * (float)i / (float)L;
                const float c2    = 2.0f * cosf(theta);   // sin(kθ) — рекуррентой Чебышёва:
                float s_prev = 0.0f;                       //   sin((k+1)θ) = 2cosθ·sin(kθ) − sin((k−1)θ)
                float s_cur  = sinf(theta);
                float acc    = 0.0f;
                for (int k = 1; k <= kmax; ++k) {
                    acc += coef[k] * s_cur;                // коэф. предрасчитан → в цикле только mul/add
                    const float s_next = c2 * s_cur - s_prev;
                    s_prev = s_cur;
                    s_cur  = s_next;
                }
                t[i] = acc;
                const float a = acc < 0.0f ? -acc : acc;
                if (a > maxabs) maxabs = a;
            }

            // Нормировка на пик = 1 (гарантирует [-1,1] и после интерполяции — она не выходит
            // за концы отрезка). Слегка разный пик по mip из-за Гиббса не критичен.
            const float norm = (maxabs > 1e-9f) ? (1.0f / maxabs) : 1.0f;
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

float AUDIO_HOT wavetable_sample(uint8_t w, float phase, int mip)
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

float AUDIO_HOT wavetable_sample_morph(float pos, float phase, int mip)
{
    if (pos < 0.0f)                        pos = 0.0f;
    else if (pos > (float)(WAVE_COUNT - 1)) pos = (float)(WAVE_COUNT - 1);
    const int   w0   = (int)pos;                                 // нижняя форма [0, WAVE_COUNT-1]
    const float frac = pos - (float)w0;
    if (frac <= 0.0f) return wavetable_sample((uint8_t)w0, phase, mip);   // целая позиция — без морфа
    const int   w1 = (w0 + 1 < WAVE_COUNT) ? w0 + 1 : w0;        // верхний сосед (страховка от выхода)
    const float a  = wavetable_sample((uint8_t)w0, phase, mip);  // общая длина на mip → фаза совпадает
    const float b  = wavetable_sample((uint8_t)w1, phase, mip);
    return a + (b - a) * frac;                                   // кроссфейд, остаётся в [-1,1]
}
