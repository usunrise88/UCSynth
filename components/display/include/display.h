// display — отладочный OLED SSD1306 (128×64, I2C 0x3C) на Core 1.
// Временный экран до цветного ST7796 + тач (этап 9): splash, осциллограф, popup параметров.
// Только ЧИТАЕТ состояние синта (get_param, audio scope) — источником управления не является.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);

#ifdef __cplusplus
}
#endif
