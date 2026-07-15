// gfx — чёткое 1-битное рисование прямо в кадровый буфер SSD1306 (page-major: байт = 8
// вертикальных пикселей, бит0 сверху). Без дизеринга/градаций: на маленькой монохромной
// панели крисп читается лучше, чем псевдо-градации (проверено на железе).
#pragma once

#include <cstdint>
#include "ssd1306.h"

void fb_clear(uint8_t *fb);
void fb_pixel(uint8_t *fb, int x, int y, bool on);
void fb_hline(uint8_t *fb, int x0, int x1, int y, bool on);
void fb_vline(uint8_t *fb, int x, int y0, int y1, bool on);
void fb_rect(uint8_t *fb, int x, int y, int w, int h, bool on);
void fb_fill_rect(uint8_t *fb, int x, int y, int w, int h, bool on);
void fb_line(uint8_t *fb, int x0, int y0, int x1, int y1, bool on);
void fb_text(uint8_t *fb, int x, int y, const char *s, int scale, bool on);
int  fb_text_width(const char *s, int scale);
