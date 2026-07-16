#!/usr/bin/env python3
"""Референс-клиент бинарного протокола UCSynth (этап 0.3).

Голым терминалом бинарь не набрать — этим скриптом проверяем 0.3 на железе.
Нужен pyserial:  pip install pyserial
Запуск:          python tools/serialtest.py COM8
                 (порт НАТИВНОГО USB S3 — где идут логи; не CH343-мост.)
Протокол и раскладки — docs/serial-protocol.md.
"""
import struct
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("нужен pyserial:  pip install pyserial")

SYNC = b"\x55\xAA"

# CMD (ПК→МК)
SET, GET, LIST, NOTE_ON, NOTE_OFF, STAT = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
# RSP (МК→ПК)
ACK, VALUE, PARAM, LISTEND, R_STAT, ERR = 0x80, 0x81, 0x82, 0x83, 0x86, 0xFF
TYPE_NAMES = {0: "float", 1: "int", 2: "enum", 3: "bool"}
ERR_NAMES = {1: "unknown_cmd", 2: "bad_id", 3: "bad_len"}


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) — как в прошивке (frame.cpp)."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(body: bytes) -> bytes:
    assert 0 < len(body) <= 255
    head = bytes([len(body)]) + body           # LEN + BODY
    return SYNC + head + struct.pack("<H", crc16(head))


class Decoder:
    """Выделяет кадры из потока: пропускает ASCII-логи и мусор (не проходят CRC)."""

    def __init__(self):
        self.buf = bytearray()

    def push(self, data: bytes):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                del self.buf[:max(0, len(self.buf) - 1)]   # хвост может быть началом синка
                break
            if i:
                del self.buf[:i]                            # выбросить лог/мусор до синка
            if len(self.buf) < 3:
                break
            length = self.buf[2]
            need = 3 + length + 2
            if len(self.buf) < need:
                break
            head = bytes(self.buf[2:3 + length])
            crc_rx = struct.unpack_from("<H", self.buf, 3 + length)[0]
            if crc16(head) == crc_rx:
                out.append(bytes(self.buf[3:3 + length]))
                del self.buf[:need]
            else:
                del self.buf[:2]                            # ложный синк — ресинк
        return out


def show(frame: bytes):
    op = frame[0]
    if op == VALUE:
        pid = struct.unpack_from("<H", frame, 1)[0]
        val = struct.unpack_from("<f", frame, 3)[0]
        print(f"  VALUE id={pid} val={val:g}")
    elif op == PARAM:
        pid, typ = struct.unpack_from("<HB", frame, 1)
        mn, mx, df, cur = struct.unpack_from("<ffff", frame, 4)
        nl = frame[20]
        name = frame[21:21 + nl].decode("utf-8", "replace")
        print(f"  PARAM id={pid} {name} type={TYPE_NAMES.get(typ, typ)} "
              f"min={mn:g} max={mx:g} def={df:g} cur={cur:g}")
    elif op == LISTEND:
        print(f"  LISTEND count={struct.unpack_from('<H', frame, 1)[0]}")
    elif op == R_STAT:
        heap, mn, up, cpu, ur = struct.unpack_from("<IIIII", frame, 1)
        print(f"  STAT heap={heap} minheap={mn} uptime_ms={up} cpu={cpu/10:.1f}% underruns={ur}")
    elif op == ACK:
        print("  ACK")
    elif op == ERR:
        print(f"  ERR {ERR_NAMES.get(frame[1], frame[1])}")
    else:
        print(f"  ? op=0x{op:02X} {frame.hex()}")


def drain(ser, dec, seconds=0.3):
    end = time.time() + seconds
    while time.time() < end:
        n = ser.in_waiting
        if n:
            for f in dec.push(ser.read(n)):
                show(f)
            end = time.time() + 0.15         # пока сыпется — продлеваем
        else:
            time.sleep(0.01)


