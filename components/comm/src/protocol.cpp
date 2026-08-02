#include "protocol.h"
#include "control.h"

#include <string.h>

namespace {

// Сериализация little-endian (S3 и x86 — LE; memcpy для float портируем и без UB).
void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void put_f32(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); put_u32(p, u); }
uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
float get_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f; memcpy(&f, &u, 4); return f;
}

void emit_err(comm_emit_fn emit, void *ctx, uint8_t code) {
    const uint8_t b[2] = { (uint8_t)RSP_ERR, code };
    emit(ctx, b, sizeof(b));
}
void emit_value(comm_emit_fn emit, void *ctx, uint16_t id, float v) {
    uint8_t b[7];
    b[0] = RSP_VALUE;
    put_u16(b + 1, id);
    put_f32(b + 3, v);
    emit(ctx, b, sizeof(b));
}
void emit_ack(comm_emit_fn emit, void *ctx) {
    const uint8_t ack = RSP_ACK;
    emit(ctx, &ack, 1);
}

// Колбэк, который бэкенд листинга зовёт на каждый пресет → собираем RSP_PRESET [slot][path_len][path].
struct PresetListCtx { comm_emit_fn emit; void *ctx; };
void preset_list_emit(void *entry_ctx, uint16_t slot, const char *path) {
    PresetListCtx *lc = static_cast<PresetListCtx *>(entry_ctx);
    size_t plen = path ? strlen(path) : 0;
    if (plen > PRESET_PATH_MAX) plen = PRESET_PATH_MAX;
    uint8_t b[1 + 2 + 1 + PRESET_PATH_MAX];
    size_t o = 0;
    b[o++] = RSP_PRESET;
    put_u16(b + o, slot); o += 2;
    b[o++] = (uint8_t)plen;
    memcpy(b + o, path, plen); o += plen;
    lc->emit(lc->ctx, b, o);
}

// То же для листинга паттернов, но опкод RSP_SEQ_ENTRY (тот же PresetListCtx-мостик).
void seq_list_emit(void *entry_ctx, uint16_t slot, const char *path) {
    PresetListCtx *lc = static_cast<PresetListCtx *>(entry_ctx);
    size_t plen = path ? strlen(path) : 0;
    if (plen > PRESET_PATH_MAX) plen = PRESET_PATH_MAX;
    uint8_t b[1 + 2 + 1 + PRESET_PATH_MAX];
    size_t o = 0;
    b[o++] = RSP_SEQ_ENTRY;
    put_u16(b + o, slot); o += 2;
    b[o++] = (uint8_t)plen;
    memcpy(b + o, path, plen); o += plen;
    lc->emit(lc->ctx, b, o);
}

// Разобрать [slot:u16][path_len:u8][path] из тела запроса. false → ошибка длины.
bool parse_slot_path(const uint8_t *body, size_t body_len, uint16_t *slot,
                     char *path_out /*[PRESET_PATH_MAX+1]*/) {
    if (body_len < 4) return false;
    *slot = get_u16(body + 1);
    const uint8_t plen = body[3];
    if (body_len < (size_t)4 + plen || plen > PRESET_PATH_MAX) return false;
    memcpy(path_out, body + 4, plen);
    path_out[plen] = '\0';
    return true;
}

}  // namespace

