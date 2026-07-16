// Package ui is the Gio front-end: a fixed VST-style panel rack built from the LIST-driven parameter
// registry, an on-screen keyboard, and a live connection/metrics bar. It reads an immutable
// device.Snapshot each frame and sends edits back through the device (which throttles/prioritizes).
// Verified via Windows cross-build; input routing exercised by headless tests.
package ui

import (
	"fmt"
	"image"
	"image/color"
	"sync"

	"gioui.org/font/gofont"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"ucsynth/app/device"
	blk "ucsynth/app/layout"
	"ucsynth/app/midi"
	"ucsynth/app/patch"
	"ucsynth/app/seq"
	"ucsynth/app/serial"
)

// Controller is the top-level UI state.
type Controller struct {
	th         *material.Theme
	invalidate func()

	dev    *device.Device
	status string

	// connection bar
	ports      []serial.PortInfo
	portBtns   []widget.Clickable
	selPort    string
	refreshBtn widget.Clickable
	connectBtn widget.Clickable
	panicBtn   widget.Clickable
	toneOffBtn widget.Clickable

	// parameter rack
	kb       *Keyboard
	controls []*control
	builtN   int
	plist    widget.List

	// tabs: 0 synth, 1 sequencer, 2 patches
	tab     int
	tabBtns [3]widget.Clickable

	// STAT history graphs
	hist       statRing
	lastUptime uint32
	haveSample bool
	showGraphs bool
	metricsBtn widget.Clickable

	// patches
	store       patch.Store
	patchList   []patch.Entry
	patchBtns   []widget.Clickable
	patchName   widget.Editor
	patchPath   widget.Editor
	saveBtn     widget.Clickable
	reloadBtn   widget.Clickable
	deleteBtn   widget.Clickable
	importBtn   widget.Clickable
	exportBtn   widget.Clickable
	patchScroll widget.List
	patchSel    string
	patchMsg    string
	patchInit   bool

	// MIDI input
	midiNames   []string
	midiBtns    []widget.Clickable
	midiRefresh widget.Clickable
	midiIn      midi.Input
	midiOpen    int // open device index, -1 = none
	midiMsg     string

	// sequencer (piano-roll)
	player   *seq.Player
	playBtn  widget.Clickable
	stopBtn  widget.Clickable
	clearBtn widget.Clickable
	tempoDec widget.Clickable
	tempoInc widget.Clickable
	rollTag  struct{} // stable pointer = grid pointer-input tag
	rollGeom rollGeom

	// sink routes note on/off from the player/MIDI goroutines to the current device race-safely.
	sink noteSink
}

// noteSink guards the current device pointer so the sequencer clock and MIDI driver goroutines can
// emit notes while the UI goroutine connects/disconnects. The device's own note methods are already
// thread-safe; only the pointer swap needs protection.
type noteSink struct {
	mu  sync.Mutex
	dev *device.Device
}

func (n *noteSink) set(d *device.Device) { n.mu.Lock(); n.dev = d; n.mu.Unlock() }

func (n *noteSink) on(note, vel uint8) {
	n.mu.Lock()
	d := n.dev
	n.mu.Unlock()
	if d != nil {
		d.NoteOn(note, vel)
	}
}

func (n *noteSink) off(note uint8) {
	n.mu.Lock()
	d := n.dev
	n.mu.Unlock()
	if d != nil {
		d.NoteOff(note)
	}
}

// rackCols assigns parameter blocks to the three VST columns (mockup layout). Any block not listed
// (only "misc" today, the LIST-driven fallback) is appended to the last column.
var rackCols = [][]string{
	{"osc1", "osc2", "osc3", "mixer"},
	{"filter", "ampenv", "fltenv", "waveenv", "overdrive"},
	{"global", "lfo1", "lfo2", "modmatrix", "lofi", "debug", "misc"},
}
var colWeights = []float32{1, 1.15, 1}

