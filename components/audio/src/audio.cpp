// audio — аудио-тракт на Core 0: I2S std TX → PCM5102 (GY-PCM5102).
// Этапы 3.1–3.4: полноценный моно-голос (voice.*) — 3 осц → микшер(+шум +ring) → ZDF-фильтр → VCA,
// две ADSR, drone (latch/loop), lo-fi. Управляется нотами (NOTE_ON/OFF из comm через FreeRTOS-
// очередь). Отладочный тест-тон (test_tone) — минимальный сквозной путь для проверки тракта.
// Плюс scope-буфер для осциллографа на дисплее. audio.cpp — I/O-оболочка: читает control и
// передаёт параметры в чистый DSP (voice), напрямую в DSP значения не идут.
// Железо и распиновка: docs/hardware-stage1-pcm5102.md.
#include "audio.h"
#include "control.h"
#include "wavetable.h"
#include "synth.h"
#include "lfo.h"
#include "fx.h"
#include "seq_engine.h"
#include "seq_codec.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

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

// Таймаут записи в I2S. Параметр i2s_channel_write — миллисекунды (внутри pdMS_TO_TICKS), поэтому
// portMAX_DELAY туда передавать нельзя. Блок длится 1.33 мс; 200 мс — заведомо «никогда» в норме.
static constexpr uint32_t I2S_WRITE_TIMEOUT_MS = 200;
static uint32_t s_write_fails = 0;   // только аудио-задача (Core 0) — синхронизация не нужна

// Состояние эффект-секции (этап 5). Кольцевые буферы delay — в PSRAM (аллокация в audio_init).
static FxState s_fx = {};

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
    uint8_t vel;    // velocity (задел под вел-чувствительность; в 3.1 не используется)
};
static QueueHandle_t s_note_q = nullptr;

// Обновление шага паттерна: comm (Core 1) → аудио-задача (Core 0) через очередь (этап 7.4). Весь SeqStep
// в элементе (~64 Б); comm десериализует блоб, кладёт сюда, задача применяет в s_seq.pattern[step] до тика.
struct StepUpdate { uint8_t step; SeqStep data; };
static QueueHandle_t s_step_q = nullptr;

// Секвенсор/арпеджиатор (этап 7): темп-клок + p-lock оверрайды. Живёт на Core 0 (тикается в audio_task,
// читается pr() в build_synth_params — обе точки в одной задаче, без гонок).
static SeqEngine s_seq;

// Чтение параметра С УЧЁТОМ p-lock-оверрайда текущего шага (этап 7). Если шаг переопределяет параметр —
// вернуть значение оверрайда (транзиентно, реестр не трогаем), иначе обычный get_param. В 7.1 оверрайдов
// нет → эквивалент get_param.
static inline float pr(uint16_t id)
{
    float v;
    return seq_plock(&s_seq, id, &v) ? v : get_param(id);
}

