// Package device is the client-side model of the synth: it builds a param registry from LIST,
// serializes all frame writes through one priority-aware writer (notes jump ahead of SET bursts
// and STAT polls), coalesces SETs, applies VALUE/STAT echoes, and exposes an immutable Snapshot
// for the UI. It depends only on an io.ReadWriteCloser, so tests drive it against a fake firmware
// over net.Pipe — no serial port, no hardware, no GUI. Pure logic; no Gio import.
package device

import (
	"io"
	"sync"
	"time"

	"ucsynth/app/proto"
)

type State int

const (
	Connecting State = iota
	Synced
	Disconnected
	Errored
)

func (s State) String() string {
	switch s {
	case Connecting:
		return "подключение…"
	case Synced:
		return "подключено"
	case Disconnected:
		return "отключено"
	case Errored:
		return "ошибка"
	}
	return "?"
}

// Snapshot is an immutable view of the model for one UI frame.
type Snapshot struct {
	State  State
	Err    error
	Params []proto.Param // discovery (id) order
	Stat   proto.Stat
}

// Param returns the param with the given id and whether it was found.
func (s Snapshot) Param(id uint16) (proto.Param, bool) {
	for _, p := range s.Params {
		if p.ID == id {
			return p, true
		}
	}
	return proto.Param{}, false
}

const (
	setFlushHz = 40                     // SET coalesce flush rate
	statPollMs = 500                    // STAT poll cadence (~2 Hz)
	closeGrace = 30 * time.Millisecond  // let the writer flush NOTE_OFFs before teardown
)

type Device struct {
	conn     io.ReadWriteCloser
	dec      *proto.Decoder
	onChange func()

	mu     sync.RWMutex
	order  []uint16
	params map[uint16]proto.Param
	stat   proto.Stat
	state  State
	err    error
	held   map[uint8]bool

	noteCh  chan []byte
	frameCh chan []byte

	pendMu  sync.Mutex
	pending map[uint16]float32

	done      chan struct{}
	closeOnce sync.Once
	wg        sync.WaitGroup
}

// New wraps a byte stream. onChange (may be nil) is called whenever the model changes — the UI
// sets it to window.Invalidate. Call Start to begin.
func New(conn io.ReadWriteCloser, onChange func()) *Device {
	if onChange == nil {
		onChange = func() {}
	}
	return &Device{
		conn:     conn,
		dec:      proto.NewDecoder(),
		onChange: onChange,
		params:   map[uint16]proto.Param{},
		held:     map[uint8]bool{},
		noteCh:   make(chan []byte, 32),
		frameCh:  make(chan []byte, 64),
		pending:  map[uint16]float32{},
		done:     make(chan struct{}),
		state:    Connecting,
	}
}

// Start launches the reader/writer goroutines and requests the registry (LIST).
func (d *Device) Start() {
	d.wg.Add(2)
	go d.reader()
	go d.writer()
	d.enqueue(d.frameCh, proto.ListFrame())
}

// --- public control ---

// SetParam queues a value change (coalesced latest-per-id, flushed at setFlushHz).
func (d *Device) SetParam(id uint16, val float32) {
	d.pendMu.Lock()
	d.pending[id] = val
	d.pendMu.Unlock()
}

// Refresh re-reads one param's value (GET) — for external-change reflection (no push in v1).
func (d *Device) Refresh(id uint16) { d.enqueue(d.frameCh, proto.GetFrame(id)) }

func (d *Device) NoteOn(note, vel uint8) {
	d.mu.Lock()
	d.held[note] = true
	d.mu.Unlock()
	d.enqueue(d.noteCh, proto.NoteOnFrame(note, vel))
}

func (d *Device) NoteOff(note uint8) {
	d.mu.Lock()
	delete(d.held, note)
	d.mu.Unlock()
	d.enqueue(d.noteCh, proto.NoteOffFrame(note))
}

// AllNotesOff sends NOTE_OFF for every currently-held note (panic; no all-off opcode exists).
func (d *Device) AllNotesOff() {
	d.mu.Lock()
	notes := make([]uint8, 0, len(d.held))
	for n := range d.held {
		notes = append(notes, n)
	}
	d.held = map[uint8]bool{}
	d.mu.Unlock()
	for _, n := range notes {
		d.enqueue(d.noteCh, proto.NoteOffFrame(n))
	}
}

