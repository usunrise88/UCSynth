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

    steps = [
        ("LIST",                    encode(bytes([LIST]))),
        ("GET master_volume",       encode(bytes([GET]) + struct.pack("<H", 0))),
        ("SET master_volume = 0.5", encode(bytes([SET]) + struct.pack("<Hf", 0, 0.5))),
        ("GET master_volume",       encode(bytes([GET]) + struct.pack("<H", 0))),
        ("SET master_volume = 9 (ждём кламп 1.0)", encode(bytes([SET]) + struct.pack("<Hf", 0, 9.0))),
        ("STAT",                    encode(bytes([STAT]))),
        ("NOTE_ON 60 100",          encode(bytes([NOTE_ON, 60, 100]))),
        ("SET test_tone_hz = 220 (октава вниз)", encode(bytes([SET]) + struct.pack("<Hf", 1, 220.0))),
        ("SET waveform = 1 (saw)",    encode(bytes([SET]) + struct.pack("<Hf", 2, 1.0))),
        ("SET waveform = 2 (square)", encode(bytes([SET]) + struct.pack("<Hf", 2, 2.0))),
        ("SET waveform = 3 (tri)",    encode(bytes([SET]) + struct.pack("<Hf", 2, 3.0))),
        ("SET waveform = 0 (sine)",   encode(bytes([SET]) + struct.pack("<Hf", 2, 0.0))),
        ("SET test_tone_hz = 440 (вернуть)", encode(bytes([SET]) + struct.pack("<Hf", 1, 440.0))),
        ("SET master_volume = 0.8 (вернуть дефолт)", encode(bytes([SET]) + struct.pack("<Hf", 0, 0.8))),
    ]
    for i, (title, frame) in enumerate(steps):
        print(f"{title}:")
        ser.write(frame)
        drain(ser, dec)
        if i < len(steps) - 1:
            time.sleep(2.0)   # пауза между сменами — успеть глянуть на экран / послушать
    ser.close()


if __name__ == "__main__":
    main()
