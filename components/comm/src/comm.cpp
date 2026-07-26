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
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "comm";

// Исходящий поток байт RX-задача → TX-задача.
//
// Почему через буфер, а не write прямо из обработчика: TX-кольцо драйвера USB-JTAG всего 256 байт,
// а ответ на LIST — 86 кадров ≈ 3.4 КБ. Записывая ответ inline, RX-задача парковалась в write на
// всю пачку и в это время не звала usb_serial_jtag_read_bytes — 256-байтное RX-кольцо оставалось
// без присмотра, и ISR молча терял переполнение. Дальше это выглядело как случайные потери команд
// (и как залипшая нота, если потерялся NOTE_OFF). Теперь блокируется только TX-задача, а чтение
// не прерывается никогда.
static constexpr size_t TX_STREAM_BYTES = 4096;   // с запасом на пачку LIST (3.4 КБ)
static StreamBufferHandle_t s_tx = nullptr;

// Сколько ждать места в TX-буфере, прежде чем признать ответ потерянным. Ждать бесконечно нельзя —
// это вернуло бы ровно ту проблему, от которой мы уходим. Потеря восстановима: GUI переспрашивает
// LIST (см. syncer в app/device), а STAT приходит по опросу.
static constexpr TickType_t TX_WAIT = pdMS_TO_TICKS(50);

// emit-колбэк: обернуть тело ответа в кадр и отдать TX-задаче одним куском
// (кадр целиком → на проводе не рвётся логами; при редком разрыве спасает CRC).
static void emit_usb(void *ctx, const uint8_t *body, size_t len)
{
    (void)ctx;
    uint8_t frame[FRAME_MAX_SIZE];
    const size_t n = frame_encode(body, len, frame, sizeof(frame));
    if (!n) return;
    if (!s_tx) return;
    const size_t sent = xStreamBufferSend(s_tx, frame, n, TX_WAIT);
    if (sent != n) {
        ESP_LOGW(TAG, "TX-буфер переполнен: кадр %u Б отброшен (хост не забирает)", (unsigned)n);
    }
}

// TX-задача: единственная, кто пишет в USB. Блокируется сколько нужно — на разбор входа это уже
// не влияет.
static void comm_tx_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        const size_t n = xStreamBufferReceive(s_tx, buf, sizeof(buf), portMAX_DELAY);
        for (size_t off = 0; off < n; ) {
            const int w = usb_serial_jtag_write_bytes(buf + off, n - off, portMAX_DELAY);
            if (w <= 0) {                       // драйвер отказал — дальше давить бессмысленно
                ESP_LOGW(TAG, "usb_serial_jtag_write_bytes вернул %d, %u Б потеряно",
                         w, (unsigned)(n - off));
                break;
            }
            off += (size_t)w;
        }
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

            // Ноты → моно-голос на Core 0 через очередь (этап 3.0). Диспетчер ниже ещё вернёт
            // ACK; здесь только прокидываем событие в аудио (напрямую в audio, не в DSP-цикл).
            if (blen >= 3 && body[0] == CMD_NOTE_ON) {
                audio_note_on(body[1], body[2]);
            } else if (blen >= 2 && body[0] == CMD_NOTE_OFF) {
                audio_note_off(body[1]);
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

    s_tx = xStreamBufferCreate(TX_STREAM_BYTES, 1);
    if (!s_tx) {
        ESP_LOGE(TAG, "не выделить TX-буфер %u Б — протокол не поднят", (unsigned)TX_STREAM_BYTES);
        return;
    }

    // Serial — на Core 1 (Core 0 отдан аудио). Приоритет средний: ждём на чтении.
    // TX отдельной задачей и приоритетом ниже RX: разбор входа важнее выдачи ответов.
    if (xTaskCreatePinnedToCore(comm_tx_task, "comm_tx", 3072, nullptr, 4, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "не создать задачу comm_tx — протокол не поднят");
        return;
    }
    if (xTaskCreatePinnedToCore(comm_task, "comm", 4096, nullptr, 5, nullptr, 1) != pdPASS) {
        ESP_LOGE(TAG, "не создать задачу comm — протокол не поднят");
        return;
    }
    ESP_LOGI(TAG, "протокол Serial (бинарный) на USB-JTAG, задачи RX/TX на Core 1");
}
