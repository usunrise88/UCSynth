package seq

import (
	"fmt"
	"sort"
	"sync"
	"testing"
	"time"
)

// tracker records the last on/off seen per note and can park the caller inside a NOTE_ON, which is
// what makes the Stop-vs-advance race reproducible.
type tracker struct {
	mu        sync.Mutex
	sounding  map[int]bool
	order     []string // full emit trace, for a readable failure message
	ons       int
	entered   chan struct{}
	enterOnce sync.Once
	hold      chan struct{}
}

func newTracker() *tracker {
	return &tracker{sounding: map[int]bool{}, entered: make(chan struct{}), hold: make(chan struct{})}
}

func (tr *tracker) emit(note int, on bool) {
	tr.mu.Lock()
	tr.sounding[note] = on
	tr.order = append(tr.order, fmt.Sprintf("%d=%v", note, on))
	if on {
		tr.ons++
	}
	tr.mu.Unlock()
	if on {
		tr.enterOnce.Do(func() { close(tr.entered) })
		<-tr.hold // park inside the first NOTE_ON; Stop must not slip past us
	}
}

func (tr *tracker) onCount() int {
	tr.mu.Lock()
	defer tr.mu.Unlock()
	return tr.ons
}

func (tr *tracker) stillSounding() ([]int, []string) {
	tr.mu.Lock()
	defer tr.mu.Unlock()
	var out []int
	for n, on := range tr.sounding {
		if on {
			out = append(out, n)
		}
	}
	sort.Ints(out)
	return out, append([]string(nil), tr.order...)
}

// Stop must not release notes that advance has recorded but not yet sounded. If it does, advance
// sounds them right afterwards with the transport already stopped — and since no further tick will
// come, nothing ever releases them: a permanent drone that only Panic can clear.
func TestStopDoesNotStrandNotesRacingAdvance(t *testing.T) {
	tr := newTracker()
	p := New(4, 60, 62, 300, tr.emit, nil) // 300 BPM → 50 ms per step
	p.Toggle(0, 60)
	p.Toggle(0, 61)

	p.Start() // real transport, not a hand-set `playing` flag
	select {
	case <-tr.entered:
	case <-time.After(2 * time.Second):
		t.Fatal("clock never fired the first step")
	}

	stopped := make(chan struct{})
	go func() { p.Stop(); close(stopped) }()
	time.Sleep(20 * time.Millisecond) // window in which the buggy Stop would emit its NOTE_OFFs
	close(tr.hold)                    // let advance finish sounding the chord

	select {
	case <-stopped:
	case <-time.After(2 * time.Second):
		t.Fatal("Stop did not return")
	}
	// Wait for advance to finish emitting too — the whole point is what the *last* write leaves
	// behind, and asserting the instant Stop returns would race advance's second NOTE_ON.
	deadline := time.Now().Add(2 * time.Second)
	for tr.onCount() < 2 && time.Now().Before(deadline) {
		time.Sleep(2 * time.Millisecond)
	}

	if left, trace := tr.stillSounding(); len(left) != 0 {
		t.Fatalf("notes left sounding after Stop: %v (emit trace: %v)", left, trace)
	}
	if p.Playing() {
		t.Fatal("player still reports playing after Stop")
	}
}

// Start/Stop through the public API: no goroutine or channel left behind, and a second Stop is a
// no-op rather than a close-of-nil-channel panic.
func TestStartStopIdempotent(t *testing.T) {
	p := New(4, 60, 62, 300, func(int, bool) {}, nil)
	p.Start()
	if !p.Playing() {
		t.Fatal("Playing() false right after Start")
	}
	p.Start() // second Start must not replace the clock
	p.Stop()
	if p.Playing() {
		t.Fatal("Playing() true after Stop")
	}
	p.Stop() // must not panic
	if p.Cur() != -1 {
		t.Fatalf("cur should reset to -1 after Stop, got %d", p.Cur())
	}
}

// Changing tempo must not restart the pattern. SetBPM used to Stop()+Start(), which reset cur to −1
// and released every sounding note — nudging the tempo on step 11 of 16 jumped back to step 0 with the
// sound cut, and it opened the Stop-vs-advance window on every click.
func TestSetBPMKeepsPosition(t *testing.T) {
	var mu sync.Mutex
	var ons int
	p := New(16, 60, 60, 300, func(_ int, on bool) {
		if on {
			mu.Lock()
			ons++
			mu.Unlock()
		}
	}, nil)
	for s := 0; s < 16; s++ {
		p.Toggle(s, 60)
	}

	p.Start()
	defer p.Stop()

	// Let it walk a few steps.
	deadline := time.Now().Add(2 * time.Second)
	for p.Cur() < 3 && time.Now().Before(deadline) {
		time.Sleep(2 * time.Millisecond)
	}
	before := p.Cur()
	if before < 3 {
		t.Fatalf("clock did not advance past step 3 (got %d)", before)
	}

	p.SetBPM(200)
	if !p.Playing() {
		t.Fatal("SetBPM stopped the transport")
	}
	if got := p.Cur(); got < before {
		t.Fatalf("SetBPM rewound the pattern: step %d → %d", before, got)
	}
	if p.BPM() != 200 {
		t.Fatalf("BPM not applied: %d", p.BPM())
	}

	// And the new interval is actually picked up: the pattern keeps advancing.
	deadline = time.Now().Add(2 * time.Second)
	for p.Cur() == before && time.Now().Before(deadline) {
		time.Sleep(2 * time.Millisecond)
	}
	if p.Cur() == before {
		t.Fatal("clock stopped advancing after SetBPM")
	}
}
