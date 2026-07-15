// display — задача рендера отладочного OLED на Core 1. Инкремент A: splash → осциллограф
// (форма волны из audio scope) + строка статуса + горизонтальный level bar.
// Дизеринг/послесвечение/фейды и popup параметра — следующий инкремент.
#include "display.h"
#include "ssd1306.h"
#include "gfx.h"
#include "control.h"
#include "audio.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cstdint>
#include <cstdio>

static const char *TAG = "display";

static constexpr gpio_num_t PIN_SDA   = GPIO_NUM_8;
static constexpr gpio_num_t PIN_SCL   = GPIO_NUM_9;
static constexpr uint8_t    OLED_ADDR = 0x3C;

static void draw_splash(Canvas &c)
{
    gfx_clear(c);
    gfx_rect(c, 0, 0, SSD1306_W, SSD1306_H, true);
    const char *title = "UCSynth";
    gfx_text(c, (SSD1306_W - gfx_text_width(title, 2)) / 2, 16, title, 2, true);
    const char *sub = "wavetable synth";
    gfx_text(c, (SSD1306_W - gfx_text_width(sub, 1)) / 2, 40, sub, 1, true);
}

static void draw_scope(Canvas &c)
{
    gfx_clear(c);

    // Статус сверху: частота + форма (пока только синус).
    static const char *kWave[] = {"sine", "saw", "square", "tri"};
    char line[24];
    const float freq = get_param(PARAM_TEST_TONE_HZ);
    const int   wf   = (int)get_param(PARAM_WAVEFORM);
    snprintf(line, sizeof(line), "%d Hz  %s", (int)(freq + 0.5f), kWave[(wf < 0 || wf > 3) ? 0 : wf]);
    gfx_text(c, 2, 0, line, 1, true);

    // Осциллограф: центральная зона, ось по центру (пунктир). scope 128 семплов = 128 px.
    int8_t scope[AUDIO_SCOPE_LEN];
    audio_scope_read(scope);
    const int top = 12, bot = 45, mid = (top + bot) / 2, amp = (bot - top) / 2;
    for (int x = 0; x < SSD1306_W; x += 4) gfx_pixel(c, x, mid, true);
    int prev_y = mid - (scope[0] * amp) / 127;
    for (int x = 1; x < AUDIO_SCOPE_LEN; ++x) {
        const int y = mid - (scope[x] * amp) / 127;
        gfx_line(c, x - 1, prev_y, x, y, true);
        prev_y = y;
    }

    // Метрики (этап 1.2): загрузка аудио-задачи + late-блоки.
    uint32_t cpu = 0, ur = 0;
    audio_get_stats(&cpu, &ur);
    char m[24];
    snprintf(m, sizeof(m), "cpu %u.%u%%  ur %u",
             (unsigned)(cpu / 10), (unsigned)(cpu % 10), (unsigned)ur);
    gfx_text(c, 2, 46, m, 1, true);

    // Level bar снизу: master_volume 0..1.
    const float vol = get_param(PARAM_MASTER_VOLUME);
    const int bw = (int)(vol * 122.0f + 0.5f);
    gfx_rect(c, 2, 54, 124, 9, true);
    if (bw > 0) gfx_fill_rect(c, 3, 55, bw, 7, true);
}

static void display_task(void *arg)
{
    (void)arg;
    static Canvas c;                        // 1 КБ в BSS, не на стеке
    const int64_t t0 = esp_timer_get_time();

    for (;;) {
        if (esp_timer_get_time() - t0 < 2000000) draw_splash(c);   // splash ~2 с
        else draw_scope(c);
        ssd1306_flush(c.fb);
        vTaskDelay(pdMS_TO_TICKS(33));      // ~30 fps
    }
}

void display_init(void)
{
    // Общая I2C-шина (позже сюда же VL53L0X / MCP). Пока владелец — display; вынос в общую
    // шину (io) при появлении второго I2C-девайса — tech-debt.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port           = I2C_NUM_0;
    bus_cfg.sda_io_num         = PIN_SDA;
    bus_cfg.scl_io_num         = PIN_SCL;
    bus_cfg.clk_source         = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt  = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus не создан (SDA=%d SCL=%d)", (int)PIN_SDA, (int)PIN_SCL);
        return;
    }
    if (!ssd1306_init(bus, OLED_ADDR)) {
        ESP_LOGW(TAG, "OLED не найден на 0x%02X — дисплей выкл (звук работает)", OLED_ADDR);
        return;   // дисплей опционален: не валим систему
    }

    // UI-задача на Core 1 (рядом с comm). Приоритет ниже comm(5): дисплей — не realtime.
    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 3, nullptr, 1);
    ESP_LOGI(TAG, "OLED SSD1306 128x64 @ 0x%02X, SDA=%d SCL=%d, задача на Core 1",
             OLED_ADDR, (int)PIN_SDA, (int)PIN_SCL);
}
