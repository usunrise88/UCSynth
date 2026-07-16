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

# Тест env (ADSR: сегменты, фронты gate, ретригер, loop).
OUT3="$(mktemp -d)/envtest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_env.cpp" \
    "$ROOT/components/audio/src/env.cpp" \
    -o "$OUT3"

"$OUT3"

# Тест filter (ZDF SVF: АЧХ LP/HP/BP, OFF, устойчивость при резонансе).
OUT4="$(mktemp -d)/filttest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_filter.cpp" \
    "$ROOT/components/audio/src/filter.cpp" \
    -o "$OUT4"

"$OUT4"

# Тест voice (голос целиком: нота/release, headroom, lo-fi, latch, glide).
OUT5="$(mktemp -d)/voicetest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_voice.cpp" \
    "$ROOT/components/audio/src/voice.cpp" \
    "$ROOT/components/audio/src/env.cpp" \
    "$ROOT/components/audio/src/waveenv.cpp" \
    "$ROOT/components/audio/src/filter.cpp" \
    "$ROOT/components/audio/src/wavetable.cpp" \
    -o "$OUT5"

"$OUT5"

# Тест synth (полифония: аллокация/steal/reuse, моно-стек, legato, сумма).
OUT6="$(mktemp -d)/synthtest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_synth.cpp" \
    "$ROOT/components/audio/src/synth.cpp" \
    "$ROOT/components/audio/src/voice.cpp" \
    "$ROOT/components/audio/src/env.cpp" \
    "$ROOT/components/audio/src/waveenv.cpp" \
    "$ROOT/components/audio/src/filter.cpp" \
    "$ROOT/components/audio/src/wavetable.cpp" \
    -o "$OUT6"

"$OUT6"

# Тест lfo (этап 4.1 — формы LFO, диапазон, фаза, S&H hold/change).
OUT7="$(mktemp -d)/lfotest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_lfo.cpp" \
    "$ROOT/components/audio/src/lfo.cpp" \
    -o "$OUT7"

"$OUT7"

# Тест matrix (этап 4.1 — мод-матрица: источник/приёмник/глубина, аккумуляция, гард NONE, velocity).
OUT8="$(mktemp -d)/matrixtest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_matrix.cpp" \
    "$ROOT/components/audio/src/voice.cpp" \
    "$ROOT/components/audio/src/env.cpp" \
    "$ROOT/components/audio/src/waveenv.cpp" \
    "$ROOT/components/audio/src/filter.cpp" \
    "$ROOT/components/audio/src/wavetable.cpp" \
    -o "$OUT8"

"$OUT8"

# Тест morph (этап 4.2 — wavetable-морф: кроссфейд форм, фаз-когерентность, клампы, диапазон).
OUT9="$(mktemp -d)/morphtest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_morph.cpp" \
    "$ROOT/components/audio/src/wavetable.cpp" \
    -o "$OUT9"

"$OUT9"

# Тест waveenv (этап 4.2 — wave-огибающая: точки, one-shot/hold, loop, rate, диапазон).
OUT10="$(mktemp -d)/waveenvtest"
g++ -std=c++17 -Wall -Wextra -O2 \
    -I "$ROOT/components/audio/src" \
    "$ROOT/test/host/test_waveenv.cpp" \
    "$ROOT/components/audio/src/waveenv.cpp" \
    -o "$OUT10"

"$OUT10"
