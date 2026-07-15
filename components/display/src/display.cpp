// display — рендер отладочного OLED на Core 1 (beauty-pass).
// Grayscale intensity-буфер → Bayer-дизеринг. Splash с фейдом → визуализатор: осциллограф с
// послесвечением (phosphor) + статус + метрики + level bar. Поверх — param-popup при смене
// параметра (детекция изменений на стороне дисплея — control/lock-free не трогаем).
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

static constexpr int64_t SPLASH_US = 2000000;   // длительность splash
static constexpr int64_t FADE_US   = 400000;    // фейд splash in/out
static constexpr int64_t POPUP_US  = 1000000;   // показ значения параметра (1 с)

static const char *kWave[] = {"sine", "saw", "square", "tri"};

// ——— splash ———
static void draw_splash(GrayCanvas &g)
{
    g_clear(g);
    g_rect(g, 4, 4, SSD1306_W - 8, SSD1306_H - 8, 170);
    g_text(g, (SSD1306_W - g_text_width("UCSynth", 2)) / 2, 14, "UCSynth", 2, 255);
    g_text(g, (SSD1306_W - g_text_width("unnecessary", 1)) / 2, 38, "unnecessary", 1, 190);
    g_text(g, (SSD1306_W - g_text_width("complicated", 1)) / 2, 48, "complicated", 1, 190);
}

static uint16_t splash_fade(int64_t age)   // 0..256
{
    if (age < FADE_US)              return (uint16_t)(256 * age / FADE_US);
    if (age > SPLASH_US - FADE_US)  return (uint16_t)(256 * (SPLASH_US - age) / FADE_US);
    return 256;
}

// ——— визуализатор: осциллограф с послесвечением ———
static void draw_scope(GrayCanvas &g)
{
    // Послесвечение: гасим кадр, крисп-элементы рисуем заново на полную, а старый след
    // осциллографа не перерисовываем → он затухает (phosphor decay).
    g_dim(g, 150, 256);

    char line[24];
    const float freq = get_param(PARAM_TEST_TONE_HZ);
    const int   wf   = (int)get_param(PARAM_WAVEFORM);
    snprintf(line, sizeof(line), "%d Hz  %s", (int)(freq + 0.5f), kWave[(wf < 0 || wf > 3) ? 0 : wf]);
    g_text(g, 2, 0, line, 1, 255);

    uint32_t cpu = 0, ur = 0;
    audio_get_stats(&cpu, &ur);
    char m[24];
    snprintf(m, sizeof(m), "cpu %u.%u%%  ur %u",
             (unsigned)(cpu / 10), (unsigned)(cpu % 10), (unsigned)ur);
    g_text(g, 2, 46, m, 1, 200);

    // Осевая (пунктир).
    const int top = 12, bot = 45, mid = (top + bot) / 2, amp = (bot - top) / 2;
    for (int x = 0; x < SSD1306_W; x += 3) g_pixel(g, x, mid, 70);

    // Новый след — на полную яркость.
    int8_t scope[AUDIO_SCOPE_LEN];
    audio_scope_read(scope);
    int prev_y = mid - (scope[0] * amp) / 127;
    for (int x = 1; x < AUDIO_SCOPE_LEN; ++x) {
        const int y = mid - (scope[x] * amp) / 127;
        g_line(g, x - 1, prev_y, x, y, 255);
        prev_y = y;
    }

    // Level bar (крисп).
    const float vol = get_param(PARAM_MASTER_VOLUME);
    const int bw = (int)(vol * 120.0f + 0.5f);
    g_rect(g, 2, 54, 124, 9, 190);
    if (bw > 0) g_fill_rect(g, 4, 56, bw, 5, 255);
}

// ——— popup значения параметра ———
static void draw_popup(GrayCanvas &g, uint16_t pid)
{
    param_info_t info;
    if (!param_get_info(pid, &info)) return;

    const int bx = 6, by = 12, bw = SSD1306_W - 12, bh = 40;
    g_fill_rect(g, bx, by, bw, bh, 0);         // чёрная подложка (читаемо)
    g_rect(g, bx, by, bw, bh, 255);

    g_text(g, bx + 6, by + 5, info.name ? info.name : "?", 1, 220);

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
    g_text(g, bx + 6, by + 17, val, 2, 255);

    if (info.max > info.min) {
        float frac = (info.cur - info.min) / (info.max - info.min);
        if (frac < 0) frac = 0; else if (frac > 1) frac = 1;
        const int fw = (int)(frac * (float)(bw - 12) + 0.5f);
        g_rect(g, bx + 5, by + bh - 9, bw - 10, 5, 180);
        if (fw > 0) g_fill_rect(g, bx + 6, by + bh - 8, fw, 3, 255);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    static GrayCanvas g;                 // 8 КБ в BSS
    static uint8_t    fb[SSD1306_FB_SIZE];

    float   prev[16];
    bool    prev_init = false;
    int     popup_id  = -1;
    int64_t popup_t0  = 0;

    const int64_t boot = esp_timer_get_time();

    for (;;) {
        const int64_t now = esp_timer_get_time();

        // Детекция смены параметра (для popup): снимок значений, сравнение с прошлым кадром.
        const uint16_t n = param_count();
        if (!prev_init) {
            for (uint16_t i = 0; i < n && i < 16; ++i) prev[i] = get_param(i);
            prev_init = true;
        } else {
            for (uint16_t i = 0; i < n && i < 16; ++i) {
                const float v = get_param(i);
                if (v != prev[i]) { prev[i] = v; popup_id = (int)i; popup_t0 = now; }
            }
        }

        if (now - boot < SPLASH_US) {
            draw_splash(g);
            g_dither(g, fb, splash_fade(now - boot), 256);
        } else {
            draw_scope(g);
            if (popup_id >= 0 && now - popup_t0 < POPUP_US) draw_popup(g, (uint16_t)popup_id);
            g_dither(g, fb, 256, 256);
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
