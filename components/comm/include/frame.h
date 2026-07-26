// Кадрирование бинарного протокола.
//
// Один канал USB-Serial-JTAG несёт И логи (ASCII от ESP_LOG), И кадры протокола.
// Поэтому кадр начинается синхромаркером и заканчивается CRC: приёмник ловит
// 0x55 0xAA, проверяет CRC — а логи, мусор и байты, случайно перемешавшиеся с
// кадром, CRC не проходят и отбрасываются с ресинхронизацией.
//
//   Кадр:  [0x55][0xAA][LEN:u8][BODY: LEN байт][CRC16_LE:2]
//   CRC:   CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), считается по (LEN + BODY),
//          в кадре — little-endian. Тот же алгоритм должен повторить GUI/скрипт.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_SYNC0     0x55u
#define FRAME_SYNC1     0xAAu
#define FRAME_MAX_BODY  255u
#define FRAME_MAX_SIZE  (2u + 1u + FRAME_MAX_BODY + 2u)  // sync(2)+len(1)+body+crc(2)

// CRC-16/CCITT-FALSE по буферу.
uint16_t frame_crc16(const uint8_t *data, size_t len);

// Обернуть body в кадр (пишет в out). Возвращает длину кадра или 0, если body слишком
// длинный (> FRAME_MAX_BODY) либо не влезает в out_cap.
size_t frame_encode(const uint8_t *body, size_t body_len, uint8_t *out, size_t out_cap);

// Потоковый декодер: корми по байту. Возвращает true ровно когда собран валидный кадр —
// тогда *body_out указывает внутрь декодера, *body_len_out = длина тела (валидны до
// следующего вызова). Иначе накапливает/ресинхронизируется.
//
// Модель — накопитель + рескан, та же, что на PC-стороне (app/proto/frame.go, tools/serialtest.py).
// Байтовый автомат тут не годится: он потребляет необратимо (увидев sync, берёт следующий байт как
// LEN и съедает LEN+2), поэтому кадр, начавшийся внутри окна ложного синка, уничтожался целиком.
typedef struct {
    int      n;                        // накоплено байт в buf
    uint8_t  buf[FRAME_MAX_SIZE];      // sync(2)+len(1)+body(255)+crc(2)
    uint8_t  body[FRAME_MAX_BODY];     // выданное тело (валидно до следующего вызова)
} frame_decoder_t;

void frame_decoder_init(frame_decoder_t *d);
bool frame_decoder_push(frame_decoder_t *d, uint8_t byte,
                        const uint8_t **body_out, size_t *body_len_out);

#ifdef __cplusplus
}
#endif
