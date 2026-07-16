// Package proto implements the UCSynth binary serial protocol (framing, CRC, message codec).
// Pure, no I/O, no GUI — host-testable, mirrors tools/serialtest.py and components/comm (frame.cpp,
// protocol.cpp). Wire contract: docs/serial-protocol.md.
package proto

// CRC16 computes CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout).
// Standard check value for "123456789" is 0x29B1 — asserted in tests for firmware parity.
func CRC16(data []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}
