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

- `LEN` — длина `BODY` (0..255). `LEN=0` легален (кадр без тела, опкода в нём нет и смысла тоже),
  но обе реализации обязаны его принимать и уметь закодировать: иначе код, прогоняющий принятое
  тело обратно через энкодер, падает на пустом кадре.
- `CRC16` — **CRC-16/CCITT-FALSE** (poly `0x1021`, init `0xFFFF`, без реверса и xorout),
  считается по `(LEN + BODY)`, в кадре — младшим байтом вперёд.
- Приёмник: **накопитель + рескан**. Ищет `55 AA`, читает `LEN`, тело и CRC, проверяет CRC.
  Не сошлось (лог, мусор, разрыв кадра логом) → сдвинуться на 2 байта и искать следующий `55 AA`
  **в уже принятых данных**. Байтовый автомат, потребляющий необратимо, здесь не годится: увидев
  ложный `55 AA`, он съел бы `LEN+2` байта вместе с настоящим кадром внутри этого окна.
  Отдельно: если кадру не хватает байт, но дальше в буфере уже лежит кадр с сошедшимся CRC —
  значит текущий синк ложный, и ждать его нельзя (иначе готовый кадр висит, пока не придут байты,
  которых может и не быть).

## Тело: `[CMD:u8][args…]`

### ПК → МК (запросы)

| CMD  | Имя           | Аргументы                                   |
|------|---------------|---------------------------------------------|
| 0x01 | SET           | `id:u16` `val:f32`                          |
| 0x02 | GET           | `id:u16`                                    |
| 0x03 | LIST          | —                                           |
| 0x04 | NOTE_ON       | `note:u8` `vel:u8`                          |
| 0x05 | NOTE_OFF      | `note:u8`                                   |
| 0x06 | STAT          | —                                           |
| 0x07 | PRESET_LIST   | —                                           |
| 0x08 | PRESET_SAVE   | `slot:u16` (`0xFFFF`=новый) `path_len:u8` `path` |
| 0x09 | PRESET_LOAD   | `slot:u16`                                  |
| 0x0A | PRESET_DELETE | `slot:u16`                                  |
| 0x0B | PRESET_RENAME | `slot:u16` `path_len:u8` `path`             |
| 0x0C | SEQ_SET_STEP  | `step:u8` `step-blob` (один шаг паттерна)   |
| 0x0D | SEQ_GET       | —                                           |
| 0x0E | SEQ_SAVE      | `slot:u16` (`0xFFFF`=новый) `path_len:u8` `path` |
| 0x0F | SEQ_LOAD      | `slot:u16`                                  |
| 0x10 | SEQ_DELETE    | `slot:u16`                                  |
| 0x11 | SEQ_LIST      | —                                           |
| 0x12 | SEQ_RENAME    | `slot:u16` `path_len:u8` `path`             |

### МК → ПК (ответы)

| RSP  | Имя          | Аргументы                                                                    |
|------|--------------|------------------------------------------------------------------------------|
| 0x80 | ACK          | — (ответ на NOTE_ON/OFF, PRESET_LOAD/DELETE/RENAME)                           |
| 0x81 | VALUE        | `id:u16` `val:f32` (ответ на GET и SET — значение после клампа)               |
| 0x82 | PARAM        | `id:u16` `type:u8` `min:f32` `max:f32` `def:f32` `cur:f32` `namelen:u8` `name` |
| 0x83 | LISTEND      | `count:u16`                                                                  |
| 0x84 | PRESET       | `slot:u16` `path_len:u8` `path` (по одному на пресет в ответ на PRESET_LIST)  |
| 0x85 | PRESET_END   | `count:u16`                                                                  |
| 0x86 | STAT         | `heap:u32` `minheap:u32` `uptime_ms:u32` `cpu_permille:u32` `underruns:u32`    |
| 0x87 | PRESET_SAVED | `slot:u16` (назначенный слот, в ответ на PRESET_SAVE)                         |
| 0x88 | SEQ_STEP     | `step:u8` `step-blob` (по одному на шаг в ответ на SEQ_GET; всего 16)         |
| 0x89 | SEQ_ENTRY    | `slot:u16` `path_len:u8` `path` (в ответ на SEQ_LIST)                         |
| 0x8A | SEQ_END      | `count:u16` (конец SEQ_LIST)                                                  |
| 0x8B | SEQ_SAVED    | `slot:u16` (назначенный слот, в ответ на SEQ_SAVE)                            |
| 0xFF | ERR          | `code:u8`                                                                     |

`LIST` → серия `PARAM` (по одному на параметр) + завершающий `LISTEND`.

**Тип параметра** (`type` в PARAM): `0` float, `1` int, `2` enum (значение = индекс),
`3` bool. GUI рисует контрол по типу.

**Ноты (этап 3.0):** `NOTE_ON note vel` играет ноту на моно-голосе (высота = 12-TET, A4=69=440 Гц),
`NOTE_OFF note` гасит её. Реальную высоту дают ноты; отладочный тон включается параметром
`test_tone` (bool, деф. 1 — звучит с загрузки, перебивает ноты; `SET test_tone 0` → играют ноты).

**Пресеты (этап 6):** хранятся на устройстве в NVS, дерево — из путей `Папка/Имя`. `PRESET_SAVE`
снимает текущий реестр в слот (`slot=0xFFFF` → выделить новый; ответ `PRESET_SAVED` несёт назначенный
слот). `PRESET_LOAD` применяет пресет к реестру — GUI обновляет свой кэш обычным `LIST` (в `PARAM`
приходит `cur`). `PRESET_LIST` → серия `PRESET` + завершающий `PRESET_END count`. Отладочный `test_tone`
в снимок не входит и загрузкой не дёргается. Путь — до 96 байт.

