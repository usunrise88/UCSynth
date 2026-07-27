// Host-тест форматирования значений для OLED-попапа. Отдельный тест потому, что display.cpp тянет
// драйвер I2C и на хосте не собирается — формат вынесен в numfmt.h ровно для проверяемости.
#include "numfmt.h"

#include <cstdio>
#include <cstring>

static int g_fail = 0;

static void expect(float v, const char *want)
{
    char got[16];
    fmt_float_2dp(got, sizeof(got), v);
    if (std::strcmp(got, want) != 0) {
        std::printf("FAIL: %g → \"%s\", ожидалось \"%s\"\n", (double)v, got, want);
        ++g_fail;
    }
}

int main()
{
    // Регресс: минус для −1 < v < 0. Целая часть усекается к нулю, поэтому наивный "%d" печатал
    // «0» и знак исчезал — flt_env_amt = −0.5 показывался как «0.49».
    expect(-0.5f,  "-0.50");
    expect(-0.25f, "-0.25");
    expect(-0.01f, "-0.01");
    expect(-0.99f, "-0.99");

    // Значения, которые и раньше выглядели правильно — поэтому баг читался как случайный глюк.
    expect(-1.0f,  "-1.00");
    expect(-1.5f,  "-1.50");
    expect(0.0f,   "0.00");
    expect(0.5f,   "0.50");
    expect(1.0f,   "1.00");
    expect(20000.0f, "20000.00");

    // Перенос при округлении дроби: 0.999 → «1.00», а не «0.100».
    expect(0.999f,  "1.00");
    expect(-0.999f, "-1.00");
    expect(1.996f,  "2.00");

    // Диапазоны реальных параметров: детюн ±24, cutoff, глубина матрицы ±1.
    expect(-24.0f, "-24.00");
    expect(-23.5f, "-23.50");
    expect(0.05f,  "0.05");

    if (g_fail == 0) { std::printf("OK: numfmt — все проверки пройдены\n"); return 0; }
    std::printf("ПРОВАЛ: %d проверок(и)\n", g_fail);
    return 1;
}
