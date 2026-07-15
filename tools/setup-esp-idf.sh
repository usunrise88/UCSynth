#!/usr/bin/env bash
#
# Ставит ESP-IDF для сборки UCSynth. Идемпотентно: повторный запуск ничего не ломает.
# Используется и вручную (`make setup`), и из SessionStart-хука в Claude Code on the web.
#
# Почему так: тулчейн большой (~1–2 ГБ), в облачном окружении его нет. Хук ставит его
# один раз за сессию, состояние контейнера кэшируется — дальше сборка «здесь» без ручных шагов.

set -euo pipefail

# --- настройки (можно переопределить через env) ---------------------------------
IDF_BRANCH="${IDF_BRANCH:-v6.0.2}"     # пинуем стабильную версию: воспроизводимость сборки
IDF_TARGET="${IDF_TARGET:-esp32s3}"    # ставим тулчейн только под нужный таргет, не под все
: "${IDF_PATH:=$HOME/esp/esp-idf}"
export IDF_PATH

# Тулчейн качаем с официального CDN Espressif, а не с github releases.
# Зачем: в облачной сессии egress-политика режет github.com/.../releases/download (403),
# а dl.espressif.com разрешён. Локально тоже работает (часто быстрее). Переопределяемо.
export IDF_GITHUB_ASSETS="${IDF_GITHUB_ASSETS:-dl.espressif.com/github_assets}"

# Сбрасываем унаследованный от прошлой версии IDF python-env. Иначе при смене версии
# install.sh падает: «env generated for 5.5 instead of 6.0». Пусть install.sh сам
# вычислит путь под текущую версию (в веб-сессии он мог просочиться через CLAUDE_ENV_FILE).
unset IDF_PYTHON_ENV_PATH || true

echo "[setup] IDF_PATH=$IDF_PATH  branch=$IDF_BRANCH  target=$IDF_TARGET"

# --- клон ESP-IDF ----------------------------------------------------------------
# shallow + shallow-submodules: тянем только нужную версию, экономим трафик/диск.
clone_idf() {
  git clone --depth 1 --branch "$IDF_BRANCH" --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_PATH"
}

mkdir -p "$(dirname "$IDF_PATH")"
if [ ! -d "$IDF_PATH/.git" ]; then
  echo "[setup] клонирую ESP-IDF $IDF_BRANCH ..."
  clone_idf
else
  # Version-aware: если пин сменился (напр. 5.5.4 -> 6.0.2), переставляем начисто.
  # Shallow-клон на другой мажор проще переклонировать, чем чинить fetch+submodule.
  current="$(git -C "$IDF_PATH" describe --tags --exact-match 2>/dev/null || echo none)"
  if [ "$current" != "$IDF_BRANCH" ]; then
    echo "[setup] версия IDF сменилась ($current -> $IDF_BRANCH), переставляю начисто ..."
    rm -rf "$IDF_PATH"
    clone_idf
  else
    echo "[setup] ESP-IDF уже на $IDF_BRANCH — пропускаю клон"
  fi
fi

# --- системные зависимости тулчейна ---------------------------------------------
# openocd (входит в набор install.sh) слинкован с libusb-1.0; без неё его self-check
# падает (exit 127) и рушит ВСЮ установку, хотя для сборки openocd не нужен.
# Ставим best-effort и только на apt-системах (облачный контейнер).
if ! ldconfig -p 2>/dev/null | grep -q 'libusb-1.0.so.0'; then
  if command -v apt-get >/dev/null 2>&1; then
    echo "[setup] ставлю libusb-1.0-0 (нужна openocd) ..."
    apt-get install -y libusb-1.0-0 >/dev/null 2>&1 \
      || { apt-get update >/dev/null 2>&1 && apt-get install -y libusb-1.0-0 >/dev/null 2>&1; } \
      || echo "[setup] warn: не поставил libusb-1.0-0 — openocd может не встать (для сборки не критично)"
  fi
fi

# --- установка инструментов (тулчейн, python-env) --------------------------------
# install.sh идемпотентен: уже установленное не качает заново.
echo "[setup] ставлю инструменты для $IDF_TARGET ..."
"$IDF_PATH/install.sh" "$IDF_TARGET"

# --- проброс окружения в сессию Claude Code on the web ---------------------------
# CLAUDE_ENV_FILE есть только в веб-сессии. Локально этот блок пропускается —
# там пользователь сам делает `. $IDF_PATH/export.sh` (или через `make`).
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "[setup] пробрасываю окружение IDF в CLAUDE_ENV_FILE"
  # export.sh правит PATH под тулчейн и python-env; забираем результат целиком,
  # чтобы `idf.py` работал в сессии напрямую, без обёрток.
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null
  {
    echo "export IDF_PATH=\"$IDF_PATH\""
    echo "export PATH=\"$PATH\""
    [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && echo "export IDF_PYTHON_ENV_PATH=\"$IDF_PYTHON_ENV_PATH\""
    [ -n "${IDF_TOOLS_PATH:-}" ]      && echo "export IDF_TOOLS_PATH=\"$IDF_TOOLS_PATH\""
  } >> "$CLAUDE_ENV_FILE"
fi

echo "[setup] готово. Сборка: \`make build\` (или \`./tools/build.sh build\`)."