// New builds the controller. invalidate (window.Invalidate) is called by the device when the model
// changes so the UI redraws.
func New(invalidate func()) *Controller {
	c := &Controller{invalidate: invalidate, builtN: -1}
	c.th = material.NewTheme()
	c.th.Shaper = text.NewShaper(text.WithCollection(gofont.Collection()))
	c.th.Palette = material.Palette{Bg: colBg, Fg: colTxt, ContrastBg: colAccent, ContrastFg: rgb(0xFFFFFF)}
	c.plist.Axis = layout.Vertical
	c.kb = NewKeyboard(
		func(n, v uint8) { c.sink.on(n, v) },
		func(n uint8) { c.sink.off(n) },
	)
	c.store = patch.Store{Root: patch.DefaultRoot()}
	c.patchName.SingleLine = true
	c.patchPath.SingleLine = true
	c.midiOpen = -1
	// Sequencer: 2 octaves (C3..C5), 16 steps, 120 BPM. Notes go to the device; redraw moves the playhead.
	c.player = seq.New(16, 48, 72, 120,
		func(note int, on bool) {
			if on {
				c.sink.on(uint8(note), 100)
			} else {
				c.sink.off(uint8(note))
			}
		},
		invalidate,
	)
	c.enumPorts()
	c.enumMidi()
	return c
}

// Shutdown stops playback, closes MIDI, and closes the connection (flushing held notes) at window close.
func (c *Controller) Shutdown() {
	if c.player != nil {
		c.player.Stop()
	}
	if c.midiIn != nil {
		c.midiIn.Close()
		c.midiIn = nil
	}
	c.disconnect()
}

func (c *Controller) enumPorts() {
	ports, err := serial.List()
	if err != nil {
		c.status = "порты: " + err.Error()
		return
	}
	c.ports = ports
	c.portBtns = make([]widget.Clickable, len(ports))
	if c.selPort == "" {
		for _, p := range ports {
			if p.IsSynth {
				c.selPort = p.Name
				break
			}
		}
	}
}

func (c *Controller) connect() {
	if c.selPort == "" {
		c.status = "выбери порт"
		return
	}
	conn, err := serial.Open(c.selPort)
	if err != nil {
		c.status = "ошибка: " + err.Error()
		return
	}
	c.dev = device.New(conn, c.invalidate)
	c.sink.set(c.dev)
	c.dev.Start()
	c.controls = nil
	c.builtN = -1
	c.status = ""
}

func (c *Controller) disconnect() {
	if c.player != nil {
		c.player.Stop()
	}
	if c.dev != nil {
		c.kb.AllOff()
		c.sink.set(nil)
		c.dev.Close()
		c.dev = nil
	}
	c.controls = nil
	c.builtN = -1
}

const (
	tabSynth = iota
	tabSeq
	tabPatches
)

// Layout renders one frame.
func (c *Controller) Layout(gtx C) D {
	paint.FillShape(gtx.Ops, colBg, clip.Rect{Max: gtx.Constraints.Max}.Op())

	// Read widget clicks BEFORE laying out — Clickable.Layout drains pending clicks, so Clicked()
	// must be queried first. Events come from last frame's input tree, so this is correct.
	c.handleButtons(gtx)

	var snap device.Snapshot
	if c.dev != nil {
		snap = c.dev.Snapshot()
		c.syncControls(snap)
		c.sampleStat(snap)
	}

	return layout.UniformInset(unit.Dp(12)).Layout(gtx, func(gtx C) D {
		flex := []layout.FlexChild{
			layout.Rigid(func(gtx C) D { return c.layoutTopBar(gtx, snap) }),
		}
		if c.showGraphs {
			flex = append(flex,
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(func(gtx C) D { return graphCard(gtx, c.th, &c.hist) }),
			)
		}
		flex = append(flex,
			layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
			layout.Rigid(c.layoutTabs),
			layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
			layout.Flexed(1, func(gtx C) D { return c.layoutTab(gtx, snap) }),
		)
		if c.tab == tabSynth { // keyboard only on the synth tab (sequencer needs the height)
			flex = append(flex,
				layout.Rigid(func(gtx C) D { return c.layoutToneBanner(gtx, snap) }),
				layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
				layout.Rigid(func(gtx C) D {
					return card(gtx, colLine, colPanel, func(gtx C) D { return c.kb.Layout(gtx, c.th) })
				}),
			)
		}
		flex = append(flex,
			layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
			layout.Rigid(func(gtx C) D { return c.layoutFooter(gtx) }),
		)
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx, flex...)
	})
}

func (c *Controller) layoutTab(gtx C, snap device.Snapshot) D {
	switch c.tab {
	case tabSeq:
		return c.layoutPianoRoll(gtx)
	case tabPatches:
		return c.layoutPatches(gtx)
	default:
		return c.layoutRack(gtx)
	}
}

