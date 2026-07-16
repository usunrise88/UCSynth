// control — единая модель параметров (ядро архитектуры).
// ВСЕ источники управления (GUI/Serial, энкодеры, тач) пишут ТОЛЬКО сюда через
// set_param/get_param, никогда напрямую в DSP. Это позволяет добавлять контролы
// позже без переписывания движка.
//
// Потокобезопасность: значения — atomic<float>, пишет Core 1 (контролы/Serial),
// читает Core 0 (аудио), без мьютекса. Метаданные (имя/тип/диапазон) — const, живут
// во flash, после старта не меняются → читаются без синхронизации.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Тип параметра: подсказывает GUI, каким контролом рисовать, и как трактовать float.
typedef enum {
    PARAM_TYPE_FLOAT = 0,  // непрерывный
    PARAM_TYPE_INT   = 1,  // целый (значение округляется)
    PARAM_TYPE_ENUM  = 2,  // выбор из списка (значение = индекс варианта)
    PARAM_TYPE_BOOL  = 3,  // 0/1
} param_type_t;

// Стабильные id. Их знают протокол/GUI/пресеты — НЕ переупорядочивать, только
// дописывать в конец. Каждый будущий этап регистрирует свои параметры.
typedef enum {
    PARAM_MASTER_VOLUME = 0,  // общая громкость, 0..1
    PARAM_TEST_TONE_HZ  = 1,  // частота отладочного тест-тона, Гц
    PARAM_WAVEFORM      = 2,  // форма волны осц1 (enum): 0 sine, 1 saw, 2 square, 3 tri (этап 1.3)
    PARAM_TEST_TONE     = 3,  // отладочный тест-тон вкл/вык (bool): 1 — непрерывный тон, перебивает
                              //   ноты (проверка тракта); 0 — играют ноты NOTE_ON/OFF (этап 3.0)
    // --- этап 3.1: ADSR (VCA) + drone ---
    PARAM_AMP_ATTACK    = 4,  // атака VCA, с
    PARAM_AMP_DECAY     = 5,  // спад VCA, с
    PARAM_AMP_SUSTAIN   = 6,  // сустейн VCA, уровень 0..1
    PARAM_AMP_RELEASE   = 7,  // релиз VCA, с
    PARAM_LATCH         = 8,  // дрон-защёлка (bool): note-on держит gate до снятия latch/новой ноты
    PARAM_AMP_LOOP      = 9,  // зацикливание VCA-огибающей A→D→A (bool): эволюция дрона
    // --- этап 3.3: три осц + микшер (осц1 форма = PARAM_WAVEFORM) ---
    PARAM_OSC1_LEVEL    = 10, // уровень осц1 в микшере 0..1
    PARAM_OSC1_DETUNE   = 11, // детюн осц1, полутоны (дробные = центы)
    PARAM_OSC2_WAVE     = 12, // форма осц2 (enum 0..3)
    PARAM_OSC2_LEVEL    = 13, // уровень осц2 0..1
    PARAM_OSC2_DETUNE   = 14, // детюн осц2, полутоны
    PARAM_OSC3_WAVE     = 15, // форма осц3 (enum 0..3)
    PARAM_OSC3_LEVEL    = 16, // уровень осц3 0..1
    PARAM_OSC3_DETUNE   = 17, // детюн осц3, полутоны
    PARAM_NOISE_LEVEL   = 18, // уровень шума в микшере 0..1
    PARAM_RING_LEVEL    = 19, // уровень ring mod (осц1×осц2) 0..1
    // --- этап 3.2: фильтр (ZDF SVF) + ADSR (VCF) ---
    PARAM_CUTOFF        = 20, // частота среза фильтра, Гц
    PARAM_RESONANCE     = 21, // резонанс 0..1 (1 ≈ самовозбуждение)
    PARAM_FILTER_MODE   = 22, // режим (enum): 0 LP, 1 HP, 2 BP, 3 OFF (сквозной)
    PARAM_FLT_ATTACK    = 23, // атака VCF, с
    PARAM_FLT_DECAY     = 24, // спад VCF, с
    PARAM_FLT_SUSTAIN   = 25, // сустейн VCF, 0..1
    PARAM_FLT_RELEASE   = 26, // релиз VCF, с
    PARAM_FLT_ENV_AMT   = 27, // глубина VCF→cutoff, -1..1 (октавы, ±6 на краях)
    PARAM_FLT_LOOP      = 28, // зацикливание VCF-огибающей (bool)
    // --- этап 3.4: lo-fi ---
    PARAM_LOFI          = 29, // lo-fi вкл/вык (bool): bit-crush + без band-limit (алиасинг)
    PARAM_LOFI_BITS     = 30, // разрядность bit-crush, 1..16 (16 = прозрачно)
    // --- этап 3.5/3.6: полифония + glide ---
    PARAM_POLY_VOICES   = 31, // число голосов (int 1..8): 1 = моно со стеком нот
    PARAM_GLIDE_TIME    = 32, // портаменто: время скольжения высоты, с (0 = мгновенно)
    PARAM_LEGATO        = 33, // моно-легато (bool): перекрытие нот не ретригерит огибающую
    PARAM_COUNT
} param_id_t;

// Снимок метаданных + текущего значения — для LIST/интроспекции.
typedef struct {
    const char  *name;
    param_type_t type;
    float        min;
    float        max;
    float        def;
    float        cur;
} param_info_t;

// Инициализация реестра: значения := дефолты. Один раз на старте (Core 1).
void control_init(void);

// Число параметров в реестре.
uint16_t param_count(void);

// Запись (Core 1): клампит в [min,max], дискретные типы округляет. Возвращает реально
// сохранённое значение (после клампа) — удобно эхо-ответом протокола. NAN при неверном id.
float set_param(uint16_t id, float value);

// Чтение (потокобезопасно, в т.ч. с Core 0). NAN при неверном id.
float get_param(uint16_t id);

// Метаданные + текущее значение. false при неверном id или out == NULL.
bool param_get_info(uint16_t id, param_info_t *out);

#ifdef __cplusplus
}
#endif
