// seq_codec — (де)сериализация паттерна секвенсора (16 шагов + p-locks) в версионированный блоб.
// Чистый (только seq_engine.h + stddef) → host-тестируем. Отдельный тип блоба от пресета: у паттерна
// свои размерность (шаги) и переменное тело (аккорды + p-locks), не флэт-список {id,f32}.
#pragma once

#include "seq_engine.h"
#include <cstddef>
#include <cstdint>

#define SEQ_FMT_VERSION 1
// Верхняя граница блоба: 1 (version) + 16 × (flags+n_notes+notes[4]+vel+trig+n_plocks + 8·{u16+f32}) ≈ 1 + 16·57.
#define SEQ_BLOB_MAX 1024

// Раскладка блоба (little-endian):
//   u8 version
//   ×16 шаг: u8 flags(bit0=active) | u8 n_notes | u8 notes[n_notes] | u8 velocity | u8 trig_q(prob·255)
//            | u8 n_plocks | n_plocks×{ u16 id; f32 val }

// Один шаг: flags|n_notes|notes|velocity|trig_q|n_plocks|plocks — ≤ ~57 байт, влезает в кадр (LEN≤255).
// Для передачи по проводу пошагово (весь паттерн ~900 Б в один кадр не влезает).
size_t seq_step_serialize(uint8_t *buf, size_t cap, const SeqStep *st);
// Разобрать один шаг. Возврат: число потреблённых байт (0 = битый/усечённый).
size_t seq_step_deserialize(const uint8_t *buf, size_t len, SeqStep *st);

// Весь паттерн: u8 version + 16×шаг. Для NVS (блоб целиком, кадр не ограничивает).
size_t seq_serialize(uint8_t *buf, size_t cap, const SeqStep steps[SEQ_STEPS]);
bool   seq_deserialize(const uint8_t *buf, size_t len, SeqStep steps[SEQ_STEPS]);
