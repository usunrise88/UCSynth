package proto

// Frame layout: [0x55][0xAA][LEN:u8][BODY: LEN bytes][CRC16_LE:u16].
// CRC covers (LEN + BODY), little-endian. See frame.cpp / serialtest.py.
const (
	Sync0   = 0x55
	Sync1   = 0xAA
	MaxBody = 255
)

// EncodeFrame wraps a body (1..255 bytes) into a full frame. Panics on out-of-range length —
// callers build bodies from fixed message layouts, so a bad length is a programming error.
func EncodeFrame(body []byte) []byte {
	n := len(body)
	if n < 1 || n > MaxBody {
		panic("proto: body length out of range")
	}
	head := make([]byte, 0, 1+n)      // LEN + BODY (what the CRC covers)
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
		length := int(d.buf[2])
		need := 3 + length + 2
		if len(d.buf) < need {
			break // wait for the rest of the frame
		}
		head := d.buf[2 : 3+length] // LEN + BODY
		crcRx := uint16(d.buf[3+length]) | uint16(d.buf[3+length+1])<<8
		if CRC16(head) == crcRx {
			body := make([]byte, length)
			copy(body, d.buf[3:3+length])
			out = append(out, body)
			d.buf = d.buf[need:]
		} else {
			d.buf = d.buf[2:] // false sync — resync
		}
	}

	if len(d.buf) > d.max {
		d.buf = d.buf[len(d.buf)-d.max:]
	}
	return out
}

func indexSync(b []byte) int {
	for i := 0; i+1 < len(b); i++ {
		if b[i] == Sync0 && b[i+1] == Sync1 {
			return i
		}
	}
	return -1
}
