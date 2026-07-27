package device

import (
	"io"
	"sync"
	"testing"
	"time"

	"ucsynth/app/proto"
)

// stalledConn models a device that stopped draining its USB endpoint: every Write blocks until
// Release is called. Read parks until Close so the reader goroutine neither spins nor errors the
// model out from under the test.
type stalledConn struct {
	mu       sync.Mutex
	written  []byte
	release  chan struct{}
	done     chan struct{}
	once     sync.Once
	parked   chan struct{} // closed once a Write is actually blocked
	parkOnce sync.Once
}

func newStalledConn() *stalledConn {
	return &stalledConn{
		release: make(chan struct{}),
		done:    make(chan struct{}),
		parked:  make(chan struct{}),
	}
}

func (s *stalledConn) Write(p []byte) (int, error) {
	s.parkOnce.Do(func() { close(s.parked) })
	select {
	case <-s.release:
	case <-s.done:
		return 0, io.ErrClosedPipe
	}
	s.mu.Lock()
	s.written = append(s.written, p...)
	s.mu.Unlock()
	return len(p), nil
}

func (s *stalledConn) Read([]byte) (int, error) { <-s.done; return 0, io.EOF }
func (s *stalledConn) Close() error             { s.once.Do(func() { close(s.done) }); return nil }
func (s *stalledConn) Release()                 { close(s.release) }

// noteOffsSent decodes the wire and returns the notes that were released.
func (s *stalledConn) noteOffsSent() map[uint8]bool {
	s.mu.Lock()
	raw := append([]byte(nil), s.written...)
	s.mu.Unlock()
	offs := map[uint8]bool{}
	for _, body := range proto.NewDecoder().Push(raw) {
		if len(body) >= 2 && body[0] == proto.CmdNoteOff {
			offs[body[1]] = true
		}
	}
	return offs
}

// A NOTE_OFF that cannot be queued must NOT be forgotten. The frame itself is allowed to be dropped
// (the link is stalled, nothing can be done about that), but the note has to stay in the held set so
// the Panic button can release it once the link recovers. Clearing held first loses the note twice
// and leaves the firmware droning with no way back short of a reboot.
func TestNoteOffSurvivesStalledWriter(t *testing.T) {
	conn := newStalledConn()
	d := New(conn, nil)
	d.Start()
	defer d.Close()

	// Wait until the writer is genuinely parked inside Write (on the LIST frame) before queueing any
	// notes. Otherwise it pops one note as it gets scheduled, leaving a free slot that makes the
	// "queue is full" premise of this test flaky.
	select {
	case <-conn.parked:
	case <-time.After(2 * time.Second):
		t.Fatal("writer never reached Write")
	}

	d.NoteOn(60, 100)
	for i := 0; i < 60; i++ { // overflow noteCh (cap 32) with unrelated notes
		d.NoteOn(uint8(70+i%40), 100)
	}
	if got := len(d.noteCh); got != cap(d.noteCh) {
		t.Fatalf("noteCh depth %d, want it saturated at %d", got, cap(d.noteCh))
	}

	start := time.Now()
	d.NoteOff(60)
	if waited := time.Since(start); waited < noteOffWait {
		t.Fatalf("NoteOff returned after %v, expected it to wait ~%v for queue room", waited, noteOffWait)
	}
	if conn.noteOffsSent()[60] {
		t.Fatal("NOTE_OFF reached the wire although the writer is stalled — test setup is wrong")
	}

	// Link recovers, user hits Panic. Note 60 must still be known as held, and get released.
	conn.Release()
	d.AllNotesOff()
	waitFor(t, "NOTE_OFF 60 on the wire after Panic", func() bool { return conn.noteOffsSent()[60] })
}

// The happy path: a NOTE_OFF that goes through must clear the note, so Panic does not re-send it.
func TestNoteOffClearsHeldOnSuccess(t *testing.T) {
	conn := newStalledConn()
	conn.Release() // writes flow freely
	d := New(conn, nil)
	d.Start()
	defer d.Close()

	d.NoteOn(60, 100)
	d.NoteOff(60)
	waitFor(t, "NOTE_OFF 60 on the wire", func() bool { return conn.noteOffsSent()[60] })

	d.mu.RLock()
	held := len(d.held)
	d.mu.RUnlock()
	if held != 0 {
		t.Fatalf("held has %d notes after a successful NOTE_OFF, want 0", held)
	}
}
