package ui

import "image/color"

// Palette — the VST "instrument" look: deep blue-graphite grounds, a single blue accent, outlined
// controls. Deliberately dark-only (an instrument panel is one visual world). Mirrors the approved
// HTML mockup token-for-token.
var (
	colBg       = rgb(0x12161C) // window background
	colPanel    = rgb(0x1E2530) // panel fill (bottom of gradient)
	colPanel2   = rgb(0x252D39) // panel fill (top of gradient) / raised
	colLine     = rgb(0x2C3643) // hairline / panel border
	colLine2    = rgb(0x3A4655) // inactive control outline
	colTxt      = rgb(0xE6EAF0) // primary text
	colMuted    = rgb(0x8790A0) // labels
	colFaint    = rgb(0x5B6675) // captions / tick labels
	colAccent   = rgb(0x3B8EF5) // blue accent (outline/arc)
	colAccentB  = rgb(0x63A6FF) // brighter accent (pointer/active text)
	colAccentDim = rgba(0x3B8EF5, 0x24) // ~14% — active fill tint
	colTrack    = rgb(0x39434F) // knob/slider track
	colKnobBody = rgb(0x1B212A) // knob face
	colOk       = rgb(0x45D483) // connected
	colWarn     = rgb(0xF0B03C) // warning / Panic
	colErr      = rgb(0xF2585F) // error

	colWhiteKey   = rgb(0xEDF0F4)
	colWhiteKeyLo = rgb(0xCBD1D9)
	colWhiteKeyOn = rgb(0x8FB6F4)
	colBlackKey   = rgb(0x1A212B)
	colBlackKeyOn = rgb(0x3F74C8)
	colKeyFace    = rgb(0x11161D) // keyboard backing
)

// rgba builds a color from 0xRRGGBB plus an explicit alpha.
func rgba(v uint32, a uint8) color.NRGBA {
	return color.NRGBA{R: byte(v >> 16), G: byte(v >> 8), B: byte(v), A: a}
}
