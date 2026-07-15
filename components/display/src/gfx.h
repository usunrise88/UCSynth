// gfx — рисование в grayscale intensity-буфер (8 бит/пиксель) с последующим Bayer-дизерингом
// в 1-бит буфер SSD1306. Даёт на монохромной панели псевдо-градации: сглаженные линии,
// послесвечение осциллографа (затухание intensity) и плавные фейды (масштаб яркости при дизере).
#pragma once

#include <cstdint>
#include "ssd1306.h"

struct GrayCanvas { uint8_t px[SSD1306_W * SSD1306_H]; };   // 8 КБ, page-independent

void g_clear(GrayCanvas &g);
void g_dim(GrayCanvas &g, uint16_t num, uint16_t den);              // px *= num/den (decay/фейд)
void g_pixel(GrayCanvas &g, int x, int y, uint8_t v);              // max с текущим значением
void g_hline(GrayCanvas &g, int x0, int x1, int y, uint8_t v);
void g_rect(GrayCanvas &g, int x, int y, int w, int h, uint8_t v);
void g_fill_rect(GrayCanvas &g, int x, int y, int w, int h, uint8_t v);
void g_line(GrayCanvas &g, int x0, int y0, int x1, int y1, uint8_t v);
void g_text(GrayCanvas &g, int x, int y, const char *s, int scale, uint8_t v);
int  g_text_width(const char *s, int scale);

// Дизеринг (Bayer 8×8) grayscale → 1-бит fb (SSD1306_FB_SIZE). fade масштабирует яркость (num/den,
// den≠0). fade=256/256 — как есть; меньше — темнее (для фейдов splash/popup).
void g_dither(const GrayCanvas &g, uint8_t *fb, uint16_t fade_num, uint16_t fade_den);