// Snapshot returns an immutable view for the current UI frame.
func (d *Device) Snapshot() Snapshot {
	d.mu.RLock()
	defer d.mu.RUnlock()
	ps := make([]proto.Param, 0, len(d.order))
	for _, id := range d.order {
		ps = append(ps, d.params[id])
	}
	return Snapshot{State: d.state, Err: d.err, Params: ps, Stat: d.stat}
}

// Close flushes held notes, stops the goroutines and closes the connection.
func (d *Device) Close() error {
	d.AllNotesOff()
	time.Sleep(closeGrace) // give the priority writer a moment to emit the NOTE_OFFs
	d.stop(Disconnected, nil)
	err := d.conn.Close() // unblocks the reader's Read
	d.wg.Wait()
	return err
}

// --- internals ---

func (d *Device) reader() {
	defer d.wg.Done()
	buf := make([]byte, 1024)
	for {
		n, err := d.conn.Read(buf)
		if n > 0 {
			changed := false
			for _, body := range d.dec.Push(buf[:n]) {
				if d.handle(body) {
					changed = true
				}
			}
			if changed {
				d.onChange()
			}
		}
		if err != nil {
			d.stop(Errored, err)
			return
		}
	}
}

func (d *Device) handle(body []byte) bool {
	switch proto.Opcode(body) {
	case proto.RspParam:
		p, err := proto.ParseParam(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		if _, ok := d.params[p.ID]; !ok {
			d.order = append(d.order, p.ID)
		}
		d.params[p.ID] = p
		d.mu.Unlock()
		return true
	case proto.RspListEnd:
		d.mu.Lock()
		if d.state == Connecting {
			d.state = Synced
		}
		d.mu.Unlock()
		return true
	case proto.RspValue:
		v, err := proto.ParseValue(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		if p, ok := d.params[v.ID]; ok {
			p.Cur = v.Val
			d.params[v.ID] = p
		}
		d.mu.Unlock()
		return true
	case proto.RspStat:
		s, err := proto.ParseStat(body)
		if err != nil {
			return false
		}
		d.mu.Lock()
		d.stat = s
		d.mu.Unlock()
		return true
	default:
		return false // ACK / ERR — no model change (ERR has no request context; ignore)
	}
}

func (d *Device) writer() {
	defer d.wg.Done()
	setFlush := time.NewTicker(time.Second / setFlushHz)
	statPoll := time.NewTicker(statPollMs * time.Millisecond)
	defer setFlush.Stop()
	defer statPoll.Stop()

	for {
		// Priority: drain notes ahead of everything else.
		select {
		case <-d.done:
			d.drainNotes()
			return
		case f := <-d.noteCh:
			d.write(f)
		default:
			select {
			case <-d.done:
				d.drainNotes()
				return
			case f := <-d.noteCh:
				d.write(f)
			case f := <-d.frameCh:
				d.write(f)
			case <-setFlush.C:
				d.flushSets()
			case <-statPoll.C:
				d.write(proto.StatFrame())
			}
		}
	}
}

// drainNotes emits any queued NOTE_OFFs at shutdown (best effort; ignore write errors).
func (d *Device) drainNotes() {
	for {
		select {
		case f := <-d.noteCh:
			_, _ = d.conn.Write(f)
		default:
			return
		}
	}
}

func (d *Device) flushSets() {
	d.pendMu.Lock()
	if len(d.pending) == 0 {
		d.pendMu.Unlock()
		return
	}
	frames := make([][]byte, 0, len(d.pending))
	for id, v := range d.pending {
		frames = append(frames, proto.SetFrame(id, v))
	}
	d.pending = map[uint16]float32{}
	d.pendMu.Unlock()
	for _, f := range frames {
		d.write(f)
	}
}

func (d *Device) write(f []byte) {
	if _, err := d.conn.Write(f); err != nil {
		d.stop(Errored, err)
	}
}

func (d *Device) enqueue(ch chan []byte, f []byte) {
	select {
	case ch <- f:
	case <-d.done:
	default: // channel full — drop (notes are transient; SETs go via the pending map)
	}
}

func (d *Device) stop(state State, err error) {
	d.closeOnce.Do(func() {
		d.mu.Lock()
		d.state = state
		if err != nil {
			d.err = err
		}
		d.mu.Unlock()
		close(d.done)
	})
	d.onChange()
}
