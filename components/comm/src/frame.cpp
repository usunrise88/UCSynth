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

void frame_decoder_init(frame_decoder_t *d)
{
    d->n = 0;
}

// Позиция первого синка в buf[from..n), либо -1.
static int index_sync(const uint8_t *b, int n, int from)
{
    for (int i = from; i + 1 < n; ++i) {
        if (b[i] == FRAME_SYNC0 && b[i + 1] == FRAME_SYNC1) return i;
    }
    return -1;
}

// Попытка собрать кадр, начинающийся в buf[off]. Возврат: полная длина кадра при валидном CRC,
// 0 при несошедшемся CRC, -1 если байт ещё не хватает.
static int try_frame_at(const frame_decoder_t *d, int off, uint8_t *len_out)
{
    if (d->n - off < 3) return -1;                       // нужны sync(2)+LEN
    const uint8_t len  = d->buf[off + 2];
    const int     need = 3 + (int)len + 2;
    if (d->n - off < need) return -1;
    // CRC считается по (LEN + BODY), а они лежат в буфере подряд — копия не нужна.
    const uint16_t calc = frame_crc16(d->buf + off + 2, 1u + (size_t)len);
    const uint16_t rx   = (uint16_t)d->buf[off + 3 + len] |
                          ((uint16_t)d->buf[off + 4 + len] << 8);
    if (calc != rx) return 0;
    *len_out = len;
    return need;
}

// Выбросить k байт с начала накопителя.
static void drop(frame_decoder_t *d, int k)
{
    if (k >= d->n) { d->n = 0; return; }
    memmove(d->buf, d->buf + k, (size_t)(d->n - k));
    d->n -= k;
}

bool frame_decoder_push(frame_decoder_t *d, uint8_t byte,
                        const uint8_t **body_out, size_t *body_len_out)
{
    if (d->n >= (int)sizeof(d->buf)) {
        drop(d, 2);   // защита: буфер полон, а кадр не собрался → там мусор, гарантируем прогресс
    }
    d->buf[d->n++] = byte;

    for (;;) {
        const int i = index_sync(d->buf, d->n, 0);
        if (i < 0) {
            // Целого синка нет. Держим только возможный его первый байт, остальное — мусор/лог.
            if (d->n > 1) drop(d, d->n - 1);
            if (d->n == 1 && d->buf[0] != FRAME_SYNC0) d->n = 0;
            break;
        }
        if (i > 0) drop(d, i);                           // отбросить лог/мусор перед синком

        uint8_t len = 0;
        const int r = try_frame_at(d, 0, &len);
        if (r > 0) {
            memcpy(d->body, d->buf + 3, len);
            drop(d, r);
            *body_out     = d->body;
            *body_len_out = len;
            return true;
        }
        if (r == 0) { drop(d, 2); continue; }             // ложный синк → рескан со сдвигом 2

        // Кадру не хватает байт. Но если ДАЛЬШЕ в буфере уже лежит целый кадр с сошедшимся CRC,
        // то текущий синк ложный и ждать его нечего: иначе готовый кадр висит до прихода байт,
        // которых может и не быть (head-of-line stall — при затихшем потоке это навсегда).
        int adv = -1;
        for (int j = index_sync(d->buf, d->n, 1); j > 0; j = index_sync(d->buf, d->n, j + 1)) {
            uint8_t l2 = 0;
            if (try_frame_at(d, j, &l2) > 0) { adv = j; break; }
        }
        if (adv > 0) { drop(d, adv); continue; }
        break;                                            // действительно ждём продолжения
    }
    return false;
}
