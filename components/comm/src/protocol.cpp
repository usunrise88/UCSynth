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

}  // namespace

void comm_handle_request(const uint8_t *body, size_t body_len,
                         const sys_stats_t *stats,
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
    default:
        emit_err(emit, ctx, ERR_UNKNOWN_CMD);
        return;
    }
}
