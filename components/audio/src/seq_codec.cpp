// seq_codec — (де)сериализация паттерна. Чистый, host-тестируем. Пошаговый кодек (для провода) +
// пообёртка на весь паттерн (для NVS).
#include "seq_codec.h"

#include <cstring>

namespace {

void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put_f32(uint8_t *p, float f) { uint32_t u; std::memcpy(&u, &f, 4); p[0]=(uint8_t)u; p[1]=(uint8_t)(u>>8); p[2]=(uint8_t)(u>>16); p[3]=(uint8_t)(u>>24); }
uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
float get_f32(const uint8_t *p) { uint32_t u=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); float f; std::memcpy(&f,&u,4); return f; }

}  // namespace

size_t seq_step_serialize(uint8_t *buf, size_t cap, const SeqStep *st)
{
    const int nn = st->n_notes  > SEQ_MAX_NOTES  ? SEQ_MAX_NOTES  : st->n_notes;
    const int np = st->n_plocks > SEQ_MAX_PLOCKS ? SEQ_MAX_PLOCKS : st->n_plocks;
    const size_t need = 1 + 1 + (size_t)nn + 1 + 1 + 1 + (size_t)np * 6;
    if (need > cap) return 0;
    size_t o = 0;
    buf[o++] = st->active ? 1 : 0;
    buf[o++] = (uint8_t)nn;
    for (int i = 0; i < nn; ++i) buf[o++] = st->notes[i];
    buf[o++] = st->velocity;
    int tq = (int)(st->trig_prob * 255.0f + 0.5f);
    if (tq < 0) tq = 0; else if (tq > 255) tq = 255;
    buf[o++] = (uint8_t)tq;
    buf[o++] = (uint8_t)np;
    for (int i = 0; i < np; ++i) { put_u16(buf + o, st->plocks[i].id); o += 2; put_f32(buf + o, st->plocks[i].val); o += 4; }
    return o;
}

size_t seq_step_deserialize(const uint8_t *buf, size_t len, SeqStep *st)
{
    *st = SeqStep{};
    if (len < 2) return 0;
    size_t o = 0;
    st->active = buf[o++] != 0;
    const int nn_read = buf[o++];
    if (o + (size_t)nn_read > len) return 0;
    const int nn = nn_read > SEQ_MAX_NOTES ? SEQ_MAX_NOTES : nn_read;
    st->n_notes = (uint8_t)nn;
    for (int i = 0; i < nn_read; ++i) { if (i < nn) st->notes[i] = buf[o]; o++; }
    if (o + 3 > len) return 0;
    st->velocity  = buf[o++];
    st->trig_prob = (float)buf[o++] * (1.0f / 255.0f);
    const int np_read = buf[o++];
    if (o + (size_t)np_read * 6 > len) return 0;
    const int np = np_read > SEQ_MAX_PLOCKS ? SEQ_MAX_PLOCKS : np_read;
    st->n_plocks = (uint8_t)np;
    for (int i = 0; i < np_read; ++i) {
        const uint16_t id = get_u16(buf + o); o += 2;
        const float    v  = get_f32(buf + o); o += 4;
        if (i < np) { st->plocks[i].id = id; st->plocks[i].val = v; }
    }
    return o;
}

size_t seq_serialize(uint8_t *buf, size_t cap, const SeqStep steps[SEQ_STEPS])
{
    if (cap < 1) return 0;
    size_t o = 0;
    buf[o++] = SEQ_FMT_VERSION;
    for (int s = 0; s < SEQ_STEPS; ++s) {
        const size_t n = seq_step_serialize(buf + o, cap - o, &steps[s]);
        if (n == 0) return 0;
        o += n;
    }
    return o;
}

bool seq_deserialize(const uint8_t *buf, size_t len, SeqStep steps[SEQ_STEPS])
{
    for (int s = 0; s < SEQ_STEPS; ++s) steps[s] = SeqStep{};
    if (len < 1 || buf[0] == 0) return false;
    size_t o = 1;
    for (int s = 0; s < SEQ_STEPS; ++s) {
        const size_t consumed = seq_step_deserialize(buf + o, len - o, &steps[s]);
        if (consumed == 0) return false;
        o += consumed;
    }
    return true;
}
