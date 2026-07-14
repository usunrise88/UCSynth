#!/usr/bin/env bash
#
# Тонкая обёртка над idf.py: сама подхватывает окружение ESP-IDF, чтобы не держать
# в голове `. export.sh`. Любая команда idf.py: `./tools/build.sh build|menuconfig|size|...`.

set -euo pipefail

: "${IDF_PATH:=$HOME/esp/esp-idf}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "ESP-IDF не найден в $IDF_PATH. Запусти ./tools/setup-esp-idf.sh (или \`make setup\`)." >&2
  exit 1
fi

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null
exec idf.py "$@"
