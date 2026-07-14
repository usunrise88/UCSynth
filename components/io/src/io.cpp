#include "io.h"
#include "esp_log.h"

static const char *TAG = "io";

void io_init(void)
{
    // Заглушка. Периферия подключается поздно (этапы 8–10), когда звук отлажен через GUI:
    // I2C-шина + сканер, MCP23017, энкодеры через INT, матрица 4x4, ST7796, тач, VL53L0X.
    ESP_LOGI(TAG, "init (заглушка — периферия, этапы 8–10)");
}
