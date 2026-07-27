package proto

// Frame layout: [0x55][0xAA][LEN:u8][BODY: LEN bytes][CRC16_LE:u16].
// CRC covers (LEN + BODY), little-endian. See frame.cpp / serialtest.py.
const (
	Sync0   = 0x55
	Sync1   = 0xAA
	MaxBody = 255
)

// EncodeFrame wraps a body (0..255 bytes) into a full frame. Panics only above MaxBody — callers
// build bodies from fixed message layouts, so an oversized length is a programming error.
// A zero-length body carries no opcode and is useless, but it is legal on the wire: the firmware
// both emits and accepts LEN=0, so panicking here would crash anything that round-trips a received
// body back through the encoder (a proxy, a logger, the Fake echoing).
func EncodeFrame(body []byte) []byte {
	n := len(body)
	if n > MaxBody {
		panic("proto: body length out of range")
	}
	head := make([]byte, 0, 1+n) // LEN + BODY (what the CRC covers)
	head = append(head, byte(n))
	head = append(head, body...)
	crc := CRC16(head)

	out := make([]byte, 0, 2+len(head)+2)
	out = append(out, Sync0, Sync1)
	out = append(out, head...)
	out = append(out, byte(crc), byte(crc>>8)) // little-endian
	return out
}

// Decoder extracts frame bodies from a byte stream, skipping ASCII log lines and garbage
// (they fail CRC). Buffer-resync model ported from serialtest.py (NOT the firmware byte state
// machine): on a false sync, drop 2 bytes and rescan. Streaming-safe: Read chunks ≠ frames.
type Decoder struct {
	buf []byte
	max int
}

// NewDecoder returns a decoder with a bounded accumulator (a garbage stream can't grow memory).
func NewDecoder() *Decoder { return &Decoder{max: 64 * 1024} }

// Push feeds bytes and returns any complete frame bodies decoded (may be empty). The returned
// slices are owned by the caller (copied out of the internal buffer).
func (d *Decoder) Push(data []byte) [][]byte {
	d.buf = append(d.buf, data...)
	var out [][]byte

	for {
		i := indexSync(d.buf)
		if i < 0 {
			// No full sync. Keep only a possible partial sync (last byte).
			if len(d.buf) > 1 {
				d.buf = d.buf[len(d.buf)-1:]
			}
			break
		}
		if i > 0 {
			d.buf = d.buf[i:] // drop junk/log before the sync
		}
		if len(d.buf) < 3 {
			break // need sync(2) + LEN
		}
		switch n := frameAt(d.buf, 0); {
		case n > 0:
			body := make([]byte, int(d.buf[2]))
			copy(body, d.buf[3:3+len(body)])
			out = append(out, body)
			d.buf = d.buf[n:]
			continue
		case n == 0:
			d.buf = d.buf[2:] // false sync — resync
			continue
		}
		// Incomplete. Before waiting, check whether a complete, CRC-valid frame is already sitting
		// further along: if so this sync is false and waiting on it would stall a frame we already
		// have (indefinitely, if the stream goes quiet).
		if adv := nextValidFrame(d.buf); adv > 0 {
			d.buf = d.buf[adv:]
			continue
		}
		break // genuinely waiting for the rest of the frame
	}

	if len(d.buf) > d.max {
		d.buf = d.buf[len(d.buf)-d.max:]
	}
	return out
}

func indexSync(b []byte) int { return indexSyncFrom(b, 0) }

func indexSyncFrom(b []byte, from int) int {
	for i := from; i+1 < len(b); i++ {
		if b[i] == Sync0 && b[i+1] == Sync1 {
			return i
		}
	}
	return -1
}

// frameAt reports the total frame length at b[off] (>0) when the CRC checks out, 0 when it does
// not, and -1 when there are not enough bytes yet to tell.
func frameAt(b []byte, off int) int {
	if len(b)-off < 3 {
		return -1
	}
	length := int(b[off+2])
	need := 3 + length + 2
	if len(b)-off < need {
		return -1
	}
	head := b[off+2 : off+3+length] // LEN + BODY
	crcRx := uint16(b[off+3+length]) | uint16(b[off+4+length])<<8
	if CRC16(head) != crcRx {
		return 0
	}
	return need
}

// nextValidFrame returns the offset of the first complete CRC-valid frame starting after b[0], or
// -1 if there is none. Used to step over a false sync instead of blocking behind it.
func nextValidFrame(b []byte) int {
	for j := indexSyncFrom(b, 1); j > 0; j = indexSyncFrom(b, j+1) {
		if frameAt(b, j) > 0 {
			return j
		}
	}
	return -1
}
