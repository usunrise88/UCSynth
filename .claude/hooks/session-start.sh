#!/bin/bash
#
# SessionStart-хук для Claude Code on the web: ставит ESP-IDF, чтобы прошивка собиралась
# «здесь» без ручных шагов. Синхронный (сессия ждёт установку) — гарантирует, что
# тулчейн готов до первой сборки. Состояние контейнера кэшируется, повторный старт быстрый.
#
# Запускается только в облаке (CLAUDE_CODE_REMOTE=true). Локально ничего не делает —
# там ESP-IDF у пользователя свой.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

"$CLAUDE_PROJECT_DIR/tools/setup-esp-idf.sh"
