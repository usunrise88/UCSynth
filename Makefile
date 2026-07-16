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

#   make host-test  — host-тесты прошивки (чистая DSP-логика, g++)
#   make app-test   — тесты GUI-контроллера (app/): чистые пакеты + кросс-vet Windows
#   make app-build  — собрать app/build/ucsynth-controller.exe (Windows, из Linux)
#
# PORT: Windows — COMx, Linux — /dev/ttyACM0 (нативный USB) или /dev/ttyUSB0 (UART-мост).

PORT ?= /dev/ttyACM0
BUILD := ./tools/build.sh

.PHONY: setup build menuconfig size merge clean fullclean flash monitor flash-monitor \
        host-test app-test app-cross app-build

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

# --- Хост-тесты и GUI-контроллер (без железа) ---

host-test:
	bash ./tools/run-host-tests.sh

app-test:
	bash ./tools/run-app-tests.sh

app-cross:
	cd app && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go vet ./...

app-build:
	cd app && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -ldflags "-H=windowsgui" -o build/ucsynth-controller.exe ./cmd/controller
	@echo "→ app/build/ucsynth-controller.exe"