// Собрать параметры синта из реестра (раз в блок, ~30 атомарных чтений — дёшево). DSP сам
// control не трогает — вся связь control→DSP здесь. Чтения идут через pr() → p-lock шага применяется
// прозрачно ко всем voice/fx-параметрам.
static void build_synth_params(SynthParams *sp)
{
    VoiceParams *vp = &sp->voice;
    vp->osc[0] = { (uint8_t)pr(PARAM_WAVEFORM),  pr(PARAM_OSC1_DETUNE), pr(PARAM_OSC1_LEVEL) };
    vp->osc[1] = { (uint8_t)pr(PARAM_OSC2_WAVE), pr(PARAM_OSC2_DETUNE), pr(PARAM_OSC2_LEVEL) };
    vp->osc[2] = { (uint8_t)pr(PARAM_OSC3_WAVE), pr(PARAM_OSC3_DETUNE), pr(PARAM_OSC3_LEVEL) };
    vp->noise_level = pr(PARAM_NOISE_LEVEL);
    vp->ring_level  = pr(PARAM_RING_LEVEL);
    vp->cutoff_hz   = pr(PARAM_CUTOFF);
    vp->resonance   = pr(PARAM_RESONANCE);
    vp->filt_mode   = (uint8_t)pr(PARAM_FILTER_MODE);
    vp->flt_env_amt = pr(PARAM_FLT_ENV_AMT);
    vp->amp_env = { pr(PARAM_AMP_ATTACK), pr(PARAM_AMP_DECAY),
                    pr(PARAM_AMP_SUSTAIN), pr(PARAM_AMP_RELEASE),
                    pr(PARAM_AMP_LOOP) > 0.5f };
    vp->flt_env = { pr(PARAM_FLT_ATTACK), pr(PARAM_FLT_DECAY),
                    pr(PARAM_FLT_SUSTAIN), pr(PARAM_FLT_RELEASE),
                    pr(PARAM_FLT_LOOP) > 0.5f };
    vp->lofi       = pr(PARAM_LOFI) > 0.5f;
    vp->lofi_bits  = (int)pr(PARAM_LOFI_BITS);
    vp->latch      = pr(PARAM_LATCH) > 0.5f;
    vp->glide_time = pr(PARAM_GLIDE_TIME);
    // этап 4.1 — мод-источники + матрица. Обнуляем все источники; глобальный mod-wheel ставим здесь,
    // LFO допишет audio_task после тика, пер-голосные (VCF-env, velocity) — сам голос в voice_render.
    for (int s = 0; s < MOD_SRC_COUNT; ++s) vp->mod_src[s] = 0.0f;
    vp->mod_src[MOD_SRC_MODWHEEL] = pr(PARAM_MOD_WHEEL);
    for (int s = 0; s < MOD_SLOTS; ++s) {
        vp->mtx[s].src   = (uint8_t)pr(PARAM_MTX1_SRC   + s * 3);
        vp->mtx[s].dst   = (uint8_t)pr(PARAM_MTX1_DST   + s * 3);
        vp->mtx[s].depth = pr(PARAM_MTX1_DEPTH + s * 3);
    }
    // этап 4.2 — wave-огибающая (8 точек подряд + rate + loop)
    for (int i = 0; i < WAVEENV_POINTS; ++i) vp->wave_env.pts[i] = pr(PARAM_WAVEENV_P1 + i);
    vp->wave_env.rate = pr(PARAM_WAVEENV_RATE);
    vp->wave_env.loop = pr(PARAM_WAVEENV_LOOP) > 0.5f;
    sp->poly_voices = (int)pr(PARAM_POLY_VOICES);
    sp->legato      = pr(PARAM_LEGATO) > 0.5f;
    // этап 5 — глобальные эффекты (применяются в audio_task после суммы голосов)
    sp->fx.od_on    = pr(PARAM_OD_ON) > 0.5f;
    sp->fx.od_drive = pr(PARAM_OD_DRIVE);
    sp->fx.od_mix   = pr(PARAM_OD_MIX);
    sp->fx.delay_on       = pr(PARAM_DELAY_ON) > 0.5f;
    sp->fx.delay_time     = pr(PARAM_DELAY_TIME);
    sp->fx.delay_feedback = pr(PARAM_DELAY_FEEDBACK);
    sp->fx.delay_damp     = pr(PARAM_DELAY_DAMP);
    sp->fx.delay_mix      = pr(PARAM_DELAY_MIX);
    sp->fx.reverb_on      = pr(PARAM_REVERB_ON) > 0.5f;
    sp->fx.reverb_size    = pr(PARAM_REVERB_SIZE);
    sp->fx.reverb_damp    = pr(PARAM_REVERB_DAMP);
    sp->fx.reverb_width   = pr(PARAM_REVERB_WIDTH);
    sp->fx.reverb_mix     = pr(PARAM_REVERB_MIX);
    sp->fx.reverb_moddepth = pr(PARAM_REVERB_MODDEPTH);
    sp->fx.reverb_modrate  = pr(PARAM_REVERB_MODRATE);
}

