package device

import (
	"math"
	"net"
	"testing"
	"time"

	"ucsynth/app/proto"
)

// withSyncTiming shortens the registry-sync knobs so these tests run in milliseconds instead of
// seconds, and restores them afterwards.
func withSyncTiming(t *testing.T, timeout time.Duration, tries int) {
	t.Helper()
	oldTimeout, oldTries := listTimeout, listTries
	listTimeout, listTries = timeout, tries
	t.Cleanup(func() { listTimeout, listTries = oldTimeout, oldTries })
}

func waitUntil(t *testing.T, within time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for: %s", what)
}

// A LISTEND whose count exceeds the PARAMs actually received must not be accepted: the registry is
// short, and going Synced there is how a knob silently disappears from the rack. The client re-asks
// and comes up complete.
func TestShortRegistryTriggersListRetry(t *testing.T) {
	withSyncTiming(t, 150*time.Millisecond, 3)

	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	fake.DropNextParams(2, false) // first LIST loses two PARAM frames
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()

	// Before the retry fires: two params in, and explicitly NOT Synced.
	waitUntil(t, time.Second, "the two surviving PARAMs", func() bool { return len(d.Snapshot().Params) == 2 })
	if s := d.Snapshot(); s.State == Synced {
		t.Fatal("went Synced with a registry short of the LISTEND count")
	}

	waitUntil(t, 2*time.Second, "complete registry after retry", func() bool {
		s := d.Snapshot()
		return s.State == Synced && len(s.Params) == 4 && s.Missing == 0
	})
}

// If the shortfall never resolves, the client must still go live and report it rather than sit in
// "подключение…" forever with a fully alive board.
func TestPersistentShortRegistryGoesLiveWithMissing(t *testing.T) {
	withSyncTiming(t, 100*time.Millisecond, 3)

	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	fake.DropNextParams(1, true) // every LIST drops one
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()

	waitUntil(t, 3*time.Second, "gave up and reported the shortfall", func() bool {
		s := d.Snapshot()
		return s.State == Synced && s.Missing == 1 && len(s.Params) == 3
	})
}

// NaN off the wire must not reach a parameter. The firmware's clamp is the gate, and the Fake
// mirrors it; if either lets NaN through it poisons the audio path and recirculates in the FX
// feedback rings until reboot.
func TestSetParamNaNClamped(t *testing.T) {
	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()
	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })

	d.SetParam(20, float32(math.NaN())) // cutoff, range 20..20000
	waitUntil(t, 2*time.Second, "NaN clamped into range", func() bool {
		p, ok := d.Snapshot().Param(20)
		return ok && !math.IsNaN(float64(p.Cur)) && p.Cur >= p.Min && p.Cur <= p.Max
	})

	d.SetParam(20, float32(math.Inf(1)))
	waitUntil(t, 2*time.Second, "+Inf clamped to max", func() bool {
		p, ok := d.Snapshot().Param(20)
		return ok && p.Cur == p.Max
	})
}

// A board that stops answering with the port still open must be visible as stale, not as a healthy
// green "подключено" with frozen metrics.
func TestStaleWhenBoardGoesQuiet(t *testing.T) {
	oldStale := staleAfter
	staleAfter = 150 * time.Millisecond
	t.Cleanup(func() { staleAfter = oldStale })

	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()
	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })
	if d.Snapshot().Stale {
		t.Fatal("reported stale while the fake was answering")
	}

	fake.Mute()
	waitUntil(t, 2*time.Second, "stale after the board went quiet", func() bool { return d.Snapshot().Stale })
	if d.Snapshot().State != Synced {
		t.Fatal("stale must not change State — the port is still open")
	}
}

// RSP_ERR used to go straight to the floor, so a rejected SET looked like it had taken effect.
func TestErrCodeSurfaced(t *testing.T) {
	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()
	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })

	d.SetParam(9999, 1) // no such id → firmware answers ERR_BAD_ID
	waitUntil(t, 2*time.Second, "ERR_BAD_ID recorded", func() bool {
		return d.Snapshot().LastErr == proto.ErrBadID
	})
}
