package ui

import (
	"image"
	"image/color"
	"strconv"

	"gioui.org/io/event"
	"gioui.org/io/key"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"
)

const kbVelocity = 100

// musicalTyping maps PC-keyboard key names → semitone offsets from the keyboard's base note, using
// the FL Studio / GarageBand "musical typing" layout (all in the key of C). Two rows an octave
// apart: lower home rows (Z X C V B N M , . / = white, S D G H J L ; = black) and the upper rows
// an octave up (Q W E R T Y U I O P = white, 2 3 5 6 7 9 0 = black). The rows overlap by an octave —
// Q and "," both land on base+12 — exactly as FL Studio does it.
var musicalTyping = map[string]int{
	"Z": 0, "X": 2, "C": 4, "V": 5, "B": 7, "N": 9, "M": 11, ",": 12, ".": 14, "/": 16,
	"S": 1, "D": 3, "G": 6, "H": 8, "J": 10, "L": 13, ";": 15,
	"Q": 12, "W": 14, "E": 16, "R": 17, "T": 19, "Y": 21, "U": 23, "I": 24, "O": 26, "P": 28,
	"2": 13, "3": 15, "5": 18, "6": 20, "7": 22, "9": 25, "0": 27,
}

var whiteSemis = []int{0, 2, 4, 5, 7, 9, 11}
var blackAt = map[int]int{0: 1, 1: 3, 3: 6, 4: 8, 5: 10} // white-index → black semitone to its right

var noteNames = []string{"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"}

func noteName(m int) string { return noteNames[((m%12)+12)%12] + strconv.Itoa(m/12-1) }

// Keyboard is an on-screen piano: mouse keys (clickable-poll) + PC musical-typing (key events). It
// reports note on/off via callbacks; the caller wires them to the device.
type Keyboard struct {
	base    int // MIDI note of the leftmost C
	octaves int
	keys    map[int]*widget.Clickable
	mouse   map[int]bool
	kbd     map[int]bool
	octDown widget.Clickable
	octUp   widget.Clickable
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

// Layout draws the octave strip, the piano, and the layout hint.
func (k *Keyboard) Layout(gtx C, th *material.Theme) D {
	if k.octDown.Clicked(gtx) {
		k.OctaveDown()
	}
	if k.octUp.Clicked(gtx) {
		k.OctaveUp()
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return k.topStrip(gtx, th) }),
		layout.Rigid(layout.Spacer{Height: unit.Dp(9)}.Layout),
		layout.Rigid(k.piano),
		layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D {
			return label(th, unit.Sp(11.5),
				"FL-раскладка: Z X C V B N M — белые, S D · G H J — чёрные · Q…P на октаву выше · октава ↑ ↓",
				colFaint).Layout(gtx)
		}),
	)
}

func (k *Keyboard) topStrip(gtx C, th *material.Theme) D {
	octBtn := func(b *widget.Clickable, lbl string) layout.Widget {
		return func(gtx C) D {
			return b.Layout(gtx, func(gtx C) D { return btnPill(false, false, false).draw(gtx, th, lbl) })
		}
	}
	chip := pill{border: colAccent, text: colAccentB, fill: colAccentDim, size: unit.Sp(13), padX: 12, padY: 5, radius: 7}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(octBtn(&k.octDown, "◂ Окт")),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return chip.draw(gtx, th, noteName(k.base)) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(octBtn(&k.octUp, "Окт ▸")),
		layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
		layout.Rigid(label(th, unit.Sp(12), "Мышью или с клавиатуры ПК", colMuted).Layout),
	)
}

// piano draws the keys and processes mouse + typing input. The clip bounds the keyboard's input
// area to its own rectangle (else its unbounded event.Op would shadow every widget above it).
func (k *Keyboard) piano(gtx C) D {
	kbW := gtx.Constraints.Max.X
	whiteN := 7 * k.octaves
	h := gtx.Dp(120)
	if whiteN == 0 || kbW == 0 {
		return D{Size: image.Pt(kbW, h)}
	}
	ww := kbW / whiteN
	bw := ww * 2 / 3
	bh := h * 3 / 5
	rad := gtx.Dp(4)

	area := clip.Rect{Max: image.Pt(kbW, h)}.Push(gtx.Ops)
	k.handleKeys(gtx)

	fillRRect(gtx.Ops, image.Rect(0, 0, kbW, h), gtx.Dp(6), colKeyFace)
	for o := 0; o < k.octaves; o++ {
		for wi, semi := range whiteSemis {
			note := k.base + o*12 + semi
			x := (o*7 + wi) * ww
			k.drawKey(gtx, note, x+1, 0, ww-2, h, rad, k.whiteColor(note))
		}
	}
	for o := 0; o < k.octaves; o++ {
		for wi := 0; wi < 7; wi++ {
			semi, ok := blackAt[wi]
			if !ok {
				continue
			}
			note := k.base + o*12 + semi
			x := (o*7+wi)*ww + ww - bw/2
			k.drawKey(gtx, note, x, 0, bw, bh, rad, k.blackColor(note))
		}
	}
	area.Pop()

	for note, clk := range k.keys {
		if clk.Pressed() {
			k.noteOn(note, k.mouse)
		} else {
			k.noteOff(note, k.mouse)
		}
	}
	return D{Size: image.Pt(kbW, h)}
}

func (k *Keyboard) drawKey(gtx C, note, x, y, w, h, rad int, col color.NRGBA) {
	if w <= 0 {
		return
	}
	off := op.Offset(image.Pt(x, y)).Push(gtx.Ops)
	k.keyOf(note).Layout(gtx, func(gtx C) D {
		rr := clip.RRect{Rect: image.Rect(0, 0, w, h), SE: rad, SW: rad}
		paint.FillShape(gtx.Ops, col, rr.Op(gtx.Ops))
		return D{Size: image.Pt(w, h)}
	})
	off.Pop()
}

func (k *Keyboard) whiteColor(note int) color.NRGBA {
	if k.mouse[note] || k.kbd[note] {
		return colWhiteKeyOn
	}
	return colWhiteKey
}

func (k *Keyboard) blackColor(note int) color.NRGBA {
	if k.mouse[note] || k.kbd[note] {
		return colBlackKeyOn
	}
	return colBlackKey
}

func (k *Keyboard) handleKeys(gtx C) {
	// Register k as an event target and make it FOCUSABLE. A tag is only focusable if it registers
	// a key.FocusFilter (io/input/router.go: FocusFilter → focusable=true); without it the router
	// strips focus every frame, so a key.Filter{Focus:k} — which only matches while focused — never
	// fires. That was the "keyboard plays nothing" bug. Re-grab focus whenever something else took
	// it (there are no text editors to protect), so typing always plays.
	event.Op(gtx.Ops, k)
	if !gtx.Focused(k) {
		gtx.Execute(key.FocusCmd{Tag: k})
	}

	filters := make([]event.Filter, 0, len(musicalTyping)+3)
	filters = append(filters, key.FocusFilter{Target: k})
	for name := range musicalTyping {
		filters = append(filters, key.Filter{Focus: k, Name: key.Name(name)})
	}
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