**Секвенсор (этап 7):** темп/транспорт/арп — обычные параметры реестра (`seq_bpm`, `seq_swing`,
`seq_playing`, `seq_on`, `arp_*`; идут через `SET`/`LIST`). **Паттерн** (16 шагов × аккорд + p-locks)
не влезает в один кадр (`LEN≤255`, весь ~900 Б), поэтому передаётся **пошагово**: `SEQ_SET_STEP step blob`
на каждый изменённый шаг; `SEQ_GET` → 16 кадров `SEQ_STEP`. Хранилище паттернов — в NVS (namespace
`patterns`), опкоды `SEQ_SAVE/LOAD/DELETE/LIST/RENAME` зеркалят пресетные (`SEQ_LOAD` применяет к живому
паттерну — GUI затем шлёт `SEQ_GET`). **Раскладка `step-blob`** (LE): `u8 flags(bit0=active)` `u8 n_notes`
`u8 notes[n_notes]` `u8 velocity` `u8 trig_q` (вероятность·255) `u8 n_plocks` `n_plocks×{ u16 id; f32 val }`.
Весь паттерн в NVS = `u8 version` + 16×step-blob.

**Коды ошибок** (`ERR`): `1` неизвестная команда, `2` неверный id, `3` неверная длина тела,
`4` слот не найден (пустой пресет/паттерн), `5` сбой хранилища (NVS).

## Пример (кадры целиком, hex)

CRC-байты ниже — **реальные** (little-endian: младший байт первым), не заглушки, там где тело кадра
полностью определено. Эти же кадры зашиты литералами и сверяются байт-в-байт в обоих наборах тестов
(`app/proto/proto_test.go` → `TestEncodeGoldenFrames`, `test/host/test_protocol.cpp` → golden-блок) —
это кросс-якорь: любое расхождение реализации со спекой валит тест. Меняешь протокол — правь **все три**
(этот файл + оба теста) разом. У кадров с переменным телом (`PARAM` с min/max/def/cur, `STAT` с
живыми метриками) CRC зависит от содержимого — оставлены `<crc>`.

```
LIST:           55 AA 01 03 5D 1E
  ← PARAM #0:   55 AA 22 82 0000 00 <min f32> <max f32> <def f32> <cur f32> 0D "master_volume" <crc>
  ← PARAM #1:   55 AA 21 82 0100 00 ... 0C "test_tone_hz" <crc>
  ← PARAM #2:   55 AA 1D 82 0200 02 ... 08 "waveform" <crc>   (type 02 = enum, 0..3)
  ← PARAM #3:   55 AA 1E 82 0300 03 ... 09 "test_tone" <crc>  (type 03 = bool, 0/1)
  ← ... PARAM #4..#33  (голос 3.1–3.6: ADSR, осц, фильтр, lo-fi, полифония, glide) ...
  ← ... PARAM #34..#63 (этап 4: LFO×2, mod-wheel, 8 слотов мод-матрицы, wave-огибающая) ...
  ← ... PARAM #64..#85 (этап 5: overdrive, delay, reverb) — все см. control.h ...
  ← ... PARAM #86..#87 (этап 6: reverb_moddepth, reverb_modrate) ...
  ← ... PARAM #88..#96 (этап 7: seq_bpm/swing/playing/on, arp_on/mode/octaves/rate/hold) ...
  ← LISTEND:    55 AA 03 83 <count LE> <crc>                  (count = PARAM_COUNT в control.h, растёт по этапам)

GET master_volume (id 0):   55 AA 03 02 0000 7C 71
  ← VALUE 0.8:              55 AA 07 81 0000 CDCC4C3F 17 B3

SET master_volume = 0.5:    55 AA 07 01 0000 0000003F FB 89
  ← VALUE 0.5:              55 AA 07 81 0000 0000003F 02 22

STAT:                       55 AA 01 06 F8 4E
  ← STAT:                   55 AA 15 86 <heap u32> <min u32> <uptime u32> <cpu u32> <underruns u32> <crc>

NOTE_ON 60 vel 100:         55 AA 03 04 3C64 06 AF
NOTE_OFF 60:                55 AA 02 05 3C D6 AA
кадр без тела (LEN=0):      55 AA 00 F0 E1
```

## Проверка на железе

Голым терминалом бинарь не набрать — используем скрипт (нужен Python + `pip install pyserial`):

```
python tools/serialtest.py COM8      # порт нативного USB S3 (не CH343-мост)
```

Скрипт делает LIST → STAT → выключает `test_tone` → играет ноты и прогоняет демо: голос
(3.1 ADSR/latch, 3.2 фильтр/резонанс/VCF-свип, 3.3 детюн/шум/ring, 3.4 lo-fi, 3.5/3.6
полифония/glide), модуляцию (4.1 LFO→cutoff/pitch через матрицу, 4.2 wave-огибающая/морф) и
эффекты (5.1 overdrive, 5.2 delay, 5.3 reverb).

Ожидаемо: список из **`PARAM_COUNT`** параметров (реестр строится динамически — новые параметры GUI/скрипт
подхватывают через LIST без правок; число сверяется с `PARAM_COUNT` в `control.h`, растёт по этапам), STAT
показывает heap/uptime + `cpu_permille` (‰ бюджета аудио-блока) и `underruns`, ноты меняют высоту,
высокие ноты звучат чисто (band-limit), демо слышно по секциям. Строки `ESP_LOG` в потоке — норма,
скрипт их пропускает (не проходят CRC).
