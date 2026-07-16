#!/usr/bin/env bash
# Проверка GUI-контроллера (app/) без железа и без дисплея — симметрично run-host-tests.sh:
#  - чистые пакеты + ui/ → нативные go-тесты (без C/дисплея). ui/ тестируется headless
#    через input.Router (роутинг событий — чистый CPU, дисплей не нужен): там живёт
#    регрессия на «клавиатура перехватывает клики кнопок».
#  - весь код, включая cmd/ → кросс-компиляция + vet на Windows (cgo-free).
# НЕ `go test ./...` нативно: cmd/controller тянет gioui.org/app (X11/Wayland/xkb), которых
# в облаке нет, и падает на сборке. Сам ui/ от gioui.org/app не зависит — тестируется.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/app"

echo "== go test (чистые пакеты + ui headless) =="
go test ./proto/... ./device/... ./serial/... ./layout/... ./patch/... ./seq/... ./midi/... ./ui/...

echo "== кросс-сборка Windows (cgo-free, вкл. UI) =="
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go vet ./...
# -H=windowsgui → GUI-подсистема PE: приложение не открывает консольное окно при старте.
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -ldflags "-H=windowsgui" -o build/ucsynth-controller.exe ./cmd/controller

echo "OK: app — тесты зелёные, build/ucsynth-controller.exe собран"
