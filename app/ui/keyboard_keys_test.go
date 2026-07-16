package ui

import (
	"image"
	"testing"

	"gioui.org/io/input"
	"gioui.org/io/key"
	"gioui.org/layout"
	"gioui.org/op"
)

// playKey drives frames until focus settles, injects a press+release of the named key, and returns
// the note reported on press (or -1). Regression for the focus bug: without key.FocusFilter the
// keyboard never gains focus and no key ever plays.
func kbHarness(t *testing.T) (*Keyboard, func(name string, state key.State), func() int) {
	t.Helper()
	var r input.Router
	got := -1
	kb := NewKeyboard(func(n, _ uint8) { got = int(n) }, func(uint8) {})
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source(), Constraints: layout.Exact(image.Pt(400, 300))}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(400, 300))
		kb.Layout(gtx)
		r.Frame(gtx.Ops)
	}
	frame()
	frame() // focus settles
	if !gtx.Focused(kb) {
		t.Fatal("keyboard never gained focus — key.FocusFilter missing?")
	}
	send := func(name string, state key.State) {
		r.Queue(key.Event{Name: key.Name(name), State: state})
		frame()
	}
	return kb, send, func() int { return got }
}

func TestKeyboardFocusAndPlays(t *testing.T) {
	_, send, got := kbHarness(t)
	send("Z", key.Press) // Z = base note (C) in the FL musical-typing layout
	if n := got(); n != 60 {
		t.Fatalf("'Z' should play base note 60, got %d", n)
	}
}

func TestMusicalTypingLayout(t *testing.T) {
	// base = 60 (C4). Spot-check white, black, upper-row, and punctuation keys.
	cases := map[string]int{
		"Z": 60, "S": 61, "X": 62, "C": 64, "V": 65, // lower white + a black
		"Q": 72, "3": 75, "E": 76, "U": 83, // upper row (one octave up) + a black
		",": 72, "/": 76, // punctuation extends the lower row
	}
	for name, want := range cases {
		_, send, got := kbHarness(t)
		send(name, key.Press)
		if n := got(); n != want {
			t.Errorf("key %q: got note %d, want %d", name, n, want)
		}
	}
}

func TestKeyboardOctaveArrows(t *testing.T) {
	kb, send, got := kbHarness(t)
	send("↑", key.Press) // NameUpArrow → octave up
	if kb.base != 72 {
		t.Fatalf("Up arrow should raise base to 72, got %d", kb.base)
	}
	send("Z", key.Press)
	if n := got(); n != 72 {
		t.Fatalf("after octave up, 'Z' should play 72, got %d", n)
	}
}
