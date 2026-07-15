#!/usr/bin/env bash
# Host-тесты чистой логики (реестр + протокол + кадрирование) обычным g++, без ESP-IDF и железа.
# Работает и в облаке, и локально. Логика намеренно отделена от I/O ради этой проверки.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)/ptest"

g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/control/include" \
    -I "$ROOT/components/comm/include" \
    "$ROOT/test/host/test_protocol.cpp" \
    "$ROOT/components/control/src/control.cpp" \
    "$ROOT/components/comm/src/protocol.cpp" \
    "$ROOT/components/comm/src/frame.cpp" \
    -o "$OUT"

"$OUT"

# Тест wavetable (чистый DSP осциллятора: формы, интерполяция, диапазон).
OUT2="$(mktemp -d)/wttest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_wavetable.cpp" \
    "$ROOT/components/audio/src/wavetable.cpp" \
    -o "$OUT2"

"$OUT2"