// --- Секвенсор: паттерн I/O (этап 7.4). set — из comm (Core 1) через очередь → применяется на Core 0.
//     get — сериализация текущего паттерна (Core 1 читает s_seq.pattern; тот меняется только дренажом
//     очереди на Core 0, редко → безопасно на практике для ручных GET/SAVE из GUI). ---
bool audio_seq_set_step(uint8_t step, const uint8_t *blob, size_t len)
{
    if (step >= SEQ_STEPS || !s_step_q) return false;
    StepUpdate su; su.step = step;
    if (seq_step_deserialize(blob, len, &su.data) == 0) return false;
    return xQueueSend(s_step_q, &su, 0) == pdTRUE;
}

bool audio_seq_set_pattern(const uint8_t *blob, size_t len)
{
    if (!s_step_q) return false;
    SeqStep steps[SEQ_STEPS];
    if (!seq_deserialize(blob, len, steps)) return false;
    for (uint8_t s = 0; s < SEQ_STEPS; ++s) {
        StepUpdate su; su.step = s; su.data = steps[s];
        xQueueSend(s_step_q, &su, pdMS_TO_TICKS(5));   // ждём место: 16 шагов подряд (очередь 40)
    }
    return true;
}

size_t audio_seq_get_step(uint8_t step, uint8_t *out, size_t cap)
{
    if (step >= SEQ_STEPS) return 0;
    return seq_step_serialize(out, cap, &s_seq.pattern[step]);
}

size_t audio_seq_get_pattern(uint8_t *out, size_t cap)
{
    return seq_serialize(out, cap, s_seq.pattern);
}

