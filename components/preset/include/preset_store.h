// preset_store — хранилище пресетов в NVS (раздел "presets"). Только прошивка (тянет nvs_flash).
// Кодек блоба — в preset.h (чистый). Дерево пресетов = пути ВНУТРИ блоба; ключ NVS — короткий числовой
// слот "p<id>" (лимит имени ключа NVS — 15 символов, длинный путь "Папка/Имя" туда не влез бы).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Открыть namespace в разделе "presets", прочитать/создать мету (монотонный счётчик слотов). NVS-раздел
// должен быть уже проинициализирован (nvs_flash_init_partition("presets") в буте). Идемпотентно.
esp_err_t preset_store_init(void);

// Снять снимок текущего реестра в слот. slot==PRESET_SLOT_NEW → выделить следующий; *out_slot ← слот.
esp_err_t preset_store_save(uint16_t slot, const char *path, uint16_t *out_slot);
// Загрузить пресет из слота и применить к реестру. ESP_ERR_NVS_NOT_FOUND — слот пуст.
esp_err_t preset_store_load(uint16_t slot);
// Удалить слот. ESP_ERR_NVS_NOT_FOUND — слота нет.
esp_err_t preset_store_delete(uint16_t slot);
// Сменить путь слота (значения НЕ трогаются — только относка в дереве).
esp_err_t preset_store_rename(uint16_t slot, const char *path);

// Обойти все пресеты: cb(ctx, slot, path) на каждый существующий слот.
typedef void (*preset_iter_fn)(void *ctx, uint16_t slot, const char *path);
esp_err_t preset_store_list(preset_iter_fn cb, void *ctx);

#ifdef __cplusplus
}
#endif
