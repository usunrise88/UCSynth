# UCSynth — Unnecessary Complicated Synthesizer

Полифонический wavetable-синтезатор на **ESP32-S3**. Проба пера: разовая
обучающая сборка из готовых модулей на бредборде. Не фундамент линейки —
настоящая платформа будет на STM32 отдельным проектом.

## Что это

- **Прошивка** — нативный ESP-IDF (не Arduino), таргет ESP32-S3-N16R8
  (16 МБ flash / 8 МБ Octal PSRAM).
- **Звук** — полноценный голос: band-limited wavetable (3 осц) → микшер → ZDF-фильтр →
  VCA, две ADSR, полифония до 8 голосов, glide → I2S → внешний ЦАП PCM5102.
- **Управление** — GUI-пульт на Go/Gio (Windows) по USB CDC (`app/`, этап 2, v1 — ядро-контроллер);
  позже энкодеры и тач.
- **Экран** — отладочный OLED SSD1306 (I2C): осциллограф + параметры (временно, до ST7796).
- **Ядра** — Core 0: аудио (I2S+DMA, DSP); Core 1: UI, дисплей, контролы, Serial.
- **Сборка** — на Linux; **прошивка и отладка** — локально (Windows).

Единая модель параметров: все источники управления пишут только через
`set_param`/`get_param`, напрямую в DSP — никогда. Это позволяет добавлять
физические контролы позже без переписывания движка.

## Статус — этапы 0–1 на железе ✅, этап 3 (голос) собран

**Этап 0** — каркас на ESP-IDF v6.0.2 (S3-N16R8: Octal PSRAM 8 МБ, flash 16 МБ);
реестр параметров (`control`, lock-free, 34 параметра); бинарный протокол Serial по
USB-JTAG (`comm`): кадры `[55 AA][LEN][BODY][CRC16]`, опкоды GET/SET/LIST/NOTE_ON/OFF/STAT.

**Этап 1 — чистый звук:** wavetable-осциллятор → I2S 48 кГц/16 бит → PCM5102; аудио-задача
на Core 0; метрики CPU/underruns в STAT.

**Этап 3 — голос целиком** (чистый DSP, отделён от I/O, host-тестируемый; сборка на `-O2`):

- **Осциллятор** — band-limited wavetable (октавные mip-таблицы против алиасинга),
  3 осц-слота (форма/детюн/уровень), шум, ring mod.
- **Фильтр** — ZDF/TPT state-variable (LP/HP/BP/OFF), cutoff + резонанс.
- **Огибающие** — две ADSR (VCA и VCF→cutoff), gate-driven; **drone** (latch + опц. loop).
- **Полифония** — до 8 голосов (round-robin + oldest-steal), моно со стеком нот.
- **Glide** — портаменто (лог-высота), mono/poly, legato.
- **Lo-fi** — bit-crush + отключение band-limit (алиасинг как фича).

Ноты играются с ПК; всё рулится 34 параметрами (GUI/скрипт строятся из `LIST` динамически).

**Отладочный OLED** (SSD1306 128×64, I2C; вне спеки — до цветного ST7796): splash,
осциллограф формы волны, popup значения параметра при смене.

На железе ✅: этапы 0–1, нотный путь + band-limited осц (3.0), голос 3.1–3.4 (звук отыгран).
В процессе: замер потолка полифонии (3.5) на `-O2`.

Дальше — этап 2 (GUI на Go) / этап 4 (модуляция: LFO×2, wave-огибающая, матрица).
Полный план — [`docs/roadmap.md`](docs/roadmap.md).

## Сборка

Linux / любой хост с ESP-IDF (в облаке тулчейн ставит SessionStart-хук):

```bash
make setup      # поставить ESP-IDF (один раз)
make build      # собрать прошивку
make size       # размер по секциям
make merge      # build/ucsynth-merged.bin — один файл для прошивки на 0x0
```

## Прошивка (локально, Windows)

Порт — **нативный USB S3** (USB Serial/JTAG), не UART-мост:

```powershell
idf.py -p COM7 flash monitor
```

Ожидаемый boot-лог, диагностика, вариант без полного IDF (голый esptool) —
[`docs/build-flash.md`](docs/build-flash.md).

## Проверка протокола

Бинарь голым терминалом не набрать — есть референс-клиент (`pip install pyserial`):

```bash
python tools/serialtest.py COM7     # LIST/STAT + демо голоса: ADSR, фильтр, 3 осц, lo-fi, полифония, glide
```

Логика (реестр, кадрирование, CRC, диспетчер) проверяется и без железа:

```bash
bash tools/run-host-tests.sh
```

Контракт протокола (по нему пишется Go-GUI) — [`docs/serial-protocol.md`](docs/serial-protocol.md).

## Документы

| Файл | О чём |
|------|-------|
| [`docs/spec.md`](docs/spec.md) | Спецификация: что строим |
| [`docs/roadmap.md`](docs/roadmap.md) | План этапов |
| [`docs/hardware.md`](docs/hardware.md) | Железо: модули, адреса, грабли |
| [`docs/hardware-stage1-pcm5102.md`](docs/hardware-stage1-pcm5102.md) | Гайд: PCM5102 по I2S |
| [`docs/hardware-oled-ssd1306.md`](docs/hardware-oled-ssd1306.md) | Гайд: OLED SSD1306 по I2C |
| [`docs/serial-protocol.md`](docs/serial-protocol.md) | Контракт бинарного протокола |
| [`docs/gui.md`](docs/gui.md) | GUI-контроллер (Go/Gio): сборка, запуск, структура |
| [`docs/build-flash.md`](docs/build-flash.md) | Сборка и прошивка |
| [`progress.md`](progress.md) | Что сделано, карта пинов, замеры |
| [`tech-debt.md`](tech-debt.md) | Костыли и упрощения |

## Структура

```
main/            точка входа, инициализация слоёв
components/
  control/       модель параметров (реестр)          ← этап 0.2
  comm/          протокол Serial по USB-JTAG (Core 1) ← этап 0.3
  audio/         I2S + DSP-голос на Core 0 (этапы 1,3):
                 wavetable / filter / env / voice / synth (полифония, glide)
  display/       отладочный OLED SSD1306 (Core 1)     ← вне спеки, до ST7796
  io/            периферия: I2C, энкодеры, тач        — этап 8+
app/             GUI-контроллер на Go/Gio (Windows)   ← этап 2 (proto/serial/device/layout/ui)
docs/            спецификация, гайды, контракты
tools/           сборка, установка ESP-IDF, тестеры
test/host/       host-тесты чистой логики (без железа)
```
