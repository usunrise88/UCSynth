// preset — пресеты (этап 6): чистый кодек снимка реестра в блоб. БЕЗ ESP-IDF (только control.h) →
// host-тестируем. NVS-хранилище — отдельным заголовком preset_store.h (только прошивка).
//
// Формат блоба (little-endian):
//   u8 version | u8 path_len | char path[path_len] | u16 count | { u16 id; f32 value }[count]
// Пары — по СТАБИЛЬНОМУ id реестра (контракт control.h: «только дописывать в конец»). Транзиентные
// параметры (отладочный тест-тон) в снимок не входят и при загрузке не сбрасываются.
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Версия формата. Растёт только при НЕсовместимой смене раскладки. Дописывание параметров в реестр
// совместимо: старый блоб грузится, отсутствующих id в нём нет → они уходят в дефолт.
#define PRESET_FMT_VERSION 1
// Максимум байт пути «Папка/Имя». ДОЛЖЕН совпадать с wire-капом PRESET_PATH_MAX в protocol.h.
#ifndef PRESET_PATH_MAX
#define PRESET_PATH_MAX 96
#endif
// Верхняя граница блоба: version(1)+path_len(1)+path(96)+count(2)+пары(≤128×6). С большим запасом.
#define PRESET_BLOB_MAX 1024
// slot == PRESET_SLOT_NEW при сохранении → выделить следующий свободный (см. preset_store).
#define PRESET_SLOT_NEW 0xFFFFu

// Снять снимок текущего реестра в блоб (по get_param/param_get_info). Возврат: длина блоба, либо 0
// при нехватке cap или слишком длинном пути (> PRESET_PATH_MAX).
size_t preset_serialize(uint8_t *buf, size_t cap, const char *path);

// Применить блоб к реестру: сперва СБРОС всех параметров в дефолт (кроме транзиентных), затем set_param
// по парам (клампит/квантует/скипит неизвестный id сам). false при битом/усечённом блобе.
bool preset_apply(const uint8_t *buf, size_t len);

// Извлечь путь из блоба БЕЗ изменения реестра (для листинга). false при битом блобе.
bool preset_read_path(const uint8_t *buf, size_t len, char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif
