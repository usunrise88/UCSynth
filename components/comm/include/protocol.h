// Логика бинарного протокола — чистая, без ESP-IDF, host-тестируема.
// Опкоды/раскладки тел см. docs/serial-protocol.md (там же — для реализации GUI).
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Максимум байт пути пресета «Папка/Имя» на проводе. ДОЛЖЕН совпадать с PRESET_PATH_MAX в preset.h.
#ifndef PRESET_PATH_MAX
#define PRESET_PATH_MAX 96
#endif

// Опкоды тела кадра. ПК → МК (запросы):
enum {
    CMD_SET      = 0x01,  // [id:u16][val:f32]
    CMD_GET      = 0x02,  // [id:u16]
    CMD_LIST     = 0x03,  // (без аргументов)
    CMD_NOTE_ON  = 0x04,  // [note:u8][vel:u8]
    CMD_NOTE_OFF = 0x05,  // [note:u8]
    CMD_STAT     = 0x06,  // (без аргументов)
    // --- пресеты (этап 6) ---
    CMD_PRESET_LIST   = 0x07,  // (без аргументов) → RSP_PRESET×N + RSP_PRESET_END
    CMD_PRESET_SAVE   = 0x08,  // [slot:u16 (0xFFFF=новый)][path_len:u8][path] → RSP_PRESET_SAVED / RSP_ERR
    CMD_PRESET_LOAD   = 0x09,  // [slot:u16] → RSP_ACK / RSP_ERR
    CMD_PRESET_DELETE = 0x0A,  // [slot:u16] → RSP_ACK / RSP_ERR
    CMD_PRESET_RENAME = 0x0B,  // [slot:u16][path_len:u8][path] → RSP_ACK / RSP_ERR
};
// МК → ПК (ответы):
enum {
    RSP_ACK          = 0x80,  // (без аргументов)
    RSP_VALUE        = 0x81,  // [id:u16][val:f32]
    RSP_PARAM        = 0x82,  // [id:u16][type:u8][min:f32][max:f32][def:f32][cur:f32][namelen:u8][name]
    RSP_LISTEND      = 0x83,  // [count:u16]
    RSP_PRESET       = 0x84,  // [slot:u16][path_len:u8][path]  (по одному на пресет в ответ на LIST)
    RSP_PRESET_END   = 0x85,  // [count:u16]
    RSP_STAT         = 0x86,  // [heap:u32][minheap:u32][uptime_ms:u32][cpu_permille:u32][underruns:u32]
    RSP_PRESET_SAVED = 0x87,  // [slot:u16]  (назначенный слот, в ответ на SAVE)
    RSP_ERR          = 0xFF,  // [code:u8]
};
// Коды ошибок (тело RSP_ERR):
enum {
    ERR_UNKNOWN_CMD = 1,
    ERR_BAD_ID      = 2,
    ERR_BAD_LEN     = 3,
    ERR_NO_PRESET   = 4,  // слот пуст / не найден
    ERR_STORAGE     = 5,  // сбой NVS
};

// Метрики для STAT. Заполняет транспорт (ESP-IDF); чистая логика от них не зависит.
typedef struct {
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t uptime_ms;
    uint32_t cpu_permille;   // загрузка аудио-задачи, ‰ бюджета блока (1000 = впритык к realtime)
    uint32_t underruns;      // блоки, не уложившиеся в realtime (прокси underrun)
} sys_stats_t;

// Колбэк выдачи ответного тела. Транспорт оборачивает его в кадр и шлёт; тест — копит.
// Один запрос может дать НЕСКОЛЬКО ответов (LIST → PARAM×N + LISTEND).
typedef void (*comm_emit_fn)(void *ctx, const uint8_t *resp_body, size_t resp_len);

// Колбэк листинга пресетов: бэкенд зовёт на КАЖДЫЙ пресет (диспетчер шлёт RSP_PRESET).
typedef void (*preset_list_emit_fn)(void *entry_ctx, uint16_t slot, const char *path);

// Бэкенд хранилища пресетов — инъекция NVS-стороны в ЧИСТЫЙ диспетчер (тем же приёмом, что comm_emit_fn
// инъектит транспорт → protocol.cpp остаётся без ESP-IDF и host-тестируем). В прошивке — обёртка над
// preset_store (comm.cpp), в тесте — in-memory. Методы save/load/del/rename: 0 = ok, иначе код RSP_ERR
// (ERR_NO_PRESET/ERR_STORAGE). list: ≥0 = число пресетов, <0 = ошибка.
typedef struct {
    int (*save)(void *ctx, uint16_t slot, const char *path, uint16_t *out_slot);
    int (*load)(void *ctx, uint16_t slot);
    int (*del)(void *ctx, uint16_t slot);
    int (*rename)(void *ctx, uint16_t slot, const char *path);
    int (*list)(void *ctx, preset_list_emit_fn emit_entry, void *entry_ctx);
    void *ctx;
} preset_backend_t;

// Разобрать тело запроса, выдать ответ(ы) через emit. Значения — из control; пресеты — через presets
// (может быть nullptr → preset-опкоды вернут ERR_UNKNOWN_CMD).
void comm_handle_request(const uint8_t *body, size_t body_len,
                         const sys_stats_t *stats,
                         const preset_backend_t *presets,
                         comm_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
