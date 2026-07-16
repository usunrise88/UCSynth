#!/usr/bin/env bash
# Проверка GUI-контроллера (app/) без железа и без дисплея — симметрично run-host-tests.sh:
#  - чистые пакеты (proto/device/serial/layout) → нативные go-тесты (без C/дисплея);
#  - весь код, включая Gio-UI → кросс-компиляция + vet на Windows (cgo-free), так UI
#    типизируется без X11/Wayland/Vulkan-библиотек, которых в облаке нет.
# НЕ `go test ./...` нативно: потянет ui/ (cgo + отсутствующие GL-библиотеки) и упадёт.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/app"

echo "== go test (чистые пакеты) =="
go test ./proto/... ./device/... ./serial/... ./layout/...

echo "== кросс-сборка Windows (cgo-free, вкл. UI) =="
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go vet ./...
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o build/ucsynth-controller.exe ./cmd/controller

echo "OK: app — тесты зелёные, build/ucsynth-controller.exe собран"
