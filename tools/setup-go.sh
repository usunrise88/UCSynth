#!/bin/bash
# Ставит Go, если его нет — для облачной проверки GUI-контроллера (app/): кросс-сборка на
# Windows (cgo-free) + чистые тесты. No-op, если go уже в PATH. У пользователя на Windows свой Go.
set -euo pipefail

if command -v go >/dev/null 2>&1; then
  echo "[setup-go] go уже есть: $(go version)"
  exit 0
fi

GO_VERSION="${GO_VERSION:-1.25.1}"   # app/go.mod требует >= 1.25 (зависимость go.bug.st/serial)
case "$(uname -m)" in
  x86_64)        GOARCH=amd64 ;;
  aarch64|arm64) GOARCH=arm64 ;;
  *) echo "[setup-go] неизвестная арх $(uname -m) — поставь Go вручную"; exit 1 ;;
esac
TARBALL="go${GO_VERSION}.linux-${GOARCH}.tar.gz"

echo "[setup-go] ставлю Go ${GO_VERSION} (${GOARCH}) ..."
curl -fsSL "https://go.dev/dl/${TARBALL}" -o "/tmp/${TARBALL}"
rm -rf /usr/local/go
tar -C /usr/local -xzf "/tmp/${TARBALL}"
echo "[setup-go] готово: $(/usr/local/go/bin/go version)"
echo "[setup-go] в PATH: export PATH=/usr/local/go/bin:\$PATH"
