// seq_store — NVS-хранилище паттернов секвенсора (этап 7). Namespace "patterns" в разделе "presets"
// (тот же раздел, что и пресеты — отдельный namespace; nvs_flash_init_partition уже сделан в буте).
// Хранит СЫРОЙ блоб паттерна (кодек — seq_codec, на стороне вызова). Только прошивка (тянет nvs_flash).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_SLOT_NEW  0xFFFFu   // slot для save = «новый»
#define SEQ_PATH_MAX  96        // макс. байт пути (= PRESET_PATH_MAX; wire-кап в protocol.h)

// Открыть namespace "patterns", прочитать/создать мету (счётчик слотов). Идемпотентно.
esp_err_t seq_store_init(void);

// Сохранить блоб паттерна в слот (slot==SEQ_SLOT_NEW → выделить). *out_slot ← слот.
esp_err_t seq_store_save(uint16_t slot, const char *path, const uint8_t *blob, size_t len, uint16_t *out_slot);
// Прочитать блоб паттерна из слота в blob (до cap). *out_len ← длина. NOT_FOUND — слот пуст.
esp_err_t seq_store_load(uint16_t slot, uint8_t *blob, size_t cap, size_t *out_len);
// Удалить слот.
esp_err_t seq_store_delete(uint16_t slot);
// Сменить путь слота (блоб паттерна не трогается).
esp_err_t seq_store_rename(uint16_t slot, const char *path);

typedef void (*seq_iter_fn)(void *ctx, uint16_t slot, const char *path);
esp_err_t seq_store_list(seq_iter_fn cb, void *ctx);

#ifdef __cplusplus
}
#endif
