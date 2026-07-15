# Протокол Serial (бинарный)

Связь ПК ↔ UCSynth по **USB-Serial-JTAG** (нативный USB S3). Бинарный: эффективнее по
ресурсам МК (без `strtof`/`snprintf`-float), парсит его GUI/скрипт, не человек. Этот файл —
**контракт**: по нему пишется Go-GUI (этап 2). Референс-реализация клиента — `tools/serialtest.py`.

Значения — **little-endian**; `f32` — IEEE-754.

## Кадр

Логи (`ESP_LOG`) и протокол идут по одному каналу, поэтому кадр самоопознаётся:

```
[0x55][0xAA][LEN:u8][BODY: LEN байт][CRC16_LE:u16]
```

- `LEN` — длина `BODY` (1..255).
- `CRC16` — **CRC-16/CCITT-FALSE** (poly `0x1021`, init `0xFFFF`, без реверса и xorout),
  считается по `(LEN + BODY)`, в кадре — младшим байтом вперёд.
- Приёмник: ищет `55 AA`, читает `LEN`, тело и CRC, проверяет CRC. Не сошлось (лог, мусор,
  разрыв кадра логом) → отбросить и искать следующий `55 AA`.

## Тело: `[CMD:u8][args…]`

### ПК → МК (запросы)

| CMD  | Имя      | Аргументы             |
|------|----------|-----------------------|
| 0x01 | SET      | `id:u16` `val:f32`    |
| 0x02 | GET      | `id:u16`              |
| 0x03 | LIST     | —                     |
| 0x04 | NOTE_ON  | `note:u8` `vel:u8`    |
| 0x05 | NOTE_OFF | `note:u8`             |
| 0x06 | STAT     | —                     |

### МК → ПК (ответы)

| RSP  | Имя     | Аргументы                                                                    |
|------|---------|------------------------------------------------------------------------------|
| 0x80 | ACK     | — (ответ на NOTE_ON/OFF)                                                      |
| 0x81 | VALUE   | `id:u16` `val:f32` (ответ на GET и SET — значение после клампа)               |
| 0x82 | PARAM   | `id:u16` `type:u8` `min:f32` `max:f32` `def:f32` `cur:f32` `namelen:u8` `name` |
| 0x83 | LISTEND | `count:u16`                                                                  |
| 0x86 | STAT    | `heap:u32` `minheap:u32` `uptime_ms:u32` `cpu_permille:u32` `underruns:u32`    |
| 0xFF | ERR     | `code:u8`                                                                     |

`LIST` → серия `PARAM` (по одному на параметр) + завершающий `LISTEND`.

**Тип параметра** (`type` в PARAM): `0` float, `1` int, `2` enum (значение = индекс),
`3` bool. GUI рисует контрол по типу; сейчас все параметры — float.

**Коды ошибок** (`ERR`): `1` неизвестная команда, `2` неверный id, `3` неверная длина тела.

## Пример (кадры целиком, hex)

```
LIST:           55 AA 01 03 <crc>
  ← PARAM #0:   55 AA 22 82 0000 00 <min f32> <max f32> <def f32> <cur f32> 0D "master_volume" <crc>
  ← PARAM #1:   55 AA 21 82 0100 00 ... 0C "test_tone_hz" <crc>
  ← PARAM #2:   55 AA 1D 82 0200 02 ... 08 "waveform" <crc>   (type 02 = enum, 0..3)
  ← LISTEND:    55 AA 03 83 0300 <crc>

GET master_volume (id 0):   55 AA 03 02 0000 <crc>
  ← VALUE 0.8:              55 AA 07 81 0000 CDCC4C3F <crc>

SET master_volume = 0.5:    55 AA 07 01 0000 0000003F <crc>
  ← VALUE 0.5:              55 AA 07 81 0000 0000003F <crc>

STAT:                       55 AA 01 06 <crc>
  ← STAT:                   55 AA 15 86 <heap u32> <min u32> <uptime u32> <cpu u32> <underruns u32> <crc>
```

## Проверка на железе

Голым терминалом бинарь не набрать — используем скрипт (нужен Python + `pip install pyserial`):

```
python tools/serialtest.py COM8      # порт нативного USB S3 (не CH343-мост)
```

Скрипт делает LIST → GET → SET → STAT → NOTE_ON → свип `waveform`/`test_tone_hz` и печатает
декодированные ответы. Ожидаемо: список из трёх параметров (`master_volume`, `test_tone_hz`,
`waveform`), `master_volume` клампится, STAT показывает heap/uptime + `cpu_permille` (‰ бюджета
аудио-блока) и `underruns`, NOTE_ON → ACK, смена `waveform` слышна/видна на осциллографе. Строки
`ESP_LOG` в потоке — норма, скрипт их пропускает (не проходят CRC).
