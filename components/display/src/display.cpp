// display — рендер отладочного OLED на Core 1. Чёткий 1-бит (после фидбэка с железа: дизеринг/
// послесвечение давали кашу). Splash → визуализатор: осциллограф (крисп линия) + статус +
// метрики + level bar. Поверх — param-popup при смене параметра (детекция на стороне дисплея,
// сравнение снимков — control/lock-free не трогаем).
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

static constexpr int64_t SPLASH_US = 2000000;   // показ splash
static constexpr int64_t POPUP_US  = 1000000;   // показ значения параметра (1 с)

static const char *kWave[] = {"sine", "saw", "square", "tri"};

static void draw_splash(uint8_t *fb)
{
    fb_clear(fb);
    fb_rect(fb, 4, 4, SSD1306_W - 8, SSD1306_H - 8, true);
    fb_text(fb, (SSD1306_W - fb_text_width("UCSynth", 2)) / 2, 14, "UCSynth", 2, true);
    fb_text(fb, (SSD1306_W - fb_text_width("unnecessary", 1)) / 2, 38, "unnecessary", 1, true);
    fb_text(fb, (SSD1306_W - fb_text_width("complicated", 1)) / 2, 48, "complicated", 1, true);
}

static void draw_scope(uint8_t *fb)
{
    fb_clear(fb);

    char line[24];
    const float freq = get_param(PARAM_TEST_TONE_HZ);
    const int   wf   = (int)get_param(PARAM_WAVEFORM);
    snprintf(line, sizeof(line), "%d Hz  %s", (int)(freq + 0.5f), kWave[(wf < 0 || wf > 3) ? 0 : wf]);
    fb_text(fb, 2, 0, line, 1, true);

    uint32_t cpu = 0, ur = 0;
    audio_get_stats(&cpu, &ur);
    char m[24];
    snprintf(m, sizeof(m), "cpu %u.%u%%  ur %u",
             (unsigned)(cpu / 10), (unsigned)(cpu % 10), (unsigned)ur);
    fb_text(fb, 2, 46, m, 1, true);

    // Осциллограф: крисп линия. Без осевой (сливалась со следом) и без послесвечения.
    const int top = 10, bot = 44, mid = (top + bot) / 2, amp = (bot - top) / 2;
    int8_t scope[AUDIO_SCOPE_LEN];
    audio_scope_read(scope);
    int prev_y = mid - (scope[0] * amp) / 127;
    for (int x = 1; x < AUDIO_SCOPE_LEN; ++x) {
        const int y = mid - (scope[x] * amp) / 127;
        fb_line(fb, x - 1, prev_y, x, y, true);
        prev_y = y;
    }

    // Level bar снизу: master_volume.
    const float vol = get_param(PARAM_MASTER_VOLUME);
    const int bw = (int)(vol * 120.0f + 0.5f);
    fb_rect(fb, 2, 54, 124, 9, true);
    if (bw > 0) fb_fill_rect(fb, 4, 56, bw, 5, true);
}

static void draw_popup(uint8_t *fb, uint16_t pid)
{
    param_info_t info;
    if (!param_get_info(pid, &info)) return;

    const int bx = 6, by = 12, bw = SSD1306_W - 12, bh = 40;
    fb_fill_rect(fb, bx, by, bw, bh, false);   // очистить область под попапом
    fb_rect(fb, bx, by, bw, bh, true);

    fb_text(fb, bx + 6, by + 5, info.name ? info.name : "?", 1, true);

    char val[16];
    if (pid == PARAM_WAVEFORM) {
        const int idx = (int)(info.cur + 0.5f);
        snprintf(val, sizeof(val), "%s", kWave[(idx < 0 || idx > 3) ? 0 : idx]);
    } else if (info.type == PARAM_TYPE_FLOAT) {
        const int whole = (int)info.cur;
        int frac = (int)((info.cur - (float)whole) * 100.0f + 0.5f);
        if (frac < 0) frac = -frac;
        snprintf(val, sizeof(val), "%d.%02d", whole, frac);
    } else {
        snprintf(val, sizeof(val), "%d", (int)(info.cur + 0.5f));
    }
    fb_text(fb, bx + 6, by + 17, val, 2, true);

    if (info.max > info.min) {
        float frac = (info.cur - info.min) / (info.max - info.min);
        if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
        const int fw = (int)(frac * (float)(bw - 12) + 0.5f);
        fb_rect(fb, bx + 5, by + bh - 9, bw - 10, 5, true);
        if (fw > 0) fb_fill_rect(fb, bx + 6, by + bh - 8, fw, 3, true);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    static uint8_t fb[SSD1306_FB_SIZE];

    float   prev[PARAM_COUNT];   // снимок значений всех параметров (реестр вырос — этап 3)
    bool    prev_init = false;
    int     popup_id  = -1;
    int64_t popup_t0  = 0;

    const int64_t boot = esp_timer_get_time();

    for (;;) {
        const int64_t now = esp_timer_get_time();

        // Детекция смены параметра для popup: снимок значений, сравнение с прошлым кадром.
        const uint16_t n = param_count();
        if (!prev_init) {
            for (uint16_t i = 0; i < n; ++i) prev[i] = get_param(i);
            prev_init = true;
        } else {
            for (uint16_t i = 0; i < n; ++i) {
                const float v = get_param(i);
                if (v != prev[i]) { prev[i] = v; popup_id = (int)i; popup_t0 = now; }
            }
        }

        if (now - boot < SPLASH_US) {
            draw_splash(fb);
        } else {
            draw_scope(fb);
            if (popup_id >= 0 && now - popup_t0 < POPUP_US) draw_popup(fb, (uint16_t)popup_id);
        }
        ssd1306_flush(fb);
        vTaskDelay(pdMS_TO_TICKS(33));   // ~30 fps
    }
}

void display_init(void)
{
    // Общая I2C-шина (позже сюда же VL53L0X / MCP). Пока владелец — display (долг T-004).
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = PIN_SDA;
    bus_cfg.scl_io_num        = PIN_SCL;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus не создан (SDA=%d SCL=%d)", (int)PIN_SDA, (int)PIN_SCL);
        return;
    }
    if (!ssd1306_init(bus, OLED_ADDR)) {
        ESP_LOGW(TAG, "OLED не найден на 0x%02X — дисплей выкл (звук работает)", OLED_ADDR);
        return;   // дисплей опционален
    }

    xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 3, nullptr, 1);
    ESP_LOGI(TAG, "OLED SSD1306 128x64 @ 0x%02X, SDA=%d SCL=%d, задача на Core 1",
             OLED_ADDR, (int)PIN_SDA, (int)PIN_SCL);
}
