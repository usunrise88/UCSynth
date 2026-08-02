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

#include "nvs_flash.h"

#include "audio.h"
#include "comm.h"
#include "control.h"
#include "io.h"
#include "preset_store.h"
#include "seq_store.h"

static const char *TAG = "ucsynth";

// Печать «паспорта» чипа. Главное на этапе 0 — убедиться, что Octal PSRAM поднялась:
// частая грабля N16R8 — забыть CONFIG_SPIRAM_MODE_OCT, тогда PSRAM = 0.
static void log_hw_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    // Boot-лог — основной инструмент проверки железа в этом проекте (progress.md фиксирует
    // «boot-лог показал flash 16 МБ» как критерий приёмки этапа 0.1). Молча напечатать 0 при
    // провале опроса = выдать сбой API за аппаратный дефект.
    uint32_t flash_size = 0;
    const esp_err_t flash_err = esp_flash_get_size(nullptr, &flash_size);
    if (flash_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_flash_get_size: %s — размер flash неизвестен (это не дефект платы)",
                 esp_err_to_name(flash_err));
    }

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

// Инициализация периферии, чьи задачи/ISR должны жить на Core 1 — выполняется САМА на Core 1.
//
// ESP-IDF выделяет прерывание периферии на том CPU, который вызвал esp_intr_alloc, а
// usb_serial_jtag_driver_install и i2c_new_master_bus делают это внутри себя. app_main пиннут на
// CPU0 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0), поэтому вызов отсюда посадил бы их ISR на Core 0 —
// ядро, отданное аудио, где каждый вход вытесняет audio_task посреди блока (раздувает cpu_permille
// и s_late_blocks). На этапах 8–9 к этому добавятся INT MCP23017 и SPI дисплея; поэтому держим всю
// не-аудио периферию (и её ISR) на Core 1.
//
// i2s_new_channel в audio_init, наоборот, правильно зовётся с Core 0 — так и оставлено.
static void core1_init_task(void *arg)
{
    (void)arg;
    comm_init();     // протокол Serial / USB CDC        (этап 0.3)
    io_init();       // I2C-шина + сканер, периферия     (этап 8.1+)
    vTaskDelete(nullptr);
}

static void init_core1_peripherals(void)
{
    TaskHandle_t h = nullptr;
    if (xTaskCreatePinnedToCore(core1_init_task, "init1", 4096, nullptr, 6, &h, 1) != pdPASS) {
        ESP_LOGE(TAG, "не создать задачу init1 — периферия поднимается с Core 0 (ISR осядут там же)");
        comm_init();
        io_init();
        return;
    }
    // Ждём, пока периферия поднимется: дальше идёт heartbeat, и лог должен быть последовательным.
    while (eTaskGetState(h) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "UCSynth boot — этап 0 (каркас и протокол)");
    log_hw_info();

    // Порядок: реестр параметров → аудио → периферия Core 1.
    //
    // audio_init ДО comm_init намеренно: comm_init сразу создаёт задачу, которая на первый же
    // NOTE_ON зовёт audio_note_on и читает s_note_q. Раньше очередь создавалась позже (audio_init
    // тратит ~1 с на wavetable_init и выделение реверба), так что существовало окно, в котором
    // указатель читался между ядрами без синхронизации — NULL-гард делал это безвредным на
    // практике, но это была гонка. Теперь очередь готова до появления читателя.

    control_init();  // реестр параметров            (этап 0.2)

    // Хранилище пресетов (этап 6): свой NVS-раздел "presets" (не системный nvs). Инициализируем ПОСЛЕ
    // control (пресеты читают/пишут реестр), ДО comm (первый CMD_PRESET_* уже найдёт хранилище готовым).
    // Сбой не фатален — синт работает без сохранения (пресеты просто не грузятся/не пишутся).
    esp_err_t nvs_err = nvs_flash_init_partition("presets");
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 'presets' требует erase (%s) — стираю и переинициализирую", esp_err_to_name(nvs_err));
        if (nvs_flash_erase_partition("presets") == ESP_OK) {   // сбой erase не фатален — просто без пресетов
            nvs_err = nvs_flash_init_partition("presets");
        }
    }
    if (nvs_err == ESP_OK) {
        preset_store_init();
        seq_store_init();   // паттерны секвенсора (этап 7) — свой namespace в том же разделе
    } else {
        ESP_LOGE(TAG, "NVS 'presets' init: %s — пресеты недоступны (синт работает без сохранения)",
                 esp_err_to_name(nvs_err));
    }

    audio_init();    // I2S + DMA + DSP на Core 0     (этап 1)
    init_core1_peripherals();

    ESP_LOGI(TAG, "boot complete");

    // Heartbeat: видно, что система жива и как расходуется память под нагрузкой.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive — heap %u КБ, мин. за всё время %u КБ",
                 (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
    }
}
