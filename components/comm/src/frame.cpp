#include "frame.h"

#include <string.h>

// CRC-16/CCITT-FALSE. Готовый стандартный алгоритм (не изобретаем): poly 0x1021,
// init 0xFFFF, без реверса и xorout. Битовая реализация — таблицы не держим (экономия
// RAM/flash), а частота кадров низкая, так что скорость не важна.
uint16_t frame_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t frame_encode(const uint8_t *body, size_t body_len, uint8_t *out, size_t out_cap)
{
    if (body_len > FRAME_MAX_BODY) return 0;
    const size_t total = 2 + 1 + body_len + 2;
    if (total > out_cap) return 0;

    out[0] = FRAME_SYNC0;
    out[1] = FRAME_SYNC1;
    out[2] = (uint8_t)body_len;
    memcpy(out + 3, body, body_len);
    // CRC по (LEN + BODY) — это байты out[2 .. 2+1+body_len).
    const uint16_t crc = frame_crc16(out + 2, 1 + body_len);
    out[3 + body_len] = (uint8_t)(crc & 0xFF);
    out[4 + body_len] = (uint8_t)(crc >> 8);
    return total;
}

// Состояния декодера.
enum { S_SYNC0 = 0, S_SYNC1, S_LEN, S_BODY, S_CRC_LO, S_CRC_HI };

void frame_decoder_init(frame_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    d->state = S_SYNC0;
}

bool frame_decoder_push(frame_decoder_t *d, uint8_t byte,
                        const uint8_t **body_out, size_t *body_len_out)
{
    switch (d->state) {
    case S_SYNC0:
        if (byte == FRAME_SYNC0) d->state = S_SYNC1;
        break;

    case S_SYNC1:
        if (byte == FRAME_SYNC1)      d->state = S_LEN;
        else if (byte == FRAME_SYNC0) d->state = S_SYNC1;  // 0x55 0x55… — держим синк
        else                          d->state = S_SYNC0;
        break;

    case S_LEN:
        d->len = byte;
        d->idx = 0;
        d->state = (byte == 0) ? S_CRC_LO : S_BODY;  // пустое тело допустимо (CRC решит)
        break;

    case S_BODY:
        d->body[d->idx++] = byte;
        if (d->idx >= d->len) d->state = S_CRC_LO;
        break;

    case S_CRC_LO:
        d->crc0 = byte;
        d->state = S_CRC_HI;
        break;

    case S_CRC_HI: {
        const uint16_t rx = (uint16_t)d->crc0 | ((uint16_t)byte << 8);
        // CRC по (LEN + BODY) — собираем цельным буфером: [len][body...].
        uint8_t tmp[1 + FRAME_MAX_BODY];
        tmp[0] = d->len;
        memcpy(tmp + 1, d->body, d->len);
        const uint16_t calc = frame_crc16(tmp, 1 + d->len);
        d->state = S_SYNC0;
        if (calc == rx) {
            *body_out = d->body;
            *body_len_out = d->len;
            return true;
        }
        break;  // CRC не сошёлся — молча ресинхронизируемся
    }

    default:
        d->state = S_SYNC0;
        break;
    }
    return false;
}