void comm_handle_request(const uint8_t *body, size_t body_len,
                         const sys_stats_t *stats,
                         const preset_backend_t *presets,
                         const seq_backend_t *seq,
                         comm_emit_fn emit, void *ctx)
{
    if (body_len < 1) { emit_err(emit, ctx, ERR_BAD_LEN); return; }

    switch (body[0]) {
    case CMD_GET: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        const uint16_t id = get_u16(body + 1);
        if (id >= param_count()) { emit_err(emit, ctx, ERR_BAD_ID); return; }
        emit_value(emit, ctx, id, get_param(id));
        return;
    }
    case CMD_SET: {
        if (body_len < 7) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        const uint16_t id = get_u16(body + 1);
        if (id >= param_count()) { emit_err(emit, ctx, ERR_BAD_ID); return; }
        const float stored = set_param(id, get_f32(body + 3));  // клампит внутри
        emit_value(emit, ctx, id, stored);
        return;
    }
    case CMD_LIST: {
        const uint16_t n = param_count();
        for (uint16_t id = 0; id < n; ++id) {
            param_info_t info;
            if (!param_get_info(id, &info)) continue;
            uint8_t b[1 + 2 + 1 + 16 + 1 + 64];  // opcode+id+type+4×f32+namelen+name(≤64)
            size_t o = 0;
            b[o++] = RSP_PARAM;
            put_u16(b + o, id);       o += 2;
            b[o++] = (uint8_t)info.type;
            put_f32(b + o, info.min); o += 4;
            put_f32(b + o, info.max); o += 4;
            put_f32(b + o, info.def); o += 4;
            put_f32(b + o, info.cur); o += 4;
            size_t namelen = info.name ? strlen(info.name) : 0;
            if (namelen > 64) namelen = 64;
            b[o++] = (uint8_t)namelen;
            memcpy(b + o, info.name, namelen); o += namelen;
            emit(ctx, b, o);
        }
        uint8_t e[3];
        e[0] = RSP_LISTEND;
        put_u16(e + 1, n);
        emit(ctx, e, sizeof(e));
        return;
    }
    case CMD_NOTE_ON: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        const uint8_t ack = RSP_ACK;   // синтезатора ещё нет — маршрут в голоса на этапе 3
        emit(ctx, &ack, 1);
        return;
    }
    case CMD_NOTE_OFF: {
        if (body_len < 2) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        const uint8_t ack = RSP_ACK;
        emit(ctx, &ack, 1);
        return;
    }
    case CMD_STAT: {
        uint8_t b[21];
        b[0] = RSP_STAT;
        put_u32(b + 1,  stats ? stats->heap_free    : 0);
        put_u32(b + 5,  stats ? stats->heap_min     : 0);
        put_u32(b + 9,  stats ? stats->uptime_ms    : 0);
        put_u32(b + 13, stats ? stats->cpu_permille : 0);
        put_u32(b + 17, stats ? stats->underruns    : 0);
        emit(ctx, b, sizeof(b));
        return;
    }
    case CMD_PRESET_LIST: {
        if (!presets || !presets->list) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        PresetListCtx lc{ emit, ctx };
        const int count = presets->list(presets->ctx, preset_list_emit, &lc);
        if (count < 0) { emit_err(emit, ctx, ERR_STORAGE); return; }
        uint8_t e[3];
        e[0] = RSP_PRESET_END;
        put_u16(e + 1, (uint16_t)count);
        emit(ctx, e, sizeof(e));
        return;
    }
    case CMD_PRESET_SAVE: {
        uint16_t slot; char path[PRESET_PATH_MAX + 1];
        if (!parse_slot_path(body, body_len, &slot, path)) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!presets || !presets->save) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        uint16_t out = 0xFFFF;
        const int rc = presets->save(presets->ctx, slot, path, &out);
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        uint8_t b[3];
        b[0] = RSP_PRESET_SAVED;
        put_u16(b + 1, out);
        emit(ctx, b, sizeof(b));
        return;
    }
    case CMD_PRESET_LOAD: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!presets || !presets->load) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = presets->load(presets->ctx, get_u16(body + 1));
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_PRESET_DELETE: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!presets || !presets->del) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = presets->del(presets->ctx, get_u16(body + 1));
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_PRESET_RENAME: {
        uint16_t slot; char path[PRESET_PATH_MAX + 1];
        if (!parse_slot_path(body, body_len, &slot, path)) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!presets || !presets->rename) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = presets->rename(presets->ctx, slot, path);
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_SEQ_SET_STEP: {
        if (body_len < 2) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!seq || !seq->set_step) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = seq->set_step(seq->ctx, body[1], body + 2, body_len - 2);
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_SEQ_GET: {
        if (!seq || !seq->get_step) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        for (uint8_t s = 0; s < 16; ++s) {
            uint8_t b[2 + 64];
            b[0] = RSP_SEQ_STEP;
            b[1] = s;
            const size_t n = seq->get_step(seq->ctx, s, b + 2, sizeof(b) - 2);
            emit(ctx, b, 2 + n);
        }
        return;
    }
    case CMD_SEQ_SAVE: {
        uint16_t slot; char path[PRESET_PATH_MAX + 1];
        if (!parse_slot_path(body, body_len, &slot, path)) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!seq || !seq->save) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        uint16_t out = 0xFFFF;
        const int rc = seq->save(seq->ctx, slot, path, &out);
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        uint8_t b[3]; b[0] = RSP_SEQ_SAVED; put_u16(b + 1, out); emit(ctx, b, sizeof(b));
        return;
    }
    case CMD_SEQ_LOAD: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!seq || !seq->load) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = seq->load(seq->ctx, get_u16(body + 1));
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_SEQ_DELETE: {
        if (body_len < 3) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!seq || !seq->del) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = seq->del(seq->ctx, get_u16(body + 1));
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    case CMD_SEQ_LIST: {
        if (!seq || !seq->list) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        PresetListCtx lc{ emit, ctx };
        // list переиспользует preset_list_emit, но со своим RSP-опкодом → отдельный колбэк ниже.
        const int count = seq->list(seq->ctx, seq_list_emit, &lc);
        if (count < 0) { emit_err(emit, ctx, ERR_STORAGE); return; }
        uint8_t e[3]; e[0] = RSP_SEQ_END; put_u16(e + 1, (uint16_t)count); emit(ctx, e, sizeof(e));
        return;
    }
    case CMD_SEQ_RENAME: {
        uint16_t slot; char path[PRESET_PATH_MAX + 1];
        if (!parse_slot_path(body, body_len, &slot, path)) { emit_err(emit, ctx, ERR_BAD_LEN); return; }
        if (!seq || !seq->rename) { emit_err(emit, ctx, ERR_UNKNOWN_CMD); return; }
        const int rc = seq->rename(seq->ctx, slot, path);
        if (rc) { emit_err(emit, ctx, (uint8_t)rc); return; }
        emit_ack(emit, ctx);
        return;
    }
    default:
        emit_err(emit, ctx, ERR_UNKNOWN_CMD);
        return;
    }
}