// Аудио-задача на Core 0: генерит блок семплов и блокируется на i2s_channel_write (пока
// DMA-буфер занят). Так задача сама тактируется скоростью вывода — без vTaskDelay/тика.
static void audio_task(void *arg)
{
    (void)arg;

    int16_t block[BLOCK_FRAMES * 2];   // STEREO: L, R (до этапа 5.2 совпадали; теперь стерео-тракт FX)
    float   fbuf[BLOCK_FRAMES];        // float-микс синта (сумма голосов)
    float   chL[BLOCK_FRAMES], chR[BLOCK_FRAMES];   // стерео-тракт эффектов (этап 5.2 delay)

    float tt_phase = 0.0f;             // фаза тест-тона (отдельный сквозной путь, без env/фильтра)

    // Осциллограф: пишем форму (до громкости) в НЕактивный буфер, по заполнении окна — флип.
    int scope_wr  = 1 - s_scope_ready.load(std::memory_order_relaxed);
    int scope_pos = 0;

    // Глобальные LFO (этап 4.1): тик раз в блок (control-rate). Состояние живёт всю жизнь задачи
    // (локали переживают итерации — из audio_task не выходим). Посев разный → LFO2 не в фазе с LFO1.
    Lfo lfo[2];
    lfo_reset(&lfo[0], 0xA5A50001u);
    lfo_reset(&lfo[1], 0x5A5A0002u);
    const float lfo_dt = (float)BLOCK_FRAMES / (float)SAMPLE_RATE;

    for (;;) {
        const int64_t t_start = esp_timer_get_time();

        // Обновления паттерна из comm (этап 7.4) → применить ДО тика (новый паттерн играет с этого блока).
        StepUpdate su;
        while (xQueueReceive(s_step_q, &su, 0) == pdTRUE)
            if (su.step < SEQ_STEPS) s_seq.pattern[su.step] = su.data;

        // Секвенсор/арп (этап 7): продвинуть темп-клок ПЕРЕД сборкой параметров — p-lock активного шага
        // должен уже стоять, когда build_synth_params читает через pr(). Ноты шага/арпа — этап 7.2/7.3.
        const SeqConfig scfg = {
            (int)get_param(PARAM_SEQ_BPM), get_param(PARAM_SEQ_SWING),
            get_param(PARAM_SEQ_PLAYING) > 0.5f, get_param(PARAM_SEQ_ON) > 0.5f,
            get_param(PARAM_ARP_ON) > 0.5f, (uint8_t)get_param(PARAM_ARP_MODE),
            (int)get_param(PARAM_ARP_OCTAVES), (int)get_param(PARAM_ARP_RATE),
            get_param(PARAM_ARP_HOLD) > 0.5f,
        };
        SeqNoteEvent seq_ev[16];
        const int seq_nev = seq_tick(&s_seq, &scfg, (float)SAMPLE_RATE, BLOCK_FRAMES, seq_ev, 16);

        // Параметры синта — до дренажа (нужны аллокатору при note-on: poly/legato/glide).
        SynthParams sp;
        build_synth_params(&sp);
        // Режим полифонии — тоже до дренажа: смена poly делает all-notes-off, и нота, пришедшая
        // в этом же блоке, иначе получила бы key_down=false сразу после аллокации и пропала.
        synth_set_poly(sp.poly_voices);
        // Тик глобальных LFO (control-rate) → мод-источники. Читатель — мод-матрица в voice_render.
        sp.voice.mod_src[MOD_SRC_LFO1] = lfo_tick(&lfo[0], get_param(PARAM_LFO1_RATE), lfo_dt,
                                                  (uint8_t)get_param(PARAM_LFO1_SHAPE));
        sp.voice.mod_src[MOD_SRC_LFO2] = lfo_tick(&lfo[1], get_param(PARAM_LFO2_RATE), lfo_dt,
                                                  (uint8_t)get_param(PARAM_LFO2_SHAPE));

        // Ноты секвенсора/арпа (этап 7): fire ПОСЛЕ сборки sp (p-lock шага уже в sp через pr()) — голос
        // аллоцируется с параметрами этого шага. Идут в тот же synth, что и клавиши/MIDI.
        for (int i = 0; i < seq_nev; ++i) {
            if (seq_ev[i].on) synth_note_on(&sp, seq_ev[i].note, seq_ev[i].vel);
            else              synth_note_off(&sp, seq_ev[i].note);
        }

        // Дренаж нотной очереди → синт, либо в арпеджиатор (этап 7.3): при arp_on клавиши/MIDI кормят
        // множество held арпа (звучит его последовательность), не играют напрямую.
        NoteEvent ev;
        while (xQueueReceive(s_note_q, &ev, 0) == pdTRUE) {
            if (scfg.arp_on)   seq_arp_note(&s_seq, ev.note, ev.on != 0, scfg.arp_hold);
            else if (ev.on)    synth_note_on(&sp, ev.note, ev.vel);
            else               synth_note_off(&sp, ev.note);
        }

        // control-rate: мастер и режим тест-тона.
        const float master  = get_param(PARAM_MASTER_VOLUME);
        const bool  test_on = get_param(PARAM_TEST_TONE) > 0.5f;

        if (test_on) {
            // Эталон тракта: осц1-форма на test_tone_hz, полная амплитуда, БЕЗ синта/софт-клипа.
            const float   ttf = get_param(PARAM_TEST_TONE_HZ);
            const uint8_t ttw = (uint8_t)get_param(PARAM_WAVEFORM);
            const int     ttm = wavetable_mip(ttf);
            const float   inc = ttf / (float)SAMPLE_RATE;
            for (int i = 0; i < BLOCK_FRAMES; ++i) {
                fbuf[i] = wavetable_sample(ttw, tt_phase, ttm);
                tt_phase += inc;
                if (tt_phase >= 1.0f) tt_phase -= 1.0f;
            }
        } else {
            synth_render(&sp, (float)SAMPLE_RATE, fbuf, BLOCK_FRAMES);   // сумма активных голосов
        }

        // Хвост эффектов: overdrive → МАСТЕР-HARD-CLAMP (ограничение суммы голосов в [-1,1] ДО
        // эффектов) → split L/R → delay (стерео) → reverb (стерео) → hard-clamp пиков wet → scope(L)
        // → громкость → int16. Ограничение ДО эффектов принципиально: delay/reverb с обратной связью
        // калиброваны на вход ~[-1,1]; если кормить сырой суммой голосов (может быть 3–8×), wet
        // раздувается и даже 1% mix звучит громко.
        //
        // Здесь был ВТОРОЙ soft-clip m/(1+|m|) — тот же, что уже стоит в голосе. Из-за него тракт
        // сжимался дважды: одиночный голос с пиком 0.5 выходил на 0.333 (−9.5 dBFS при master=1), а
        // сумма 8 голосов (~4) — на 0.8, то есть аккорд был громче одной ноты всего в 2.4 раза вместо
        // 8. Схема, зафиксированная в tech-debt D-010, другая: soft-clip в ГОЛОСЕ + hard-clamp на
        // мастере. Приводим код к документу — hard-clamp так же надёжно даёт [-1,1] для FX, но
        // прозрачен ниже единицы, так что уровни перестают проседать без причины.
        for (int i = 0; i < BLOCK_FRAMES; ++i) {
            float m = fbuf[i];
            if (!test_on) {
                m = fx_overdrive(m, &sp.fx);
                if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;   // мастер hard-clamp (D-010)
            }
            chL[i] = chR[i] = m;
        }
        if (!test_on) {
            fx_delay(&s_fx, &sp.fx, chL, chR, BLOCK_FRAMES, (float)SAMPLE_RATE);
            fx_reverb(&s_fx, &sp.fx, chL, chR, BLOCK_FRAMES);   // этап 5.3 — последний в цепи
        }

        for (int i = 0; i < BLOCK_FRAMES; ++i) {
            float L = chL[i], R = chR[i];
            if (!test_on) {                              // wet эффектов может выйти за ±1 → жёсткий предел
                if (L > 1.0f) L = 1.0f; else if (L < -1.0f) L = -1.0f;   // (сухой уже нормирован → no-op)
                if (R > 1.0f) R = 1.0f; else if (R < -1.0f) R = -1.0f;
            }
            // Снимок формы (канал L, до громкости) для дисплея: для нот — после софт-клипа,
            // для тест-тона — сам эталон (клип обходится).
            s_scope[scope_wr][scope_pos++] = (int8_t)lrintf(L * 127.0f);
            if (scope_pos >= AUDIO_SCOPE_LEN) {
                s_scope_ready.store(scope_wr, std::memory_order_release);
                scope_wr ^= 1;
                scope_pos = 0;
            }

            float outL = L * master, outR = R * master;
            if (outL > 1.0f) outL = 1.0f; else if (outL < -1.0f) outL = -1.0f;   // страховка перед int16
            if (outR > 1.0f) outR = 1.0f; else if (outR < -1.0f) outR = -1.0f;
            block[2 * i]     = (int16_t)lrintf(outL * 32767.0f);   // L
            block[2 * i + 1] = (int16_t)lrintf(outR * 32767.0f);   // R
        }

        // Метрики: время генерации блока против бюджета realtime (этап 1.2).
        const uint32_t gen_us   = (uint32_t)(esp_timer_get_time() - t_start);
        const uint32_t permille = gen_us * 1000u / BLOCK_US;
        s_cpu_permille.store((s_cpu_permille.load(std::memory_order_relaxed) * 7 + permille) / 8,
                             std::memory_order_relaxed);
        if (gen_us > BLOCK_US) s_late_blocks.fetch_add(1, std::memory_order_relaxed);

        // Таймаут в МИЛЛИСЕКУНДАХ, не в тиках: внутри он идёт в pdMS_TO_TICKS, и portMAX_DELAY
        // свернулся бы в ~11.9 часа, а не в бесконечность. Берём заведомо больше длительности
        // блока, но конечное — чтобы отказ канала был виден, а не превратился в вечное ожидание.
        //
        // Возврат и written проверяем: i2s_channel_write крутит цикл, пока канал в состоянии
        // RUNNING, и при выходе из него возвращается с ЧАСТИЧНОЙ записью. Без проверки задача
        // провалилась бы сквозь write в генерацию следующего блока без точки блокировки —
        // 100% Core 0, голодание IDLE0, TWDT каждые 5 с и ни строчки в логе о причине.
        size_t written = 0;
        const esp_err_t werr = i2s_channel_write(s_tx, block, sizeof(block), &written,
                                                 I2S_WRITE_TIMEOUT_MS);
        if (werr != ESP_OK || written != sizeof(block)) {
            ++s_write_fails;
            if (s_write_fails <= 3 || (s_write_fails % 1000) == 0) {   // не заливаем лог
                ESP_LOGE(TAG, "i2s_channel_write: %s, записано %u из %u (сбоев: %lu)",
                         esp_err_to_name(werr), (unsigned)written, (unsigned)sizeof(block),
                         (unsigned long)s_write_fails);
            }
            // Отдаём процессор, чтобы при постоянном отказе канала задача не съела Core 0 целиком.
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

// Асимметрия таймаутов намеренная. Потерянный NOTE_ON лишь неслышен, потерянный NOTE_OFF — вечный
// дрон: голос остаётся с открытым gate, других механизмов релиза нет, и снять его можно только
// повторным нажатием той же ноты. Поэтому note-off ждёт место в очереди (зовут из comm_task на
// Core 1 — блокироваться на десятки мс там можно), а note-on по-прежнему не ждёт.
static constexpr TickType_t NOTE_OFF_WAIT = pdMS_TO_TICKS(20);

void audio_note_on(uint8_t note, uint8_t vel)
{
    if (!s_note_q) return;
    const NoteEvent ev = { 1, note, vel };
    if (xQueueSend(s_note_q, &ev, 0) != pdTRUE) {   // не блокируемся (зовут с Core 1)
        ESP_LOGW(TAG, "нотная очередь полна — NOTE_ON %u потерян", (unsigned)note);
    }
}

void audio_note_off(uint8_t note)
{
    if (!s_note_q) return;
    const NoteEvent ev = { 0, note, 0 };
    if (xQueueSend(s_note_q, &ev, NOTE_OFF_WAIT) != pdTRUE) {
        ESP_LOGE(TAG, "нотная очередь полна — NOTE_OFF %u потерян, нота залипнет", (unsigned)note);
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

    // Band-limited wavetable-таблицы (октавные mip) под нашу частоту — до старта задачи.
    // Замеряем: генерация должна быть быстрой (float/FPU). Если снова десятки мс×1000 — регресс
    // на double-математику (у S3 нет аппаратного double → софт-эмуляция), см. wavetable.cpp.
    const int64_t t_wt = esp_timer_get_time();
    wavetable_init((float)SAMPLE_RATE);
    ESP_LOGI(TAG, "wavetable band-limited: mip-таблицы сгенерированы за %lld мс",
             (esp_timer_get_time() - t_wt) / 1000);

    // Пул голосов (статический в synth) — до старта задачи.
    synth_init();
    seq_init(&s_seq);   // секвенсор/арп (этап 7): темп-клок в состоянии «стоп»

    // FX-буферы delay — ТОЛЬКО PSRAM (этап 5.2). Стерео 1 с @48к = 2×48000 float ≈ 384 КБ; во внутренний
    // DRAM не влезет и не нужно. Первая heap_caps-аллокация в проекте. Не вышло (PSRAM off/фрагментация)
    // → s_fx.dl_l/dl_r = nullptr, fx_delay сам пропускает эффект (delay просто не работает, звук — нет).
    const int dl_len = SAMPLE_RATE;   // 1 секунда
    float *dl_l = (float *)heap_caps_malloc(dl_len * sizeof(float), MALLOC_CAP_SPIRAM);
    float *dl_r = (float *)heap_caps_malloc(dl_len * sizeof(float), MALLOC_CAP_SPIRAM);
    if (dl_l && dl_r) {
        fx_delay_init(&s_fx, dl_l, dl_r, dl_len);
        ESP_LOGI(TAG, "FX delay: буферы в PSRAM %d КБ (стерео 1 с)", (int)(2 * dl_len * sizeof(float) / 1024));
    } else {
        if (dl_l) heap_caps_free(dl_l);
        if (dl_r) heap_caps_free(dl_r);
        fx_delay_init(&s_fx, nullptr, nullptr, 0);   // delay отключён, тракт работает без него
        ESP_LOGE(TAG, "FX delay: не выделить PSRAM (%d КБ) — delay отключён", (int)(2 * dl_len * sizeof(float) / 1024));
    }

    // FX reverb (Freeverb, этап 5.3) — потолок CPU. Линии гребёнок/allpass (~110 КБ) кладём во ВНУТРЕННИЙ
    // RAM (одноцикловый доступ), а НЕ в PSRAM (медленнее, через кэш): ревер бьёт по 24 буферам на каждый
    // выходной сэмпл, латентность PSRAM тут дорога. Не влезло во внутр. RAM → откат в PSRAM (работает).
    // Не вышло совсем → fx_reverb_init(nullptr) отключает реверб (тракт работает).
    const int rv_n = fx_reverb_bufsize();
    float *rv_buf = (float *)heap_caps_malloc(rv_n * sizeof(float), MALLOC_CAP_INTERNAL);
    const char *rv_where = "внутр. RAM";
    if (!rv_buf) {
        rv_buf = (float *)heap_caps_malloc(rv_n * sizeof(float), MALLOC_CAP_SPIRAM);
        rv_where = "PSRAM (внутр. RAM не хватило)";
    }
    if (rv_buf) {
        fx_reverb_init(&s_fx, rv_buf, rv_n);
        ESP_LOGI(TAG, "FX reverb: буферы в %s, %d КБ (Freeverb 8×гребёнка+4×allpass/канал)",
                 rv_where, (int)(rv_n * sizeof(float) / 1024));
    } else {
        fx_reverb_init(&s_fx, nullptr, 0);           // reverb отключён
        ESP_LOGE(TAG, "FX reverb: не выделить %d КБ — reverb отключён", (int)(rv_n * sizeof(float) / 1024));
    }

    // Нотная очередь Core 1 → Core 0. Глубина с запасом (шквал нот дренится за блок ~1.3 мс).
    // Проверяем NULL: audio_task зовёт xQueueReceive безусловно, и на NULL это configASSERT внутри
    // FreeRTOS — цикл ребутов с бэктрейсом, указывающим куда угодно, кроме провалившегося
    // выделения. Внутренняя DRAM тут уже плотная: реверб забрал ~110 КБ выше.
    s_note_q = xQueueCreate(32, sizeof(NoteEvent));
    if (!s_note_q) {
        ESP_LOGE(TAG, "не выделить нотную очередь — аудио-задача не запущена, звука не будет");
        return;
    }
    // Очередь обновлений паттерна (этап 7.4): глубина 40 — хватает на весь паттерн (16 шагов) + правки.
    s_step_q = xQueueCreate(40, sizeof(StepUpdate));
    if (!s_step_q) {
        ESP_LOGE(TAG, "не выделить очередь паттерна — правки секвенсора из GUI не пройдут");
        // не фатально: звук и ноты работают, паттерн просто не редактируется по проводу
    }

    // 3) Аудио-задача на Core 0 (Core 1 отдан comm/UI). Приоритет выше comm(5): звук важнее.
    //    Стек 6 КБ: SynthParams + float-буферы на стеке (пул голосов — статический, не тут).
    if (xTaskCreatePinnedToCore(audio_task, "audio", 6144, nullptr, 10, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "не создать аудио-задачу — звука не будет");
        return;
    }

    ESP_LOGI(TAG, "I2S TX %u Гц/16 бит, BCK=%d WS=%d DIN=%d — полифония 3.5/3.6 (до 8 голосов + glide)",
             (unsigned)SAMPLE_RATE, (int)PIN_BCK, (int)PIN_WS, (int)PIN_DIN);
}
