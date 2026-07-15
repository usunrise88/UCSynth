// ssd1306 — минимальный драйвер OLED SSD1306 128×64 по I2C (свой, без внешних компонентов).
// Кадровый буфер page-major: 1024 байта, байт = 8 вертикальных пикселей (одна страница),
// бит 0 — верхний пиксель. Это «родной» формат GDDRAM SSD1306 — flush пишет его как есть.
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

static constexpr int SSD1306_W       = 128;
static constexpr int SSD1306_H       = 64;
static constexpr int SSD1306_FB_SIZE = SSD1306_W * SSD1306_H / 8;   // 1024

// Создать device на готовой I2C-шине и проинициализировать панель. false при отсутствии ACK
// (не тот адрес / не подключено / SH1106 — тогда и картинки не будет корректной).
bool ssd1306_init(i2c_master_bus_handle_t bus, uint8_t addr);

// Вытолкнуть весь кадровый буфер (SSD1306_FB_SIZE байт) на экран одним I2C-пакетом.
void ssd1306_flush(const uint8_t *fb);
