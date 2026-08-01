// preset_codec — чистая (де)сериализация снимка реестра. Без ESP-IDF → host-тестируем.
#include "preset.h"
#include "control.h"

#include <string.h>

namespace {

// Транзиентные параметры — отладочный тест-тон: в пресет НЕ входят и при загрузке НЕ сбрасываются
// (иначе загрузка чужого пресета могла бы дёрнуть отладочный тон поверх звука). Источник id — реестр.
inline bool is_transient(uint16_t id) { return id == PARAM_TEST_TONE; }

void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put_f32(uint8_t *p, float f) {
    uint32_t u; memcpy(&u, &f, 4);
    p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); p[3] = (uint8_t)(u >> 24);
}
uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
float get_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f; memcpy(&f, &u, 4); return f;
}

}  // namespace

size_t preset_serialize(uint8_t *buf, size_t cap, const char *path)
{
    const size_t plen = path ? strlen(path) : 0;
    if (plen > PRESET_PATH_MAX) return 0;

    const uint16_t n = param_count();
    const size_t need = 1 + 1 + plen + 2 + (size_t)n * 6;   // верхняя оценка (транзиентные вычтутся)
    if (need > cap) return 0;

    size_t o = 0;
    buf[o++] = PRESET_FMT_VERSION;
    buf[o++] = (uint8_t)plen;
    memcpy(buf + o, path, plen); o += plen;
    const size_t count_off = o; o += 2;   // count заполним после цикла

    uint16_t count = 0;
    for (uint16_t id = 0; id < n; ++id) {
        if (is_transient(id)) continue;
        param_info_t info;
        if (!param_get_info(id, &info)) continue;
        put_u16(buf + o, id);       o += 2;
        put_f32(buf + o, info.cur); o += 4;
        ++count;
    }
    put_u16(buf + count_off, count);
    return o;
}

bool preset_apply(const uint8_t *buf, size_t len)
{
    if (len < 2) return false;
    if (buf[0] == 0) return false;                     // version 0 — битый
    const uint8_t plen = buf[1];
    size_t o = (size_t)2 + plen;
    if (o + 2 > len) return false;                     // нет места под count
    const uint16_t count = get_u16(buf + o); o += 2;
    if (o + (size_t)count * 6 > len) return false;     // тело короче заявленного count

    // Сброс всех параметров в дефолт (кроме транзиентных) → пресет = полный звук, а не дельта.
    const uint16_t n = param_count();
    for (uint16_t id = 0; id < n; ++id) {
        if (is_transient(id)) continue;
        param_info_t info;
        if (param_get_info(id, &info)) set_param(id, info.def);
    }
    // Применить пары. set_param сам клампит/квантует и игнорирует id >= param_count (старый блоб в
    // новой прошивке / наоборот).
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t id = get_u16(buf + o); o += 2;
        const float    v  = get_f32(buf + o); o += 4;
        set_param(id, v);
    }
    return true;
}

bool preset_read_path(const uint8_t *buf, size_t len, char *path_out, size_t path_cap)
{
    if (len < 2 || path_cap == 0) return false;
    if (buf[0] == 0) return false;
    const uint8_t plen = buf[1];
    if ((size_t)2 + plen > len) return false;
    size_t c = plen;
    if (c > path_cap - 1) c = path_cap - 1;
    memcpy(path_out, buf + 2, c);
    path_out[c] = '\0';
    return true;
}