def main():
    if len(sys.argv) < 2:
        sys.exit("Использование: python serialtest.py <PORT>   (напр. COM8 или /dev/ttyACM0)")
    # Нативный USB-Serial-JTAG S3 использует RTS как линию сброса (EN) — тем же механизмом
    # esptool перезагружает плату по USB. По умолчанию pyserial дёргает RTS при открытии/закрытии
    # порта → чип ресетится в конце скрипта. Держим RTS сброшенным: сброса не будет, на данные
    # (bulk-endpoints CDC) это не влияет.
    ser = serial.Serial()
    ser.port = sys.argv[1]
    ser.baudrate = 115200        # для USB-JTAG не важен
    ser.timeout = 0.1
    ser.rts = False
    ser.open()
    dec = Decoder()
    time.sleep(0.2)
    ser.reset_input_buffer()

    # id параметров (стабильные, см. control.h). LIST даёт их динамически — тут для удобства.
    PID_VOL, PID_HZ, PID_WAVE, PID_TEST = 0, 1, 2, 3
    PID_AMP_A, PID_AMP_D, PID_AMP_S, PID_AMP_R, PID_LATCH, PID_AMP_LOOP = 4, 5, 6, 7, 8, 9
    PID_OSC1_LVL, PID_OSC1_DET = 10, 11
    PID_OSC2_W, PID_OSC2_LVL, PID_OSC2_DET = 12, 13, 14
    PID_OSC3_W, PID_OSC3_LVL, PID_OSC3_DET = 15, 16, 17
    PID_NOISE, PID_RING = 18, 19
    PID_CUTOFF, PID_RES, PID_FMODE = 20, 21, 22
    PID_FA, PID_FD, PID_FS, PID_FR, PID_FENV, PID_FLOOP = 23, 24, 25, 26, 27, 28
    PID_LOFI, PID_LOFI_BITS = 29, 30
    PID_POLY, PID_GLIDE, PID_LEGATO = 31, 32, 33

    def setp(pid, val):
        return encode(bytes([SET]) + struct.pack("<Hf", pid, val))

    def step(title, frame, pause=2.0):
        """Команда + дренаж ответа + пауза (успеть глянуть на экран / послушать)."""
        print(f"{title}:")
        ser.write(frame)
        drain(ser, dec)
        time.sleep(pause)

    def play(title, note, hold=1.3, vel=100):
        """Сыграть ноту: NOTE_ON → держим hold с → NOTE_OFF. Голос моно (этап 3.0)."""
        print(f"{title}:")
        ser.write(encode(bytes([NOTE_ON, note, vel])))
        drain(ser, dec)
        time.sleep(hold)
        ser.write(encode(bytes([NOTE_OFF, note])))
        drain(ser, dec, 0.2)
        time.sleep(0.6)

    step("LIST (реестр — теперь 4 параметра, добавился test_tone)", encode(bytes([LIST])))
    step("STAT", encode(bytes([STAT])))

    # Тест-тон по умолчанию ВКЛ — тон звучит с загрузки (проверка тракта). Гасим → играют ноты.
    step("SET test_tone = 0 (тест-тон выкл → играют ноты NOTE_ON/OFF)", setp(PID_TEST, 0.0))
    step("SET waveform = 0 (sine)", setp(PID_WAVE, 0.0))

    # Три октавы A — слышно, что нота задаёт высоту (220 / 440 / 880 Гц).
    play("NOTE A3 (57 ≈ 220 Гц)", 57)
    play("NOTE A4 (69 = 440 Гц)", 69)
    play("NOTE A5 (81 ≈ 880 Гц)", 81)

    # Пила: низкая и ВЫСОКАЯ нота — на верхах band-limit убирает алиасинг (наивная пила «звенела» бы).
    step("SET waveform = 1 (saw)", setp(PID_WAVE, 1.0))
    play("NOTE C4 (60 ≈ 262 Гц) пилой", 60)
    play("NOTE C7 (96 ≈ 2093 Гц) пилой — band-limit: чисто, без алиасинга", 96)

    # Меандр на верхах — та же проверка.
    step("SET waveform = 2 (square)", setp(PID_WAVE, 2.0))
    play("NOTE C6 (84 ≈ 1047 Гц) меандром", 84)

    step("SET waveform = 0 (sine, вернуть)", setp(PID_WAVE, 0.0))

    # --- этап 3.1: ADSR (VCA) ---
    print("\n=== 3.1 ADSR (VCA) ===")
    step("pluck: attack 0.005 / decay 0.15 / sustain 0 / release 0.1",
         setp(PID_AMP_A, 0.005))
    step("  decay 0.15", setp(PID_AMP_D, 0.15))
    step("  sustain 0", setp(PID_AMP_S, 0.0))
    step("  release 0.1", setp(PID_AMP_R, 0.1))
    play("NOTE C4 (60) пиццикато — быстро гаснет", 60, hold=0.15)
    play("NOTE E4 (64) пиццикато", 64, hold=0.15)
    step("pad: attack 0.6 / release 0.8 / sustain 0.8", setp(PID_AMP_A, 0.6))
    step("  sustain 0.8", setp(PID_AMP_S, 0.8))
    step("  release 0.8", setp(PID_AMP_R, 0.8))
    play("NOTE C4 (60) пэд — плавный вход/выход", 60, hold=1.5)

    # --- drone: latch ---
    print("\n=== 3.1 drone (latch) ===")
    step("latch = 1", setp(PID_LATCH, 1.0))
    print("NOTE A3 (57) + latch — дрон держится после note-off:")
    ser.write(encode(bytes([NOTE_ON, 57, 100]))); drain(ser, dec)
    ser.write(encode(bytes([NOTE_OFF, 57]))); drain(ser, dec, 0.2)
    time.sleep(2.5)
    step("latch = 0 — дрон отпускается (релиз)", setp(PID_LATCH, 0.0))
    # вернуть организменный VCA-env для остальных демо
    step("VCA-env → орган (A 0.005 / S 1 / R 0.02)", setp(PID_AMP_A, 0.005))
    step("  sustain 1", setp(PID_AMP_S, 1.0)); step("  release 0.02", setp(PID_AMP_R, 0.02))

    # --- этап 3.2: фильтр + VCF-env ---
    print("\n=== 3.2 фильтр (ZDF SVF) + VCF-env ===")
    step("saw", setp(PID_WAVE, 1.0))
    step("cutoff 800 Гц (глухо)", setp(PID_CUTOFF, 800.0))
    play("NOTE C4 (60) — приглушённо (LP 800)", 60, hold=1.2)
    step("resonance 0.85", setp(PID_RES, 0.85))
    play("NOTE C4 (60) — с резонансом", 60, hold=1.2)
    step("VCF-env: env_amt 0.8 (свип вверх)", setp(PID_FENV, 0.8))
    step("  flt sustain 0 (свип вниз к cutoff)", setp(PID_FS, 0.0))
    step("  flt decay 0.4", setp(PID_FD, 0.4))
    play("NOTE C4 (60) — вау-свип фильтра по ноте", 60, hold=1.2)
    play("NOTE C3 (48) — свип ниже", 48, hold=1.2)
    step("фильтр назад: cutoff 20000 / res 0 / env_amt 0", setp(PID_CUTOFF, 20000.0))
    step("  res 0", setp(PID_RES, 0.0)); step("  env_amt 0", setp(PID_FENV, 0.0))
    step("  flt sustain 1", setp(PID_FS, 1.0))

    # --- этап 3.3: три осц + микшер ---
    print("\n=== 3.3 три осц + микшер ===")
    step("osc2 saw, уровень 0.8", setp(PID_OSC2_W, 1.0))
    step("  osc2 level 0.8", setp(PID_OSC2_LVL, 0.8))
    step("  osc2 detune +0.1 полутона (расстройка/густота)", setp(PID_OSC2_DET, 0.1))
    play("NOTE C4 (60) — 2 осц, лёгкий детюн (бьётся)", 60, hold=1.5)
    step("osc3 square, уровень 0.5, detune -0.12", setp(PID_OSC3_W, 2.0))
    step("  osc3 level 0.5", setp(PID_OSC3_LVL, 0.5))
    step("  osc3 detune -0.12", setp(PID_OSC3_DET, -0.12))
    play("NOTE C4 (60) — 3 осц, густой унисон", 60, hold=1.5)
    step("noise 0.25", setp(PID_NOISE, 0.25))
    play("NOTE C4 (60) — с шумом (дыхание)", 60, hold=1.2)
    step("noise 0", setp(PID_NOISE, 0.0))
    step("ring mod 0.7 (осц1×осц2)", setp(PID_RING, 0.7))
    play("NOTE C4 (60) — ring mod (металл)", 60, hold=1.2)
    step("ring 0 / osc2 level 0 / osc3 level 0 (назад к 1 осц)", setp(PID_RING, 0.0))
    step("  osc2 level 0", setp(PID_OSC2_LVL, 0.0)); step("  osc3 level 0", setp(PID_OSC3_LVL, 0.0))

    # --- этап 3.4: lo-fi ---
    print("\n=== 3.4 lo-fi ===")
    play("NOTE C6 (84) чистый (band-limit, для сравнения)", 84, hold=1.0)
    step("lofi = 1, bits = 4", setp(PID_LOFI, 1.0))
    step("  lofi_bits 4", setp(PID_LOFI_BITS, 4.0))
    play("NOTE C6 (84) lo-fi 4 бит — грязь + алиасинг", 84, hold=1.2)
    step("lofi_bits 2 (грубее)", setp(PID_LOFI_BITS, 2.0))
    play("NOTE C4 (60) lo-fi 2 бит", 60, hold=1.2)
    step("lofi = 0 (назад)", setp(PID_LOFI, 0.0))
    step("  lofi_bits 16", setp(PID_LOFI_BITS, 16.0))

    # --- этап 3.5: полифония + замер CPU ---
    print("\n=== 3.5 полифония (замер CPU на -O2) ===")
    step("SET waveform = 1 (saw — нагрузочная форма)", setp(PID_WAVE, 1.0))
    step("poly_voices = 8", setp(PID_POLY, 8.0))

    def chord(title, notes, hold=1.6):
        """Взять аккорд, снять STAT под нагрузкой, отпустить."""
        print(f"{title}:")
        for nn in notes:
            ser.write(encode(bytes([NOTE_ON, nn, 100])))
        drain(ser, dec)
        time.sleep(hold)
        ser.write(encode(bytes([STAT])))     # cpu под нагрузкой (ноты ещё звучат)
        drain(ser, dec)
        for nn in notes:
            ser.write(encode(bytes([NOTE_OFF, nn])))
        drain(ser, dec, 0.2)
        time.sleep(0.9)

    chord("1 нота (C4) + STAT",              [60])
    chord("аккорд 4 ноты (Cmaj7) + STAT",    [60, 64, 67, 71])
    chord("аккорд 8 нот + STAT (потолок)",   [48, 52, 55, 59, 60, 64, 67, 71])

    # --- этап 3.6: glide / legato ---
    print("\n=== 3.6 glide / legato ===")
    step("poly_voices = 1 (моно)", setp(PID_POLY, 1.0))
    step("glide_time = 0.15 с", setp(PID_GLIDE, 0.15))
    step("legato = 1 (моно-лид, без ретригера на перекрытии)", setp(PID_LEGATO, 1.0))
    print("mono-glide лид C4→E4→G4→C5 (ноты внахлёст → скольжение высоты):")
    for nn in (60, 64, 67, 72):
        ser.write(encode(bytes([NOTE_ON, nn, 100]))); drain(ser, dec); time.sleep(0.5)
    for nn in (60, 64, 67, 72):
        ser.write(encode(bytes([NOTE_OFF, nn])))
    drain(ser, dec, 0.2); time.sleep(1.0)

    step("legato = 0", setp(PID_LEGATO, 0.0))
    step("poly_voices = 4 (poly-glide)", setp(PID_POLY, 4.0))
    print("poly-glide: ноты скользят от ближайшего звучащего голоса:")
    for nn in (55, 59, 62, 67):
        ser.write(encode(bytes([NOTE_ON, nn, 100]))); drain(ser, dec); time.sleep(0.45)
    for nn in (55, 59, 62, 67):
        ser.write(encode(bytes([NOTE_OFF, nn])))
    drain(ser, dec, 0.2); time.sleep(1.0)

    step("glide_time = 0 (вернуть)", setp(PID_GLIDE, 0.0))
    step("poly_voices = 1 (вернуть моно)", setp(PID_POLY, 1.0))

    step("SET waveform = 0 (sine)", setp(PID_WAVE, 0.0))
    step("STAT (после всех демо — cpu/underruns)", encode(bytes([STAT])))

    # Вернуть отладочный тест-тон и дефолты.
    step("SET test_tone = 1 (тест-тон вернуть)", setp(PID_TEST, 1.0))
    step("SET test_tone_hz = 440", setp(PID_HZ, 440.0))
    step("SET master_volume = 0.8", setp(PID_VOL, 0.8), pause=0.3)
    ser.close()


if __name__ == "__main__":
    main()
