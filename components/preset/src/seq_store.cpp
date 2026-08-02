// seq_store — NVS-хранилище паттернов (этап 7). По образцу preset_store, но хранит СЫРОЙ блоб паттерна
// (кодек — на стороне вызова). Namespace "patterns" в разделе "presets". Только прошивка.
//
// NVS-запись слота = [u8 path_len | char path | u8 pattern_blob...]: путь и блоб вместе (ключ NVS —
// короткий числовой слот "q<id>", лимит имени 15). Мета "meta" — монотонный next_id. Листинг — probe.
#include "seq_store.h"

#include "nvs.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG  = "seq_store";
static const char *PART = "presets";    // раздел (уже проинициализирован в буте)
static const char *NS   = "patterns";   // отдельный namespace для паттернов
static const char *META = "meta";

static nvs_handle_t s_h = 0;
static bool s_ready = false;

struct Meta { uint8_t version; uint16_t next_id; };

static void slot_key(uint16_t slot, char *out /*[8]*/) { snprintf(out, 8, "q%u", (unsigned)slot); }

static void read_meta(Meta *m)
{
    size_t sz = sizeof(*m);
    if (nvs_get_blob(s_h, META, m, &sz) != ESP_OK || sz != sizeof(*m)) { m->version = 1; m->next_id = 0; }
}

esp_err_t seq_store_init(void)
{
    if (s_ready) return ESP_OK;
    esp_err_t err = nvs_open_from_partition(PART, NS, NVS_READWRITE, &s_h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open(%s/%s): %s", PART, NS, esp_err_to_name(err)); return err; }
    Meta m; size_t sz = sizeof(m);
    err = nvs_get_blob(s_h, META, &m, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND || sz != sizeof(m)) {
        m.version = 1; m.next_id = 0;
        nvs_set_blob(s_h, META, &m, sizeof(m));
        nvs_commit(s_h);
    }
    s_ready = true;
    ESP_LOGI(TAG, "паттерны: NVS namespace '%s', следующий слот %u", NS, (unsigned)m.next_id);
    return ESP_OK;
}

esp_err_t seq_store_save(uint16_t slot, const char *path, const uint8_t *blob, size_t len, uint16_t *out_slot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const size_t plen = path ? strlen(path) : 0;
    if (plen > SEQ_PATH_MAX) return ESP_ERR_INVALID_SIZE;

    static uint8_t rec[1 + SEQ_PATH_MAX + 1024];   // static: единственный писатель — RX-задача comm
    if (1 + plen + len > sizeof(rec)) return ESP_ERR_INVALID_SIZE;
    size_t o = 0;
    rec[o++] = (uint8_t)plen;
    memcpy(rec + o, path, plen); o += plen;
    memcpy(rec + o, blob, len);  o += len;

    Meta m; read_meta(&m);
    const uint16_t use = (slot == SEQ_SLOT_NEW) ? m.next_id : slot;
    char key[8]; slot_key(use, key);
    esp_err_t err = nvs_set_blob(s_h, key, rec, o);
    if (err != ESP_OK) return err;
    if (slot == SEQ_SLOT_NEW) { m.next_id = (uint16_t)(use + 1); nvs_set_blob(s_h, META, &m, sizeof(m)); }
    err = nvs_commit(s_h);
    if (err != ESP_OK) return err;
    if (out_slot) *out_slot = use;
    return ESP_OK;
}

esp_err_t seq_store_load(uint16_t slot, uint8_t *blob, size_t cap, size_t *out_len)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    static uint8_t rec[1 + SEQ_PATH_MAX + 1024];
    size_t rlen = sizeof(rec);
    esp_err_t err = nvs_get_blob(s_h, key, rec, &rlen);
    if (err != ESP_OK) return err;
    if (rlen < 1) return ESP_ERR_INVALID_ARG;
    const size_t plen = rec[0];
    if (1 + plen > rlen) return ESP_ERR_INVALID_ARG;
    const size_t blen = rlen - 1 - plen;
    if (blen > cap) return ESP_ERR_INVALID_SIZE;
    memcpy(blob, rec + 1 + plen, blen);
    if (out_len) *out_len = blen;
    return ESP_OK;
}

esp_err_t seq_store_delete(uint16_t slot)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    esp_err_t err = nvs_erase_key(s_h, key);
    if (err != ESP_OK) return err;
    return nvs_commit(s_h);
}

esp_err_t seq_store_rename(uint16_t slot, const char *path)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    char key[8]; slot_key(slot, key);
    static uint8_t rec[1 + SEQ_PATH_MAX + 1024];
    size_t rlen = sizeof(rec);
    esp_err_t err = nvs_get_blob(s_h, key, rec, &rlen);
    if (err != ESP_OK) return err;
    if (rlen < 1) return ESP_ERR_INVALID_ARG;
    const size_t old_plen = rec[0];
    if (1 + old_plen > rlen) return ESP_ERR_INVALID_ARG;
    const size_t blen = rlen - 1 - old_plen;
    const size_t new_plen = path ? strlen(path) : 0;
    if (new_plen > SEQ_PATH_MAX) return ESP_ERR_INVALID_SIZE;

    static uint8_t out[1 + SEQ_PATH_MAX + 1024];
    if (1 + new_plen + blen > sizeof(out)) return ESP_ERR_INVALID_SIZE;
    size_t o = 0;
    out[o++] = (uint8_t)new_plen;
    memcpy(out + o, path, new_plen); o += new_plen;
    memcpy(out + o, rec + 1 + old_plen, blen); o += blen;   // сохранить блоб паттерна
    err = nvs_set_blob(s_h, key, out, o);
    if (err != ESP_OK) return err;
    return nvs_commit(s_h);
}

esp_err_t seq_store_list(seq_iter_fn cb, void *ctx)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    Meta m; read_meta(&m);
    static uint8_t rec[1 + SEQ_PATH_MAX + 1024];
    char path[SEQ_PATH_MAX + 1];
    for (uint16_t slot = 0; slot < m.next_id; ++slot) {
        char key[8]; slot_key(slot, key);
        size_t rlen = sizeof(rec);
        if (nvs_get_blob(s_h, key, rec, &rlen) != ESP_OK || rlen < 1) continue;
        size_t plen = rec[0];
        if (1 + plen > rlen) continue;
        if (plen > SEQ_PATH_MAX) plen = SEQ_PATH_MAX;
        memcpy(path, rec + 1, plen); path[plen] = '\0';
        cb(ctx, slot, path);
    }
    return ESP_OK;
}
