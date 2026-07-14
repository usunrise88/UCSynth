#include "control.h"
#include "esp_log.h"

static const char *TAG = "control";

void control_init(void)
{
    // Заглушка. На этапе 0.2 здесь появится реестр параметров
    // (id -> {имя, тип, диапазон, значение, дефолт}) и thread-safe set/get_param.
    ESP_LOGI(TAG, "init (заглушка — модель параметров, этап 0.2)");
}
