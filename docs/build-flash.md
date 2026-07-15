# Сборка и прошивка

Модель работы: **сборка — в облаке** (Linux, здесь), **прошивка и монитор — локально**
(Windows, плата у тебя). Тулчейн в облаке ставит SessionStart-хук; локально нужен свой ESP-IDF
(или голый `esptool` — см. ниже).

---

## Сборка (облако / любой Linux с ESP-IDF)

```bash
make build          # собрать (первый раз компилит весь IDF, дальше инкрементально)
make size           # размер по секциям
make merge          # build/ucsynth-merged.bin — один файл для прошивки на 0x0
make menuconfig     # поменять sdkconfig (PSRAM, консоль, ...)
```

Без `make`: `./tools/build.sh <любая idf.py-команда>` (сам подхватывает окружение IDF).

Артефакты в `build/`:
- `ucsynth.bin` — приложение
- `bootloader/bootloader.bin`, `partition_table/partition-table.bin`
- `flasher_args.json` / `flash_args` — смещения и параметры прошивки (источник правды)
- `ucsynth-merged.bin` — после `make merge`, всё слитое, шьётся одним куском на `0x0`

---

## Прошивка (локально, Windows)

`PORT`: нативный USB S3 обычно `COM…` (USB Serial/JTAG); UART-мост (CP2102/CH340) — другой `COM…`.
В диспетчере устройств два разных порта могут соответствовать одной плате — см. «нет логов».

### Вариант A. Установлен ESP-IDF (рекомендую)
Из папки проекта в «ESP-IDF PowerShell»:
```powershell
idf.py -p COM7 flash monitor
```

### Вариант B. Только esptool (без полного IDF)
Забираешь из облака `build/ucsynth-merged.bin`, потом:
```powershell
pip install esptool
esptool.py --chip esp32s3 -p COM7 -b 460800 write_flash 0x0 ucsynth-merged.bin
```
Или тремя файлами по смещениям (см. `flasher_args.json`; для S3 это):
```
0x0      bootloader/bootloader.bin
0x8000   partition_table/partition-table.bin
0x10000  ucsynth.bin
```
> ⚠ Смещение загрузчика у S3 — **0x0** (не 0x1000, как у классического ESP32). Не перепутай.

---

## Что должно быть в логе (проверка этапа 0)

Открой монитор (`idf.py -p COM7 monitor`, выход `Ctrl+]`). Ожидаемо:

```
I (…) ucsynth: UCSynth boot — этап 0 (каркас и протокол)
I (…) ucsynth: chip ESP32-S3 rev N, 2 ядр(а), flash 16 МБ
I (…) ucsynth: PSRAM 8 МБ (свободно …)
I (…) control: init (заглушка — модель параметров, этап 0.2)
I (…) comm:    init (заглушка — протокол Serial, этап 0.3)
I (…) io:      init (заглушка — периферия, этапы 8–10)
I (…) audio:   init (заглушка — I2S/DSP, этап 1)
I (…) ucsynth: boot complete
I (…) ucsynth: alive — heap … КБ …        (раз в 5 c)
```

**Ключевое:** `flash 16 МБ` и `PSRAM 8 МБ`. Если PSRAM 0 или warning про Octal — см. ниже.

---

## Диагностика

**Не прошивается / порт не открывается**
- Плата не в режиме загрузки: зажми **BOOT (IO0)**, коротко нажми **RST/EN**, отпусти BOOT → повтори flash.
- Порт занят открытым монитором/другой программой — закрой.
- Windows: нет драйвера USB Serial/JTAG (нативный USB S3) или CP210x/CH340 (мост) — доставь.
- Попробуй ниже скорость: `-b 115200`.

**Прошилось (`Hash of data verified`), но чип висит в `waiting for download`**
- В логе `boot:0x0 (DOWNLOAD(USB/UART0))` — чип на сбросе ушёл в загрузчик, а не в приложение
  (на сбросе GPIO0/BOOT был низким). Авто-сброс не всегда возвращает в run.
- Фикс: нажми на плате **RST/EN** один раз (BOOT **не** держи) — приложение стартует.
- Если повторяется каждый раз через мост — это квирк авто-сброса моста; проще шить/смотреть
  через нативный **USB** порт (он же и для логов).

**Прошилось, но нет логов**
- Скорее всего логи идут не в тот канал. По умолчанию консоль — **USB-Serial-JTAG** (нативный USB
  S3, `sdkconfig.defaults`). Если твоя плата подключена через **UART-мост**, логов на нём не будет.
  Варианты: (1) подключись к нативному USB-порту S3; (2) переключи консоль на UART0 —
  `make menuconfig` → *Component config → ESP System Settings → Channel for console output → UART0*,
  пересобери, прошей.
- После сброса монитор мог пропустить первые строки — нажми RST при открытом мониторе.

**PSRAM 0 МБ или предупреждение про Octal**
- N16R8 — это **Octal** PSRAM. Проверь в `sdkconfig.defaults`: `CONFIG_SPIRAM_MODE_OCT=y`
  (не Quad). Если модуль реально Quad (R2/R8V иных ревизий) — поставь `CONFIG_SPIRAM_MODE_QUAD=y`.
- Реже: PSRAM конфликтует по частоте с flash. Понизь `CONFIG_SPIRAM_SPEED_40M` и проверь.

**Сборка падает: `IDF_PATH` не найден / `idf.py: command not found`**
- Не поставлен тулчейн: `make setup` (в облаке — уже сделал SessionStart-хук).
