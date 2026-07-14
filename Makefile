# UCSynth — обёртки над idf.py, чтобы держать в голове минимум команд.
# Все цели через ./tools/build.sh, который сам подхватывает окружение ESP-IDF.
#
#   make setup      — поставить ESP-IDF (один раз; в облаке делает SessionStart-хук)
#   make build      — собрать прошивку
#   make menuconfig — конфигуратор sdkconfig
#   make size       — размер прошивки по секциям
#   make merge      — слить в один build/ucsynth-merged.bin для прошивки на 0x0
#   make clean / fullclean
#   make flash PORT=... / make monitor PORT=...   — локально (плата у пользователя)
#
# PORT: Windows — COMx, Linux — /dev/ttyACM0 (нативный USB) или /dev/ttyUSB0 (UART-мост).

PORT ?= /dev/ttyACM0
BUILD := ./tools/build.sh

.PHONY: setup build menuconfig size merge clean fullclean flash monitor flash-monitor

setup:
	./tools/setup-esp-idf.sh

build:
	$(BUILD) build

menuconfig:
	$(BUILD) menuconfig

size:
	$(BUILD) size

# Слитый образ: удобно отдать один файл на локальную машину и прошить на 0x0.
merge: build
	$(BUILD) merge-bin -o build/ucsynth-merged.bin

clean:
	$(BUILD) clean

fullclean:
	$(BUILD) fullclean

flash:
	$(BUILD) -p $(PORT) flash

monitor:
	$(BUILD) -p $(PORT) monitor

flash-monitor:
	$(BUILD) -p $(PORT) flash monitor