func (c *Controller) layoutTabs(gtx C) D {
	names := []string{"Синт", "Секвенсор", "Патчи"}
	children := make([]layout.FlexChild, 0, len(names)*2)
	for i, n := range names {
		i, n := i, n
		children = append(children, layout.Rigid(func(gtx C) D {
			return layout.Inset{Right: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
				return c.tabBtns[i].Layout(gtx, func(gtx C) D {
					return btnPill(c.tab == i, false, false).draw(gtx, c.th, n)
				})
			})
		}))
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, children...)
}

// sampleStat appends a new STAT sample to the history ring when the device reports a fresh one.
func (c *Controller) sampleStat(snap device.Snapshot) {
	if snap.State != device.Synced {
		return
	}
	if c.haveSample && snap.Stat.UptimeMS == c.lastUptime {
		return
	}
	c.lastUptime = snap.Stat.UptimeMS
	c.haveSample = true
	c.hist.push(snap.Stat)
}

func (c *Controller) handleButtons(gtx C) {
	if c.refreshBtn.Clicked(gtx) {
		c.enumPorts()
	}
	for i := range c.portBtns {
		if c.portBtns[i].Clicked(gtx) && i < len(c.ports) {
			c.selPort = c.ports[i].Name
		}
	}
	if c.connectBtn.Clicked(gtx) {
		if c.dev == nil {
			c.connect()
		} else {
			c.disconnect()
		}
	}
	if c.panicBtn.Clicked(gtx) && c.dev != nil {
		c.dev.AllNotesOff()
		c.kb.AllOff()
	}
	if c.toneOffBtn.Clicked(gtx) && c.dev != nil {
		if id, ok := paramID(c.dev.Snapshot(), "test_tone"); ok {
			c.dev.SetParam(id, 0)
		}
	}
	for i := range c.tabBtns {
		if c.tabBtns[i].Clicked(gtx) {
			c.setTab(i)
		}
	}
	if c.metricsBtn.Clicked(gtx) {
		c.showGraphs = !c.showGraphs
	}
	c.handlePatches(gtx)
	c.handleMidi(gtx)
	c.handleSeq(gtx)
}

func (c *Controller) setTab(t int) {
	c.tab = t
	if t == tabPatches {
		c.refreshPatches()
	}
}

func (c *Controller) syncControls(snap device.Snapshot) {
	if len(snap.Params) != c.builtN {
		c.controls = c.controls[:0]
		for _, p := range snap.Params {
			c.controls = append(c.controls, newControl(p))
		}
		c.builtN = len(snap.Params)
		return
	}
	for i, p := range snap.Params {
		if i < len(c.controls) {
			c.controls[i].p = p
		}
	}
}

// --- top bar ---

func (c *Controller) layoutTopBar(gtx C, snap device.Snapshot) D {
	return card(gtx, colLine, colPanel, func(gtx C) D {
		children := []layout.FlexChild{
			layout.Rigid(func(gtx C) D {
				l := label(c.th, unit.Sp(13), "UC SYNTH", colMuted)
				l.Font.Weight = 600
				return layout.Inset{Right: unit.Dp(6)}.Layout(gtx, l.Layout)
			}),
			layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.refreshBtn, "Порты", false, false, false) }),
		}
		for i := range c.portBtns {
			i := i
			children = append(children,
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D {
					sel := i < len(c.ports) && c.ports[i].Name == c.selPort
					return c.portBtns[i].Layout(gtx, func(gtx C) D {
						return segPill(sel).draw(gtx, c.th, c.ports[i].Label)
					})
				}))
		}
		connLabel := "Подключить"
		if c.dev != nil {
			connLabel = "Отключить"
		}
		children = append(children,
			layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.connectBtn, connLabel, true, false, false) }),
			layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.panicBtn, "Panic", false, true, false) }),
			layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
			layout.Rigid(func(gtx C) D { return c.metricsText(gtx, snap) }),
			layout.Rigid(layout.Spacer{Width: unit.Dp(14)}.Layout),
			layout.Rigid(func(gtx C) D { return c.statusDot(gtx, snap) }),
		)
		return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, children...)
	})
}

