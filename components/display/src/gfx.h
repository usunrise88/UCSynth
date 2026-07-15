// gfx — рисование в 1-битный кадровый буфер SSD1306 (page-major).
// Инкремент A: линии/прямоугольники/текст 5×7. Дизеринг и псевдо-градации (intensity-буфер,
// послесвечение, фейды) — следующий инкремент.
#pragma once

#include <cstdint>
#include "ssd1306.h"

struct Canvas {
    uint8_t fb[SSD1306_FB_SIZE];
};

void gfx_clear(Canvas &c);
void gfx_pixel(Canvas &c, int x, int y, bool on);
void gfx_hline(Canvas &c, int x0, int x1, int y, bool on);
void gfx_vline(Canvas &c, int x, int y0, int y1, bool on);
void gfx_rect(Canvas &c, int x, int y, int w, int h, bool on);
void gfx_fill_rect(Canvas &c, int x, int y, int w, int h, bool on);
void gfx_line(Canvas &c, int x0, int y0, int x1, int y1, bool on);

// Моноширинный шрифт 5×7 (шаг 6 px). scale — целочисленное увеличение (1, 2, ...).
void gfx_text(Canvas &c, int x, int y, const char *s, int scale, bool on);
int  gfx_text_width(const char *s, int scale);
