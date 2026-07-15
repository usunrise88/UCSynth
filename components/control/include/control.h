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
    PARAM_TEST_TONE_HZ  = 1,  // частота осциллятора, Гц
    PARAM_WAVEFORM      = 2,  // форма волны (enum): 0 sine, 1 saw, 2 square, 3 tri (этап 1.3)
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
