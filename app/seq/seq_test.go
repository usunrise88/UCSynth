package seq

import (
	"reflect"
	"testing"
)

type ev struct {
	note int
	on   bool
}

func TestAdvanceEmitsAndReleases(t *testing.T) {
	var got []ev
	p := New(4, 60, 72, 120, func(n int, on bool) { got = append(got, ev{n, on}) }, nil)
	// step 0: C4(60); step 1: (none); step 2: E4(64)+G4(67)
	p.Toggle(0, 60)
	p.Toggle(2, 64)
	p.Toggle(2, 67)

	p.playing = true // simulate Start without launching the clock goroutine

	p.advance() // → step 0: on 60
	p.advance() // → step 1: off 60, nothing on
	p.advance() // → step 2: on 64,67
	p.advance() // → step 3: off 64,67

	want := []ev{
		{60, true},
		{60, false},
		{64, true}, {67, true},
		{64, false}, {67, false},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("emit sequence mismatch:\n got %+v\nwant %+v", got, want)
	}
	if p.Cur() != 3 {
		t.Fatalf("cur should be 3, got %d", p.Cur())
	}
}

func TestAdvanceWraps(t *testing.T) {
	p := New(3, 60, 60, 120, func(int, bool) {}, nil)
	p.playing = true
	p.advance() // 0
	p.advance() // 1
	p.advance() // 2
	p.advance() // wraps → 0
	if p.Cur() != 0 {
		t.Fatalf("expected wrap to 0, got %d", p.Cur())
	}
}

func TestToggleAndClear(t *testing.T) {
	p := New(4, 60, 72, 120, func(int, bool) {}, nil)
	p.Toggle(1, 65)
	if !p.On(1, 65) {
		t.Fatal("cell should be on after toggle")
	}
	p.Toggle(1, 65)
	if p.On(1, 65) {
		t.Fatal("cell should be off after second toggle")
	}
	p.Toggle(2, 67)
	p.Clear()
	if p.On(2, 67) {
		t.Fatal("cell should be off after clear")
	}
}
