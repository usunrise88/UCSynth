// audio — аудио-тракт на Core 0: I2S std TX → PCM5102 (GY-PCM5102).
// Этап 3.0: моно-голос, управляемый нотами (NOTE_ON/OFF из comm через FreeRTOS-очередь),
// band-limited wavetable (октавные mip-таблицы, без алиасинга на верхах). Отладочный тест-тон
// (test_tone) перебивает ноты для проверки тракта. Плюс scope-буфер для осциллографа на дисплее.
// Параметры читаются ТОЛЬКО через control (get_param) — напрямую в DSP никогда.
// Железо и распиновка: docs/hardware-stage1-pcm5102.md.
#include "audio.h"
#include "control.h"
#include "wavetable.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
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

// Нотный путь Core 1 → Core 0 (этап 3.0). comm кладёт события в очередь, аудио-задача дренит
// её в начале блока. Состояние голоса (нота/gate/фаза) живёт ТОЛЬКО в аудио-задаче (Core 0) —
// очередь и есть точка передачи, атомики для голоса не нужны.
struct NoteEvent {
    uint8_t on;     // 1 = note-on, 0 = note-off
    uint8_t note;   // MIDI-номер
    uint8_t vel;    // velocity (в 3.0 не используется; задел под VCA 3.1)
};
static QueueHandle_t s_note_q = nullptr;

// MIDI-нота → частота (12-TET, A4=69=440 Гц). Зовём на событие ноты (control-rate), powf ок.
static inline float note_to_hz(uint8_t note)
{
    return 440.0f * powf(2.0f, ((int)note - 69) * (1.0f / 12.0f));
}

// Аудио-задача на Core 0: генерит блок семплов и блокируется на i2s_channel_write (пока
// DMA-буфер занят). Так задача сама тактируется скоростью вывода — без vTaskDelay/тика.
static void audio_task(void *arg)
{
    (void)arg;

    int16_t block[BLOCK_FRAMES * 2];   // STEREO-слот: L и R одинаковые (mono → оба канала)

    // Состояние моно-голоса — только здесь (Core 0), без атомиков.
    float   phase    = 0.0f;           // [0,1), фаза свободнобегущая (без ресета на ноте → без щелчка)
    float   note_hz  = 440.0f;         // частота текущей ноты
    bool    gate     = false;          // нота удерживается?
    uint8_t cur_note = 255;            // какая нота звучит (моно: note-off гасит только её)
    float   amp      = 0.0f;           // сглаженная амплитуда голоса (анти-клик; ADSR — этап 3.1)

    // Осциллограф: пишем форму (до громкости) в НЕактивный буфер, по заполнении окна — флип.
    int scope_wr  = 1 - s_scope_ready.load(std::memory_order_relaxed);
    int scope_pos = 0;

    // Анти-клик: линейная рампа amp к цели за ~5 мс (замена ADSR на 3.0; XSMT статически на 3.3В).
    const float amp_step = 1.0f / (0.005f * SAMPLE_RATE);

    for (;;) {
        const int64_t t_start = esp_timer_get_time();

        // Дренаж нотной очереди в начале блока: моно, последняя нота приоритетна (ретригер).
        NoteEvent ev;
        while (xQueueReceive(s_note_q, &ev, 0) == pdTRUE) {
            if (ev.on) {
                cur_note = ev.note;
                note_hz  = note_to_hz(ev.note);
                gate     = true;
            } else if (ev.note == cur_note) {
                gate = false;              // гасим только текущую (устаревший off чужой ноты игнор)
            }
        }

        // control-rate: параметры читаем раз в блок, не на каждый семпл.
        const float   vol     = get_param(PARAM_MASTER_VOLUME);
        const uint8_t wave    = (uint8_t)get_param(PARAM_WAVEFORM);
        const bool    test_on = get_param(PARAM_TEST_TONE) > 0.5f;

        // Тест-тон (отладка тракта) перебивает ноты: играет непрерывно на test_tone_hz.
        const float freq       = test_on ? get_param(PARAM_TEST_TONE_HZ) : note_hz;
        const float amp_target = (test_on || gate) ? 1.0f : 0.0f;
        const int   mip        = wavetable_mip(freq);          // выбор mip — раз в блок, без log2 в цикле
        const float phase_inc  = freq / (float)SAMPLE_RATE;    // оборотов на семпл

        for (int i = 0; i < BLOCK_FRAMES; ++i) {
            if (amp < amp_target)      { amp += amp_step; if (amp > amp_target) amp = amp_target; }
            else if (amp > amp_target) { amp -= amp_step; if (amp < amp_target) amp = amp_target; }

            const float s0 = wavetable_sample(wave, phase, mip);   // band-limited форма [-1,1]
            const float s  = s0 * amp * vol;                       // на выход (амплитуда × громкость)
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

void audio_note_on(uint8_t note, uint8_t vel)
{
    if (!s_note_q) return;
    const NoteEvent ev = { 1, note, vel };
    xQueueSend(s_note_q, &ev, 0);   // не блокируемся (зовут с Core 1); переполнение → событие теряется
}

void audio_note_off(uint8_t note)
{
    if (!s_note_q) return;
    const NoteEvent ev = { 0, note, 0 };
    xQueueSend(s_note_q, &ev, 0);
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

    // Band-limited wavetable-таблицы (октавные mip) под нашу частоту — до старта задачи.
    // Замеряем: генерация должна быть быстрой (float/FPU). Если снова десятки мс×1000 — регресс
    // на double-математику (у S3 нет аппаратного double → софт-эмуляция), см. wavetable.cpp.
    const int64_t t_wt = esp_timer_get_time();
    wavetable_init((float)SAMPLE_RATE);
    ESP_LOGI(TAG, "wavetable band-limited: mip-таблицы сгенерированы за %lld мс",
             (esp_timer_get_time() - t_wt) / 1000);

    // Нотная очередь Core 1 → Core 0. Глубина с запасом (шквал нот дренится за блок ~1.3 мс).
    s_note_q = xQueueCreate(32, sizeof(NoteEvent));

    // 3) Аудио-задача на Core 0 (Core 1 отдан comm/UI). Приоритет выше comm(5): звук важнее.
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 10, nullptr, 0);

    ESP_LOGI(TAG, "I2S TX %u Гц/16 бит, BCK=%d WS=%d DIN=%d — моно-голос + band-limited wt (3.0)",
             (unsigned)SAMPLE_RATE, (int)PIN_BCK, (int)PIN_WS, (int)PIN_DIN);
}
