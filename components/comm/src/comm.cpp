#include "comm.h"
#include "esp_log.h"

static const char *TAG = "comm";

void comm_init(void)
{
    // Заглушка. На этапе 0.3 здесь появится разбор текстовых команд Serial
    // и их трансляция в set_param/get_param (control) — никогда напрямую в audio.
    ESP_LOGI(TAG, "init (заглушка — протокол Serial, этап 0.3)");
}
