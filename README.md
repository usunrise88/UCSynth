# UCSynth — Unnecessary Complicated Synthesizer

Полифонический wavetable-синтезатор на **ESP32-S3**. Проба пера: разовая
обучающая сборка из готовых модулей на бредборде. Не фундамент линейки —
настоящая платформа будет на STM32 отдельным проектом.

## Что это

- **Прошивка** — нативный ESP-IDF (не Arduino), таргет ESP32-S3-N16R8
  (16 МБ flash / 8 МБ Octal PSRAM).
- **Звук** — I2S → внешний ЦАП PCM5102.
- **Управление** — GUI-пульт на Go (Windows) по USB CDC; позже энкодеры и тач.
- **Ядра** — Core 0: аудио (I2S+DMA, DSP); Core 1: UI, контролы, Serial.
- **Сборка** — на Linux; **прошивка и отладка** — локально (Windows).

Единая модель параметров: все источники управления пишут только через
`set_param`/`get_param`, напрямую в DSP — никогда. Это позволяет добавлять
физические контролы позже без переписывания движка.

## Статус — этап 0 закрыт ✅ (проверено на железе)

- Каркас на ESP-IDF v6.0.2, конфиг под S3-N16R8 (Octal PSRAM 8 МБ, flash 16 МБ).
- Реестр параметров (`control`): lock-free, метаданные во flash.
- Бинарный протокол Serial по USB-JTAG (`comm`): кадры `[55 AA][LEN][BODY][CRC16]`,
  опкоды GET/SET/LIST/NOTE_ON/OFF/STAT — читать/менять любой параметр с ПК ещё до звука.

Дальше — этап 1: чистый звук (I2S + PCM5102 + wavetable-осциллятор).
Полный план этапов — [`docs/roadmap.md`](docs/roadmap.md).

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
python tools/serialtest.py COM7     # LIST/GET/SET/STAT/NOTE_ON, декодирует ответы
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
| [`docs/serial-protocol.md`](docs/serial-protocol.md) | Контракт бинарного протокола |
| [`docs/build-flash.md`](docs/build-flash.md) | Сборка и прошивка |
| [`progress.md`](progress.md) | Что сделано, карта пинов, замеры |
| [`tech-debt.md`](tech-debt.md) | Костыли и упрощения |

## Структура

```
main/            точка входа, инициализация слоёв
components/
  control/       модель параметров (реестр)          ← этап 0.2
  comm/          протокол Serial по USB-JTAG (Core 1) ← этап 0.3
  audio/         I2S + DMA + DSP (Core 0)             — этап 1+
  io/            периферия: I2C, энкодеры, тач        — этап 8+
docs/            спецификация, гайды, контракты
tools/           сборка, установка ESP-IDF, тестеры
test/host/       host-тесты чистой логики (без железа)
```
