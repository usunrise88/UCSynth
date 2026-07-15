#include "ssd1306.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "ssd1306";
static i2c_master_dev_handle_t s_dev = nullptr;

bool ssd1306_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = addr;
    dev_cfg.scl_speed_hz    = 400000;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add_device 0x%02X не удалось", addr);
        return false;
    }

    // Стандартная init-последовательность SSD1306 128×64. Первый байт 0x00 = control «поток
    // команд» (все последующие байты трактуются как команды/аргументы).
    static const uint8_t init_seq[] = {
        0x00,             // control: команды
        0xAE,             // display off
        0xD5, 0x80,       // clock divide ratio / oscillator
        0xA8, 0x3F,       // multiplex ratio = 63 (64 строки)
        0xD3, 0x00,       // display offset 0
        0x40,             // start line 0
        0x8D, 0x14,       // charge pump ON (без него экран тёмный)
        0x20, 0x00,       // memory addressing mode: horizontal
        0xA1,             // segment remap (столбец 127 → SEG0)
        0xC8,             // COM scan direction: remapped (иначе картинка вверх ногами)
        0xDA, 0x12,       // COM pins hardware config
        0x81, 0xCF,       // contrast
        0xD9, 0xF1,       // pre-charge period
        0xDB, 0x40,       // VCOMH deselect level
        0xA4,             // resume: выводить содержимое RAM
        0xA6,             // normal display (не инверсный)
        0x2E,             // scroll off
        0xAF,             // display ON
    };
    if (i2c_master_transmit(s_dev, init_seq, sizeof(init_seq), 100) != ESP_OK) {
        ESP_LOGE(TAG, "нет ACK от 0x%02X — проверь адрес/подключение (или это SH1106)", addr);
        return false;
    }
    ESP_LOGI(TAG, "SSD1306 128x64 init OK (0x%02X)", addr);
    return true;
}

void ssd1306_flush(const uint8_t *fb)
{
    if (!s_dev) return;
    // Адресуемся на весь экран: колонки 0..127, страницы 0..7.
    const uint8_t win[] = {0x00, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
    i2c_master_transmit(s_dev, win, sizeof(win), 100);
    // Данные одним пакетом: [0x40 = поток данных][1024 байта GDDRAM].
    static uint8_t tx[1 + SSD1306_FB_SIZE];
    tx[0] = 0x40;
    memcpy(tx + 1, fb, SSD1306_FB_SIZE);
    i2c_master_transmit(s_dev, tx, sizeof(tx), 100);
}
