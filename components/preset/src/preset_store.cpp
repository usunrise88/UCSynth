// preset_store — NVS-хранилище пресетов. Кодек блоба — preset.h (чистый). Только прошивка.
//
// Один блоб на пресет под ключом "p<slot>" (десятичный slot ≤ 5 цифр → влезает в лимит имени NVS 15).
// Мета "meta" = монотонный счётчик next_id (slot никогда не переиспользуется после удаления → простые,
// стабильные числовые ссылки для GUI/железных контролов). Дерево — из путей ВНУТРИ блоба.
//
// Листинг — перебором слотов 0..next_id с probe nvs_get_blob (без NVS-итератора: меньше API-поверхности,
// а пресетов на «пробе пера» десятки). NVS-запись = запись flash → короткий стоп кэша обоих ядер →
// возможен микро-глитч аудио на save/delete; save/load редкие и ручные → принято (D-020).
#include "preset_store.h"
#include "preset.h"

#include "nvs.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG  = "preset";
static const char *PART = "presets";   // раздел (partition label в partitions.csv)
static const char *NS   = "presets";   // namespace внутри раздела
static const char *META = "meta";

static nvs_handle_t s_h = 0;
static bool s_ready = false;

// Мета: следующий свободный слот (монотонный).
struct Meta { uint8_t version; uint16_t next_id; };

static void slot_key(uint16_t slot, char *out /*[8]*/) { snprintf(out, 8, "p%u", (unsigned)slot); }

static void read_meta(Meta *m)
{
    size_t sz = sizeof(*m);
    if (nvs_get_blob(s_h, META, m, &sz) != ESP_OK || sz != sizeof(*m)) {
        m->version = PRESET_FMT_VERSION;
        m->next_id = 0;
    }
}

esp_err_t preset_store_init(void)
{
    if (s_ready) return ESP_OK;
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition(%s/%s): %s — пресеты недоступны", PART, NS, esp_err_to_name(err));
        return err;
    }
    Meta m;
    size_t sz = sizeof(m);
    err = nvs_get_blob(s_h, META, &m, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND || sz != sizeof(m)) {   // первый запуск — создать мету
        m.version = PRESET_FMT_VERSION;
        m.next_id = 0;
        nvs_set_blob(s_h, META, &m, sizeof(m));
        nvs_commit(s_h);
    }
    s_ready = true;
    ESP_LOGI(TAG, "пресеты: NVS раздел '%s', следующий слот %u", PART, (unsigned)m.next_id);
    return ESP_OK;
}

esp_err_t preset_store_save(uint16_t slot, const char *path, uint16_t *out_slot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    Meta m; read_meta(&m);
    const uint16_t use = (slot == PRESET_SLOT_NEW) ? m.next_id : slot;

    uint8_t blob[PRESET_BLOB_MAX];
    const size_t len = preset_serialize(blob, sizeof(blob), path ? path : "");
    if (len == 0) return ESP_ERR_INVALID_SIZE;

    char key[8]; slot_key(use, key);
    esp_err_t err = nvs_set_blob(s_h, key, blob, len);
    if (err != ESP_OK) return err;
    if (slot == PRESET_SLOT_NEW) {                       // новый слот занят → сдвинуть счётчик
        m.next_id = (uint16_t)(use + 1);
        nvs_set_blob(s_h, META, &m, sizeof(m));
    }
    err = nvs_commit(s_h);
    if (err != ESP_OK) return err;
    if (out_slot) *out_slot = use;
    return ESP_OK;
}

esp_err_t preset_store_load(uint16_t slot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    uint8_t blob[PRESET_BLOB_MAX];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(s_h, key, blob, &len);
    if (err != ESP_OK) return err;                       // NOT_FOUND → слот пуст
    if (!preset_apply(blob, len)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t preset_store_delete(uint16_t slot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    esp_err_t err = nvs_erase_key(s_h, key);
    if (err != ESP_OK) return err;
    return nvs_commit(s_h);
}

esp_err_t preset_store_rename(uint16_t slot, const char *path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    uint8_t blob[PRESET_BLOB_MAX];
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(s_h, key, blob, &len);
    if (err != ESP_OK) return err;

    // Переписать ТОЛЬКО путь: сохранить хвост {count, пары} как есть (значения не пересъёмываем).
    if (len < 2) return ESP_ERR_INVALID_ARG;
    const uint8_t old_plen = blob[1];
    if ((size_t)2 + old_plen > len) return ESP_ERR_INVALID_ARG;
    const size_t tail_off = (size_t)2 + old_plen;
    const size_t tail_len = len - tail_off;
    const size_t new_plen = path ? strlen(path) : 0;
    if (new_plen > PRESET_PATH_MAX) return ESP_ERR_INVALID_SIZE;

    uint8_t out[PRESET_BLOB_MAX];
    if (2 + new_plen + tail_len > sizeof(out)) return ESP_ERR_INVALID_SIZE;
    size_t o = 0;
    out[o++] = blob[0];                        // version
    out[o++] = (uint8_t)new_plen;
    memcpy(out + o, path, new_plen);           o += new_plen;
    memcpy(out + o, blob + tail_off, tail_len); o += tail_len;

    err = nvs_set_blob(s_h, key, out, o);
    if (err != ESP_OK) return err;
    return nvs_commit(s_h);
}

esp_err_t preset_store_list(preset_iter_fn cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    Meta m; read_meta(&m);
    uint8_t blob[PRESET_BLOB_MAX];
    char path[PRESET_PATH_MAX + 1];
    for (uint16_t slot = 0; slot < m.next_id; ++slot) {
        char key[8]; slot_key(slot, key);
        size_t len = sizeof(blob);
        if (nvs_get_blob(s_h, key, blob, &len) != ESP_OK) continue;   // удалённый/пустой слот
        if (!preset_read_path(blob, len, path, sizeof(path))) continue;
        cb(ctx, slot, path);
    }
    return ESP_OK;
}