func (c *Controller) obtn(gtx C, b *widget.Clickable, lbl string, accent, danger, active bool) D {
	return b.Layout(gtx, func(gtx C) D { return btnPill(active, accent, danger).draw(gtx, c.th, lbl) })
}

func (c *Controller) metricsText(gtx C, snap device.Snapshot) D {
	return c.metricsBtn.Layout(gtx, func(gtx C) D {
		if c.dev == nil {
			return label(c.th, unit.Sp(12.5), c.status, colMuted).Layout(gtx)
		}
		if snap.State != device.Synced {
			return label(c.th, unit.Sp(12.5), "…", colFaint).Layout(gtx)
		}
		s := snap.Stat
		txt := fmt.Sprintf("CPU %.1f%%   underruns %d   heap %.1fМ   up %dс   графики ▾",
			float64(s.CPUPermille)/10, s.Underruns, float64(s.Heap)/1048576, s.UptimeMS/1000)
		col := colMuted
		if c.showGraphs {
			col = colAccentB
		}
		return label(c.th, unit.Sp(12.5), txt, col).Layout(gtx)
	})
}

func (c *Controller) statusDot(gtx C, snap device.Snapshot) D {
	col, txt := colFaint, "не подключено"
	if c.dev != nil {
		switch snap.State {
		case device.Synced:
			col, txt = colOk, "Synced"
		case device.Connecting:
			col, txt = colWarn, "Connecting"
		default:
			col, txt = colErr, snap.State.String()
		}
		if snap.Err != nil {
			txt = snap.State.String() + ": " + snap.Err.Error()
		}
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return ledDot(gtx, col) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(7)}.Layout),
		layout.Rigid(label(c.th, unit.Sp(12.5), txt, colMuted).Layout),
	)
}

func ledDot(gtx C, col color.NRGBA) D {
	d := gtx.Dp(9)
	paint.FillShape(gtx.Ops, col, clip.Ellipse{Max: image.Pt(d, d)}.Op(gtx.Ops))
	return D{Size: image.Pt(d, d)}
}

// --- rack ---

func (c *Controller) layoutRack(gtx C) D {
	if c.dev == nil {
		return centerMsg(gtx, c.th, "Не подключено. Выбери порт и нажми «Подключить».")
	}
	if len(c.controls) == 0 {
		return centerMsg(gtx, c.th, "Загрузка реестра параметров…")
	}
	return material.List(c.th, &c.plist).Layout(gtx, 1, func(gtx C, _ int) D { return c.rack(gtx) })
}

func (c *Controller) rack(gtx C) D {
	byBlock := map[string][]*control{}
	for _, ct := range c.controls {
		byBlock[ct.fld.Block] = append(byBlock[ct.fld.Block], ct)
	}
	cols := make([]layout.FlexChild, 0, len(rackCols))
	for ci, keys := range rackCols {
		ci, keys := ci, keys
		cols = append(cols, layout.Flexed(colWeights[ci], func(gtx C) D {
			ins := layout.Inset{}
			if ci > 0 {
				ins.Left = unit.Dp(6)
			}
			if ci < len(rackCols)-1 {
				ins.Right = unit.Dp(6)
			}
			return ins.Layout(gtx, func(gtx C) D { return c.column(gtx, keys, byBlock) })
		}))
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Start}.Layout(gtx, cols...)
}

func (c *Controller) column(gtx C, keys []string, byBlock map[string][]*control) D {
	var items []layout.FlexChild
	first := true
	for _, k := range keys {
		cs := byBlock[k]
		if len(cs) == 0 {
			continue
		}
		k, cs := k, cs
		if !first {
			items = append(items, layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout))
		}
		first = false
		items = append(items, layout.Rigid(func(gtx C) D {
			body := func(gtx C) D { return c.panelBody(gtx, cs) }
			if k == "modmatrix" { // compact per-slot view instead of generic pill-rows
				body = func(gtx C) D { return c.matrixPanel(gtx, cs) }
			}
			return vstPanel(gtx, c.th, blk.BlockTitle(k), body)
		}))
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, items...)
}

