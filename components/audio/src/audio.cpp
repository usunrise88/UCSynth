#include "audio.h"
#include "esp_log.h"

static const char *TAG = "audio";

void audio_init(void)
{
    // Заглушка. На этапе 1 здесь поднимется I2S+DMA (PCM5102) и аудио-задача на Core 0
    // с блочной обработкой; далее wavetable-осцилляторы, фильтр, эффекты (этапы 1–5).
    ESP_LOGI(TAG, "init (заглушка — I2S/DSP, этап 1)");
}
