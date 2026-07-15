// audio — аудио-тракт на Core 0: I2S std TX → PCM5102 (GY-PCM5102).
// Этап 1.1: чистый синус (test_tone_hz × master_volume) для проверки железа тракта.
// Плюс scope-буфер: снимок формы волны для осциллографа на дисплее (Core 1).
// Параметры читаются ТОЛЬКО через control (get_param) — напрямую в DSP никогда.
// Железо и распиновка: docs/hardware-stage1-pcm5102.md.
#include "audio.h"
#include "control.h"
#include "wavetable.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cmath>
#include <cstdint>
#include <atomic>

static const char *TAG = "audio";

// Формат тракта (см. план этапа 1). 16 бит / 48 кГц; расширить до 24/32 — правка конвертера.
static constexpr uint32_t SAMPLE_RATE  = 48000;
static constexpr int      BLOCK_FRAMES = 64;        // блок обработки (32–128 семплов)

// GPIO I2S → PCM5102. MCLK не используется: SCK модуля на GND (внутренний PLL из BCK).
static constexpr gpio_num_t PIN_BCK = GPIO_NUM_5;   // bit clock
static constexpr gpio_num_t PIN_WS  = GPIO_NUM_6;   // word select (LRCK)
static constexpr gpio_num_t PIN_DIN = GPIO_NUM_7;   // data (S3 → модуль)

static i2s_chan_handle_t s_tx = nullptr;

// Осциллограф: двойной буфер снимков формы. Core 0 пишет, Core 1 читает. Без блокировок и
// практически без тиринга: читатель копирует за микросекунды, писатель флипает раз в ~2.7 мс.
static int8_t s_scope[2][AUDIO_SCOPE_LEN];
static std::atomic<int> s_scope_ready{0};

// Метрики аудио-задачи (этап 1.2). Пишет Core 0, читают STAT/дисплей (Core 1).
static constexpr uint32_t BLOCK_US = (uint32_t)(1000000ULL * BLOCK_FRAMES / SAMPLE_RATE);
static std::atomic<uint32_t> s_cpu_permille{0};   // ‰ бюджета блока (EMA)
static std::atomic<uint32_t> s_late_blocks{0};    // блоки, не уложившиеся в realtime

// Аудио-задача на Core 0: генерит блок семплов и блокируется на i2s_channel_write (пока
// DMA-буфер занят). Так задача сама тактируется скоростью вывода — без vTaskDelay/тика.
static void audio_task(void *arg)
{
    (void)arg;

    int16_t block[BLOCK_FRAMES * 2];   // STEREO-слот: L и R одинаковые (mono → оба канала)
    float phase = 0.0f;                // [0,1), живёт только здесь (Core 0) — без атомиков

    // Осциллограф: пишем форму (до громкости) в НЕактивный буфер, по заполнении окна — флип.
    int scope_wr  = 1 - s_scope_ready.load(std::memory_order_relaxed);
    int scope_pos = 0;

    // Антипоп: плавный fade-in ~8 мс (XSMT статически на 3.3В, программно им не управляем).
    const float fade_step = 1.0f / (0.008f * SAMPLE_RATE);
    float fade = 0.0f;

    for (;;) {
        const int64_t t_start = esp_timer_get_time();

        // control-rate: параметры читаем раз в блок, не на каждый семпл.
        const float   freq      = get_param(PARAM_TEST_TONE_HZ);
        const float   vol       = get_param(PARAM_MASTER_VOLUME);
        const uint8_t wave      = (uint8_t)get_param(PARAM_WAVEFORM);
        const float   phase_inc = freq / (float)SAMPLE_RATE;   // оборотов на семпл

        for (int i = 0; i < BLOCK_FRAMES; ++i) {
            if (fade < 1.0f) { fade += fade_step; if (fade > 1.0f) fade = 1.0f; }

            const float s0 = wavetable_sample(wave, phase);   // форма волны [-1,1]
            const float s  = s0 * vol * fade;        // на выход (с громкостью)
            phase += phase_inc;
            if (phase >= 1.0f) phase -= 1.0f;

            const int16_t v = (int16_t)lrintf(s * 32767.0f);
            block[2 * i]     = v;   // L
            block[2 * i + 1] = v;   // R

            // Снимок формы для дисплея; по заполнении окна публикуем буфер (release).
            s_scope[scope_wr][scope_pos++] = (int8_t)lrintf(s0 * 127.0f);
            if (scope_pos >= AUDIO_SCOPE_LEN) {
                s_scope_ready.store(scope_wr, std::memory_order_release);
                scope_wr ^= 1;
                scope_pos = 0;
            }
        }

        // Метрики: время генерации блока против бюджета realtime (этап 1.2).
        const uint32_t gen_us   = (uint32_t)(esp_timer_get_time() - t_start);
        const uint32_t permille = gen_us * 1000u / BLOCK_US;
        s_cpu_permille.store((s_cpu_permille.load(std::memory_order_relaxed) * 7 + permille) / 8,
                             std::memory_order_relaxed);
        if (gen_us > BLOCK_US) s_late_blocks.fetch_add(1, std::memory_order_relaxed);

        size_t written = 0;
        i2s_channel_write(s_tx, block, sizeof(block), &written, portMAX_DELAY);
    }
}

void audio_scope_read(int8_t *out)
{
    const int r = s_scope_ready.load(std::memory_order_acquire);
    for (int i = 0; i < AUDIO_SCOPE_LEN; ++i) out[i] = s_scope[r][i];
}

void audio_get_stats(uint32_t *cpu_permille, uint32_t *underruns)
{
    if (cpu_permille) *cpu_permille = s_cpu_permille.load(std::memory_order_relaxed);
    if (underruns)    *underruns    = s_late_blocks.load(std::memory_order_relaxed);
}

void audio_init(void)
{
    // 1) Канал I2S: master TX + DMA. auto_clear=true → при underrun в буфер идёт тишина,
    //    а не «зажёванный» старый блок (меньше артефактов на срыве).
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = BLOCK_FRAMES;
    chan_cfg.auto_clear    = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, nullptr));

    // 2) Standard-режим, Philips (= стандартный I2S; на модуле FMT→GND). 16 бит, стерео-слот.
    //    MCLK не заводим (SCK модуля на GND), din не нужен (только выход).
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_BCK,
            .ws   = PIN_WS,
            .dout = PIN_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {},          // без инверсии клоков
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));

    // Таблицы форм волн (генерятся в RAM один раз) — до старта задачи.
    wavetable_init();

    // 3) Аудио-задача на Core 0 (Core 1 отдан comm/UI). Приоритет выше comm(5): звук важнее.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 10, nullptr, 0);

    ESP_LOGI(TAG, "I2S TX %u Гц/16 бит, BCK=%d WS=%d DIN=%d — синус (этап 1.1)",
             (unsigned)SAMPLE_RATE, (int)PIN_BCK, (int)PIN_WS, (int)PIN_DIN);
}
