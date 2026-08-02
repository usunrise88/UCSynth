#include "io.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <cstdio>

static const char *TAG = "io";

// Общая I2C-шина проекта. Владелец — io (раньше её создавал display; долг T-004 закрыт вместе с
// удалением отладочного OLED). Сюда садятся MCP23017 ×3 (этап 8) и VL53L0X (этап 10). Пины — те же,
// что раньше вёл OLED, чтобы не трогать разводку бредборда.
static constexpr gpio_num_t PIN_SDA = GPIO_NUM_8;
static constexpr gpio_num_t PIN_SCL = GPIO_NUM_9;

static i2c_master_bus_handle_t s_bus = nullptr;

// Boot-сканер (этап 8.1). Без него «не вижу MCP» неотличимо от «не тот адрес / RESET висит»:
// пройтись по 7-битному диапазону и залогировать, кто ACK-ает. Пробинг не создаёт device — не
// требует адреса/скорости; ACK ловится дёшево.
static void i2c_scan(void)
{
    char line[128];
    int  n     = snprintf(line, sizeof(line), "I2C scan:");
    int  found = 0;
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ++found;
            if (n < (int)sizeof(line) - 8)   // защита от переполнения строки при куче устройств
                n += snprintf(line + n, sizeof(line) - n, " 0x%02X", addr);
        }
    }
    if (found == 0)
        snprintf(line + n, sizeof(line) - n, " пусто (проверь SDA/SCL, питание и RESET→VCC на MCP)");
    ESP_LOGI(TAG, "%s  [%d устр.]", line, found);
}

void io_init(void)
{
    // Периферия подключается поздно (этапы 8–10), когда звук отлажен через GUI. Сейчас — общая
    // I2C-шина + сканер (8.1). Энкодеры/матрица через INT и сенсоры добавятся сюда же.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = PIN_SDA;
    bus_cfg.scl_io_num        = PIN_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    // Слабая (~45 кОм) внутренняя подтяжка — только страховка. Рабочие подтяжки шины — внешние,
    // на платах MCP23017 (см. docs/hardware.md: оставить один комплект, лишние снять).
    bus_cfg.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus не создан (SDA=%d SCL=%d) — периферия недоступна",
                 (int)PIN_SDA, (int)PIN_SCL);
        s_bus = nullptr;
        return;
    }
    ESP_LOGI(TAG, "I2C bus поднят: SDA=%d SCL=%d", (int)PIN_SDA, (int)PIN_SCL);
    i2c_scan();
}
