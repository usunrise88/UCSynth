package ui

import (
	"image"
	"image/color"

	"gioui.org/io/event"
	"gioui.org/io/key"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/widget"
)

const kbVelocity = 100

// musicalTyping maps PC-keyboard key names → semitone offsets from the keyboard's base note,
// using the FL Studio / GarageBand "musical typing" layout (all in the key of C). Two rows an
// octave apart: the lower home rows (Z X C V B N M , . / = white, S D G H J L ; = black) and the
// upper rows one octave up (Q W E R T Y U I O P = white, 2 3 5 6 7 9 0 = black). The rows overlap
// by an octave — Q and "," both land on base+12 — exactly as FL Studio does it.
var musicalTyping = map[string]int{
	// lower row — white keys
	"Z": 0, "X": 2, "C": 4, "V": 5, "B": 7, "N": 9, "M": 11, ",": 12, ".": 14, "/": 16,
	// lower row — black keys
	"S": 1, "D": 3, "G": 6, "H": 8, "J": 10, "L": 13, ";": 15,
	// upper row — white keys (one octave above the lower row)
	"Q": 12, "W": 14, "E": 16, "R": 17, "T": 19, "Y": 21, "U": 23, "I": 24, "O": 26, "P": 28,
	// upper row — black keys
	"2": 13, "3": 15, "5": 18, "6": 20, "7": 22, "9": 25, "0": 27,
}

var whiteSemis = []int{0, 2, 4, 5, 7, 9, 11}
var blackAt = map[int]int{0: 1, 1: 3, 3: 6, 4: 8, 5: 10} // white-index → black semitone to its right

// Keyboard is an on-screen piano: mouse keys (clickable-poll) + PC-QWERTY (key events). It reports
// note on/off via callbacks; the caller wires them to the device. No text Editor exists in the UI,
// so the keyboard holds keyboard focus unconditionally.
type Keyboard struct {
	base    int // MIDI note of the leftmost C
	octaves int
	keys    map[int]*widget.Clickable
	mouse   map[int]bool // notes currently sounded by mouse
	kbd     map[int]bool // notes currently sounded by QWERTY
	onOn    func(note, vel uint8)
	onOff   func(note uint8)
}

func NewKeyboard(onOn func(note, vel uint8), onOff func(note uint8)) *Keyboard {
	return &Keyboard{
		base: 60, octaves: 2,
		keys:  map[int]*widget.Clickable{},
		mouse: map[int]bool{},
		kbd:   map[int]bool{},
		onOn:  onOn, onOff: onOff,
	}
}

func (k *Keyboard) keyOf(note int) *widget.Clickable {
	if c, ok := k.keys[note]; ok {
		return c
	}
	c := &widget.Clickable{}
	k.keys[note] = c
	return c
}

func (k *Keyboard) noteOn(note int, src map[int]bool) {
	if !src[note] {
		src[note] = true
		k.onOn(uint8(note), kbVelocity)
	}
}

func (k *Keyboard) noteOff(note int, src map[int]bool) {
	if src[note] {
		delete(src, note)
		k.onOff(uint8(note))
	}
}

// OctaveDown/Up shift the keyboard, releasing anything held (no stuck notes at the old pitch).
func (k *Keyboard) OctaveDown() { k.shift(-12) }
func (k *Keyboard) OctaveUp()   { k.shift(12) }

func (k *Keyboard) shift(d int) {
	k.AllOff()
	k.base += d
	if k.base < 12 {
		k.base = 12
	}
	if k.base > 108 {
		k.base = 108
	}
}

// AllOff releases every note the keyboard is currently sounding.
func (k *Keyboard) AllOff() {
	for n := range k.mouse {
		k.onOff(uint8(n))
	}
	for n := range k.kbd {
		k.onOff(uint8(n))
	}
	k.mouse = map[int]bool{}
	k.kbd = map[int]bool{}
}

