// Точка входа UCSynth. Этап 0 (каркас): проект собирается, «Hello» в serial,
// PSRAM определяется. DSP/периферия — заглушки, наполняются на следующих этапах.
//
// Архитектура (см. CLAUDE.md): источники управления пишут ТОЛЬКО через control
// (модель параметров), никогда напрямую в audio. Здесь только инициализация слоёв.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "audio.h"
#include "comm.h"
#include "control.h"
#include "display.h"
#include "io.h"

static const char *TAG = "ucsynth";

// Печать «паспорта» чипа. Главное на этапе 0 — убедиться, что Octal PSRAM поднялась:
// частая грабля N16R8 — забыть CONFIG_SPIRAM_MODE_OCT, тогда PSRAM = 0.
static void log_hw_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(nullptr, &flash_size);

    ESP_LOGI(TAG, "chip ESP32-S3 rev %d, %d ядр(а), flash %lu МБ",
             chip.revision, chip.cores,
             (unsigned long)(flash_size / (1024 * 1024)));

#if CONFIG_SPIRAM
    size_t psram = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM %u МБ (свободно %u КБ)",
             (unsigned)(psram / (1024 * 1024)),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (psram < 4 * 1024 * 1024) {
        ESP_LOGW(TAG, "PSRAM меньше 4 МБ — проверь Octal-режим (CONFIG_SPIRAM_MODE_OCT)");
    }
#else
    ESP_LOGE(TAG, "PSRAM отключена в sdkconfig — delay/reverb (этап 5) работать не будут");
#endif

    ESP_LOGI(TAG, "heap: свободно %u КБ (internal)",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "UCSynth boot — этап 0 (каркас и протокол)");
    log_hw_info();

    // Инициализация слоёв. Порядок: сначала модель параметров и связь (ими пользуются
    // остальные), потом периферия, аудио — последним (оно начнёт читать параметры).
    control_init();  // реестр параметров            (этап 0.2)
    comm_init();     // протокол Serial / USB CDC     (этап 0.3)
    io_init();       // периферия: I2C, энкодеры, тач (этап 8+)
    audio_init();    // I2S + DMA + DSP на Core 0     (этап 1)
    display_init();  // отладочный OLED SSD1306 на Core 1 (вне спеки, до ST7796)

    ESP_LOGI(TAG, "boot complete");

    // Heartbeat: видно, что система жива и как расходуется память под нагрузкой.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive — heap %u КБ, мин. за всё время %u КБ",
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    }
}