func (c *Controller) panelBody(gtx C, cs []*control) D {
	var segs, knobs, sliders, steppers, toggles []*control
	for _, ct := range cs {
		switch ct.kind {
		case kindSeg:
			segs = append(segs, ct)
		case kindKnob:
			knobs = append(knobs, ct)
		case kindSlider:
			sliders = append(sliders, ct)
		case kindStepper:
			steppers = append(steppers, ct)
		case kindToggle:
			toggles = append(toggles, ct)
		}
	}
	var rows []layout.FlexChild
	first := true
	sec := func(w layout.Widget) {
		if !first {
			rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(11)}.Layout))
		}
		first = false
		rows = append(rows, layout.Rigid(w))
	}
	for _, ct := range segs {
		ct := ct
		sec(func(gtx C) D { return ct.segField(gtx, c.th, c.setParam) })
	}
	if len(knobs) > 0 {
		sec(func(gtx C) D { return c.cellRow(gtx, knobs, false) })
	}
	if len(sliders) > 0 {
		sec(func(gtx C) D { return c.cellRow(gtx, sliders, true) })
	}
	for _, ct := range steppers {
		ct := ct
		sec(func(gtx C) D { return ct.stepperCell(gtx, c.th, c.setParam) })
	}
	if len(toggles) > 0 {
		sec(func(gtx C) D { return c.toggleRow(gtx, toggles) })
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
}

func (c *Controller) cellRow(gtx C, cs []*control, slider bool) D {
	children := make([]layout.FlexChild, 0, len(cs)*2)
	for i, ct := range cs {
		ct := ct
		if i > 0 {
			children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout))
		}
		children = append(children, layout.Rigid(func(gtx C) D {
			if slider {
				return ct.sliderCell(gtx, c.th, c.setParam)
			}
			return ct.knobCell(gtx, c.th, c.setParam)
		}))
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Start}.Layout(gtx, children...)
}

func (c *Controller) toggleRow(gtx C, cs []*control) D {
	children := make([]layout.FlexChild, 0, len(cs)*2)
	for i, ct := range cs {
		ct := ct
		if i > 0 {
			children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout))
		}
		children = append(children, layout.Rigid(func(gtx C) D { return ct.toggleCell(gtx, c.th, c.setParam) }))
	}
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, children...)
}

// --- tone banner + footer ---

func (c *Controller) layoutToneBanner(gtx C, snap device.Snapshot) D {
	if c.dev == nil {
		return D{}
	}
	p, ok := snap.Param(mustID(snap, "test_tone"))
	if !ok || p.Cur <= 0.5 {
		return D{}
	}
	return layout.Inset{Top: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
		return card(gtx, colWarn, rgba(0xF0B03C, 0x1E), func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
				layout.Flexed(1, label(c.th, unit.Sp(13), "Тест-тон включён — ноты не слышны.", colWarn).Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.toneOffBtn, "Выключить тон", false, false, false) }),
			)
		})
	})
}

func (c *Controller) layoutFooter(gtx C) D {
	return card(gtx, colLine, colPanel, func(gtx C) D { return c.layoutMidiRow(gtx) })
}

func (c *Controller) setParam(id uint16, val float32) {
	if c.dev != nil {
		c.dev.SetParam(id, val)
	}
}

// --- helpers ---

// card draws a full-width rounded bordered bar/panel around content (measure-first).
func card(gtx C, border, fill color.NRGBA, content layout.Widget) D {
	macro := op.Record(gtx.Ops)
	dims := layout.UniformInset(unit.Dp(11)).Layout(gtx, content)
	call := macro.Stop()
	dims.Size.X = gtx.Constraints.Max.X
	r := image.Rectangle{Max: dims.Size}
	fillRRect(gtx.Ops, r, gtx.Dp(10), fill)
	strokeRRect(gtx.Ops, r, gtx.Dp(10), float32(gtx.Dp(1)), border)
	call.Add(gtx.Ops)
	return dims
}

func centerMsg(gtx C, th *material.Theme, s string) D {
	return layout.Center.Layout(gtx, label(th, unit.Sp(15), s, colMuted).Layout)
}

func rgb(v uint32) color.NRGBA {
	return color.NRGBA{R: byte(v >> 16), G: byte(v >> 8), B: byte(v), A: 255}
}

func paramID(snap device.Snapshot, name string) (uint16, bool) {
	for _, p := range snap.Params {
		if p.Name == name {
			return p.ID, true
		}
	}
	return 0, false
}

func mustID(snap device.Snapshot, name string) uint16 {
	id, _ := paramID(snap, name)
	return id
}
