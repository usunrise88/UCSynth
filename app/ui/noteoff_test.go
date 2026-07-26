package ui

import (
	"image"
	"testing"

	"gioui.org/f32"
	"gioui.org/io/input"
	"gioui.org/io/key"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
)

// noteLog records every on/off the keyboard emits, so tests can assert on the *release* path — the
// existing keyboard tests pass `func(uint8) {}` for onOff and never look at it, which is why two
// stuck-note bugs lived here with a green suite.
type noteLog struct {
	ons  []int
	offs []int
}

func (l *noteLog) held() map[int]int { // note → net on/off balance
	bal := map[int]int{}
	for _, n := range l.ons {
		bal[n]++
	}
	for _, n := range l.offs {
		bal[n]--
	}
	for n, v := range bal {
		if v == 0 {
			delete(bal, n)
		}
	}
	return bal
}

// kbFrames builds a keyboard wired to a log, plus a frame function and the router.
func kbFrames(t *testing.T) (*Keyboard, *noteLog, *input.Router, func()) {
	t.Helper()
	var r input.Router
	log := &noteLog{}
	kb := NewKeyboard(
		func(n, _ uint8) { log.ons = append(log.ons, int(n)) },
		func(n uint8) { log.offs = append(log.offs, int(n)) },
	)
	th := testTheme()
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(400, 300))
		kb.Layout(gtx, th)
		r.Frame(gtx.Ops)
	}
	return kb, log, &r, frame
}

// Shifting the octave while a key is held with the mouse must leave nothing sounding. The key stops
// being laid out, so Gio drops its tag from the router and its Release is never delivered —
// widget.Clickable.Pressed() then stays true forever. Polling every retained Clickable therefore
// re-sent NOTE_ON on every single frame, surviving both the mouse release and Panic.
func TestOctaveShiftWhileMouseHeldReleasesNote(t *testing.T) {
	kb, log, r, frame := kbFrames(t)

	frame() // establish key areas
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(10, 60), PointerID: 1})
	frame() // press polled → note on

	base := kb.base
	if got := log.held(); len(got) != 1 || got[base] != 1 {
		t.Fatalf("expected exactly note %d held after the press, got %v", base, got)
	}

	kb.OctaveUp() // note `base` is no longer drawn while the mouse is still down
	frame()
	frame()
	frame() // several frames: the bug re-fired the note on each one

	if got := log.held(); len(got) != 0 {
		t.Fatalf("notes still sounding after an octave shift with the mouse held: %v", got)
	}

	// And releasing the mouse afterwards must not resurrect it either.
	r.Queue(pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(10, 60), PointerID: 1})
	frame()
	frame()
	if got := log.held(); len(got) != 0 {
		t.Fatalf("notes reappeared after releasing the mouse: %v", got)
	}
}

// Releasing a PC-keyboard key only produces key.Release while the widget is laid out and focused. On
// focus loss (tab switch, Alt-Tab) no Release will ever arrive, so the FocusEvent has to release
// everything — it used to be explicitly discarded.
func TestFocusLossReleasesTypedNotes(t *testing.T) {
	kb, log, r, frame := kbFrames(t)
	frame()
	frame() // focus settles

	r.Queue(key.Event{Name: "Z", State: key.Press})
	frame()
	if got := log.held(); len(got) != 1 || got[kb.base] != 1 {
		t.Fatalf("expected note %d held after pressing Z, got %v", kb.base, got)
	}

	// Hand focus to another tag: the router sends the keyboard a FocusEvent{Focus:false}.
	var other struct{}
	r.Queue(key.FocusEvent{Focus: false})
	r.Source().Execute(key.FocusCmd{Tag: &other})
	frame()

	if got := log.held(); len(got) != 0 {
		t.Fatalf("typed note still sounding after focus loss: %v", got)
	}
}

// AllOff — what Panic and the tab switch call — must silence typed notes for good, and must leave
// nothing behind once the mouse is also up.
//
// Note the asymmetry, which is intended rather than a bug: a mouse-held key that is still being laid
// out legitimately re-sounds on the next frame, because polling Pressed() is how mouse keys work and
// the key really is still down. A typed note has no such polling — k.kbd is driven purely by
// Press/Release events — so it stays silent until the key is pressed again.
func TestAllOffReleasesEverything(t *testing.T) {
	kb, log, r, frame := kbFrames(t)
	frame()
	frame()

	r.Queue(key.Event{Name: "S", State: key.Press}) // base+1, a key the mouse press below won't touch
	r.Queue(pointer.Event{Kind: pointer.Press, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(10, 60), PointerID: 1})
	frame()
	if got := log.held(); len(got) != 2 {
		t.Fatalf("expected the typed and the clicked note held, got %v", got)
	}

	kb.AllOff()
	frame()
	frame()
	if got := log.held(); got[kb.base+1] != 0 {
		t.Fatalf("AllOff left the typed note sounding: %v", got)
	}

	// Now let the mouse go: after that nothing at all may remain.
	r.Queue(pointer.Event{Kind: pointer.Release, Source: pointer.Mouse, Buttons: pointer.ButtonPrimary, Position: f32.Pt(10, 60), PointerID: 1})
	frame()
	frame()
	if got := log.held(); len(got) != 0 {
		t.Fatalf("notes still sounding after AllOff and mouse release: %v", got)
	}
}

// The octave clamp has to bound the highest *reachable* note. Clamping the base at 108 still let the
// FL layout's "P" (base+28) emit 136, which the firmware turns into ~21 kHz: the key lights up and
// nothing is heard.
func TestOctaveClampKeepsNotesInMidiRange(t *testing.T) {
	kb, log, _, frame := kbFrames(t)
	frame()
	frame()

	for i := 0; i < 12; i++ { // walk to the top of the range
		kb.OctaveUp()
	}
	frame()

	for name, semi := range musicalTyping {
		if n := kb.base + semi; n > 127 {
			t.Fatalf("key %q reaches note %d (base=%d) — above the MIDI range", name, n, kb.base)
		}
	}
	// Highest drawn key too (2 octaves = base+23).
	if top := kb.base + 23; top > 127 {
		t.Fatalf("top drawn key is note %d (base=%d)", top, kb.base)
	}
	_ = log
}