// Layout draws the keyboard and processes mouse + QWERTY input.
func (k *Keyboard) Layout(gtx C) D {
	kbW := gtx.Constraints.Max.X
	whiteN := 7 * k.octaves
	if whiteN == 0 || kbW == 0 {
		return D{Size: image.Pt(kbW, gtx.Dp(150))}
	}
	ww := kbW / whiteN
	h := gtx.Dp(150)
	bw := ww * 2 / 3
	bh := h * 3 / 5

	// Bound the keyboard's key-event target to its own rectangle. event.Op with no clip
	// attaches to the root (unbounded) area; recorded last (the keyboard is the final child),
	// it would shadow pointer hit-testing for every widget above it — the connection-bar
	// buttons then never receive their clicks. The clip scopes handleKeys' event.Op here.
	area := clip.Rect{Max: image.Pt(kbW, h)}.Push(gtx.Ops)
	k.handleKeys(gtx)

	// dark backing (gives key separation via 1px gaps)
	paint.FillShape(gtx.Ops, color.NRGBA{R: 20, G: 22, B: 26, A: 255},
		clip.Rect{Max: image.Pt(kbW, h)}.Op())

	// white keys
	for o := 0; o < k.octaves; o++ {
		for wi, semi := range whiteSemis {
			note := k.base + o*12 + semi
			x := (o*7 + wi) * ww
			k.drawKey(gtx, note, x+1, 0, ww-2, h, k.whiteColor(note))
		}
	}
	// black keys (on top → win pointer hit-tests in overlap)
	for o := 0; o < k.octaves; o++ {
		for wi := 0; wi < 7; wi++ {
			semi, ok := blackAt[wi]
			if !ok {
				continue
			}
			note := k.base + o*12 + semi
			x := (o*7+wi)*ww + ww - bw/2
			k.drawKey(gtx, note, x, 0, bw, bh, k.blackColor(note))
		}
	}

	area.Pop()

	// poll mouse state → note on/off
	for note, clk := range k.keys {
		if clk.Pressed() {
			k.noteOn(note, k.mouse)
		} else {
			k.noteOff(note, k.mouse)
		}
	}

	return D{Size: image.Pt(kbW, h)}
}

func (k *Keyboard) drawKey(gtx C, note, x, y, w, h int, col color.NRGBA) {
	if w <= 0 {
		return
	}
	off := op.Offset(image.Pt(x, y)).Push(gtx.Ops)
	k.keyOf(note).Layout(gtx, func(gtx C) D {
		paint.FillShape(gtx.Ops, col, clip.Rect{Max: image.Pt(w, h)}.Op())
		return D{Size: image.Pt(w, h)}
	})
	off.Pop()
}

func (k *Keyboard) whiteColor(note int) color.NRGBA {
	if k.mouse[note] || k.kbd[note] {
		return color.NRGBA{R: 129, G: 199, B: 132, A: 255} // played (green)
	}
	return color.NRGBA{R: 240, G: 240, B: 242, A: 255}
}

func (k *Keyboard) blackColor(note int) color.NRGBA {
	if k.mouse[note] || k.kbd[note] {
		return color.NRGBA{R: 76, G: 175, B: 80, A: 255}
	}
	return color.NRGBA{R: 32, G: 34, B: 40, A: 255}
}

func (k *Keyboard) handleKeys(gtx C) {
	// Register k as an event target and make it FOCUSABLE. A tag is only focusable if it
	// registers a key.FocusFilter (io/input/router.go: FocusFilter → focusable=true); without it
	// the router strips focus every frame, so a key.Filter{Focus:k} — which only matches while
	// focused — never fires. That was the "keyboard plays nothing" bug. Re-grab focus whenever
	// something else took it (there are no text editors to protect), so typing always plays.
	event.Op(gtx.Ops, k)
	if !gtx.Focused(k) {
		gtx.Execute(key.FocusCmd{Tag: k})
	}

	filters := make([]event.Filter, 0, len(musicalTyping)+3)
	filters = append(filters, key.FocusFilter{Target: k})
	for name := range musicalTyping {
		filters = append(filters, key.Filter{Focus: k, Name: key.Name(name)})
	}
	// Up/Down arrows shift the octave (Z/X are notes now).
	filters = append(filters,
		key.Filter{Focus: k, Name: key.NameUpArrow},
		key.Filter{Focus: k, Name: key.NameDownArrow},
	)

	for {
		ev, ok := gtx.Event(filters...)
		if !ok {
			break
		}
		ke, ok := ev.(key.Event)
		if !ok {
			continue // e.g. key.FocusEvent from the FocusFilter
		}
		switch ke.Name {
		case key.NameUpArrow:
			if ke.State == key.Press {
				k.OctaveUp()
			}
			continue
		case key.NameDownArrow:
			if ke.State == key.Press {
				k.OctaveDown()
			}
			continue
		}
		semi, ok := musicalTyping[string(ke.Name)]
		if !ok {
			continue
		}
		note := k.base + semi
		if ke.State == key.Press {
			k.noteOn(note, k.kbd) // noteOn dedups → OS key-repeat is ignored
		} else {
			k.noteOff(note, k.kbd)
		}
	}
}
