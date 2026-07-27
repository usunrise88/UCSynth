#!/bin/bash
# Ставит Go, если подходящей версии нет — для облачной проверки GUI-контроллера (app/):
# кросс-сборка на Windows (cgo-free) + чистые тесты. У пользователя на Windows свой Go.
#
# Проверяем именно ВЕРСИЮ, а не «есть ли go в PATH». Раньше скрипт выходил при любом go, и в
# контейнере с системным Go 1.24.7 при `go 1.25.0` в go.mod включался GOTOOLCHAIN=auto: Go молча
# СКАЧИВАЛ toolchain@go1.25.0 на каждую сборку. Сборка выглядела рабочей, но зависела от сети —
# при закрытом egress `make app-test` падал с «toolchain not available» при формально
# установленном Go.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Нужная версия — из app/go.mod (строка `go X.Y[.Z]`), чтобы не расходилась с реальным требованием.
NEED="$(awk '/^go /{print $2; exit}' "$ROOT/app/go.mod")"
NEED="${NEED:-1.25.0}"

# Сравнение версий: 1.24.7 < 1.25.0. sort -V даёт это без разбора на компоненты.
ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }

have=""
if command -v go >/dev/null 2>&1; then
  have="$(go env GOVERSION 2>/dev/null | sed 's/^go//')"
fi

if [ -n "$have" ] && ver_ge "$have" "$NEED"; then
  echo "[setup-go] go $have уже подходит (нужно >= $NEED)"
  exit 0
fi

if [ -n "$have" ]; then
  echo "[setup-go] go $have старее требуемого $NEED (иначе GOTOOLCHAIN качал бы тулчейн на каждую сборку)"
fi

GO_VERSION="${GO_VERSION:-$NEED}"
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

# Пробросить PATH в сессию, как это делает setup-esp-idf.sh. Раньше путь только печатался, и в
# свежем контейнере хук рапортовал об успехе, а следующая команда получала `go: command not found`.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "[setup-go] пробрасываю PATH в CLAUDE_ENV_FILE"
  echo "export PATH=\"/usr/local/go/bin:\$PATH\"" >> "$CLAUDE_ENV_FILE"
else
  echo "[setup-go] в PATH: export PATH=/usr/local/go/bin:\$PATH"
fi
