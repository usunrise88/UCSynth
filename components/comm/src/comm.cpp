// comm — транспорт бинарного протокола по USB-Serial-JTAG.
// Разбор/сборка кадров — в frame.*/protocol.* (чистые, без ESP-IDF). Здесь только I/O:
// драйвер USB-JTAG, задача на Core 1, склейка байтов в кадры и выдача ответов.
// Команды транслируются в set_param/get_param (control) — никогда напрямую в audio.
#include "comm.h"
#include "protocol.h"
#include "frame.h"
#include "audio.h"

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "comm";

// emit-колбэк: обернуть тело ответа в кадр и вытолкнуть в USB-JTAG одним write
// (кадр целиком → на проводе не рвётся логами; при редком разрыве спасает CRC).
static void emit_usb(void *ctx, const uint8_t *body, size_t len)
{
    (void)ctx;
    uint8_t frame[FRAME_MAX_SIZE];
    const size_t n = frame_encode(body, len, frame, sizeof(frame));
    if (n) {
        usb_serial_jtag_write_bytes(frame, n, portMAX_DELAY);
    }
}

static void comm_task(void *arg)
{
    (void)arg;
    frame_decoder_t dec;
    frame_decoder_init(&dec);

    uint8_t rx[128];
    for (;;) {
        // Блокируемся на чтении — в простое 0 CPU.
        const int got = usb_serial_jtag_read_bytes(rx, sizeof(rx), portMAX_DELAY);
        for (int i = 0; i < got; ++i) {
            const uint8_t *body;
            size_t blen;
            if (!frame_decoder_push(&dec, rx[i], &body, &blen)) {
                continue;
            }

            // Ноты пока некуда играть (синтезатор — этап 3), но лог полезен при отладке.
            if (blen >= 3 && body[0] == CMD_NOTE_ON) {
                ESP_LOGI(TAG, "NOTE_ON n=%u vel=%u (в голоса — этап 3)", body[1], body[2]);
            } else if (blen >= 2 && body[0] == CMD_NOTE_OFF) {
                ESP_LOGI(TAG, "NOTE_OFF n=%u", body[1]);
            }

            uint32_t cpu = 0, underruns = 0;
            audio_get_stats(&cpu, &underruns);
            const sys_stats_t st = {
                .heap_free    = (uint32_t)esp_get_free_heap_size(),
                .heap_min     = (uint32_t)esp_get_minimum_free_heap_size(),
                .uptime_ms    = (uint32_t)(esp_timer_get_time() / 1000),
                .cpu_permille = cpu,
                .underruns    = underruns,
            };
            comm_handle_request(body, blen, &st, emit_usb, nullptr);
        }
    }
}

void comm_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    // Перевести консольный VFS на драйвер: ESP_LOG/printf продолжают идти по тому же
    // USB-JTAG, а мы читаем/пишем протокол сырым API — один канал, без гонок за периферию.
    usb_serial_jtag_vfs_use_driver();

    // Serial — на Core 1 (Core 0 отдан аудио). Приоритет средний: ждём на чтении.
    xTaskCreatePinnedToCore(comm_task, "comm", 4096, nullptr, 5, nullptr, 1);
    ESP_LOGI(TAG, "протокол Serial (бинарный) на USB-JTAG, задача на Core 1");
}
