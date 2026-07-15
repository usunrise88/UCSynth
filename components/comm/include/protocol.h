// Логика бинарного протокола — чистая, без ESP-IDF, host-тестируема.
// Опкоды/раскладки тел см. docs/serial-protocol.md (там же — для реализации GUI).
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Опкоды тела кадра. ПК → МК (запросы):
enum {
    CMD_SET      = 0x01,  // [id:u16][val:f32]
    CMD_GET      = 0x02,  // [id:u16]
    CMD_LIST     = 0x03,  // (без аргументов)
    CMD_NOTE_ON  = 0x04,  // [note:u8][vel:u8]
    CMD_NOTE_OFF = 0x05,  // [note:u8]
    CMD_STAT     = 0x06,  // (без аргументов)
};
// МК → ПК (ответы):
enum {
    RSP_ACK     = 0x80,  // (без аргументов)
    RSP_VALUE   = 0x81,  // [id:u16][val:f32]
    RSP_PARAM   = 0x82,  // [id:u16][type:u8][min:f32][max:f32][def:f32][cur:f32][namelen:u8][name]
    RSP_LISTEND = 0x83,  // [count:u16]
    RSP_STAT    = 0x86,  // [heap:u32][minheap:u32][uptime_ms:u32][cpu_permille:u32][underruns:u32]
    RSP_ERR     = 0xFF,  // [code:u8]
};
// Коды ошибок (тело RSP_ERR):
enum {
    ERR_UNKNOWN_CMD = 1,
    ERR_BAD_ID      = 2,
    ERR_BAD_LEN     = 3,
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

// Разобрать тело запроса, выдать ответ(ы) через emit. Значения — из control.
void comm_handle_request(const uint8_t *body, size_t body_len,
                         const sys_stats_t *stats,
                         comm_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif
