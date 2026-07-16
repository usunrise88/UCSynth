// Package ui is the Gio front-end: connection bar, LIST-driven parameter blocks, on-screen
// keyboard, and live metrics. It reads an immutable device.Snapshot each frame and sends edits
// back through the device (which throttles/prioritizes). Imports Gio; verified via Windows cross-build.
package ui

import (
	"fmt"
	"image"
	"image/color"

	"gioui.org/font/gofont"
	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	blk "ucsynth/app/layout"
	"ucsynth/app/device"
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

	// parameter blocks
	kb       *Keyboard
	controls []*control
	builtN   int
	plist    widget.List
}

// New builds the controller. invalidate (window.Invalidate) is called by the device when the
// model changes so the UI redraws.
func New(invalidate func()) *Controller {
	c := &Controller{invalidate: invalidate, builtN: -1}
	c.th = material.NewTheme()
	c.th.Shaper = text.NewShaper(text.WithCollection(gofont.Collection()))
	c.th.Palette = material.Palette{
		Bg:         rgb(0x1E1F24),
		Fg:         rgb(0xE6E6E8),
		ContrastBg: rgb(0x4C9A5A),
		ContrastFg: rgb(0xFFFFFF),
	}
	c.plist.Axis = layout.Vertical
	c.kb = NewKeyboard(
		func(n, v uint8) {
			if c.dev != nil {
				c.dev.NoteOn(n, v)
			}
		},
		func(n uint8) {
			if c.dev != nil {
				c.dev.NoteOff(n)
			}
		},
	)
	c.enumPorts()
	return c
}

// Shutdown closes the connection (flushing held notes) at window close.
func (c *Controller) Shutdown() { c.disconnect() }

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
	c.dev.Start()
	c.controls = nil
	c.builtN = -1
	c.status = ""
}

func (c *Controller) disconnect() {
	if c.dev != nil {
		c.kb.AllOff()
		c.dev.Close()
		c.dev = nil
	}
	c.controls = nil
	c.builtN = -1
}

// Layout renders one frame.
func (c *Controller) Layout(gtx C) D {
	paint.FillShape(gtx.Ops, c.th.Palette.Bg, clip.Rect{Max: gtx.Constraints.Max}.Op())

	// Read widget clicks BEFORE laying out. material.Button.Layout → Clickable.layout drains
	// every pending click in a loop and discards it (widget/button.go), so a separate Clicked()
	// query must run first or it always sees an empty queue. The events themselves were routed
	// against last frame's input tree, so reading them here (before this frame's areas are
	// recorded) is correct.
	c.handleButtons(gtx)

	var snap device.Snapshot
	if c.dev != nil {
		snap = c.dev.Snapshot()
		c.syncControls(snap)
	}

	in := layout.UniformInset(unit.Dp(10))
	return in.Layout(gtx, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D { return c.layoutConnBar(gtx, snap) }),
			layout.Rigid(func(gtx C) D { return c.layoutMetrics(gtx, snap) }),
			layout.Rigid(func(gtx C) D { return c.layoutToneBanner(gtx, snap) }),
			layout.Flexed(1, func(gtx C) D { return c.layoutParams(gtx) }),
			layout.Rigid(func(gtx C) D { return c.kb.Layout(gtx) }),
		)
	})
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
	for i, p := range snap.Params { // refresh live values into existing widgets
		if i < len(c.controls) {
			c.controls[i].p = p
		}
	}
}

func (c *Controller) layoutConnBar(gtx C, snap device.Snapshot) D {
	set := func(gtx C, w layout.Widget) layout.FlexChild {
		return layout.Rigid(func(gtx C) D { return layout.Inset{Right: unit.Dp(6)}.Layout(gtx, w) })
	}
	children := []layout.FlexChild{
		set(gtx, material.Button(c.th, &c.refreshBtn, "Порты").Layout),
	}
	for i := range c.portBtns {
		i := i
		children = append(children, set(gtx, func(gtx C) D {
			b := material.Button(c.th, &c.portBtns[i], c.ports[i].Label)
			b.TextSize = unit.Sp(13)
			if c.ports[i].Name == c.selPort {
				b.Background = c.th.Palette.ContrastBg
			} else {
				b.Background = rgb(0x3A3C44)
			}
			return b.Layout(gtx)
		}))
	}
	label := "Подключить"
	if c.dev != nil {
		label = "Отключить"
	}
	children = append(children,
		layout.Flexed(1, func(gtx C) D { return D{Size: image.Pt(gtx.Constraints.Min.X, 0)} }),
		set(gtx, material.Button(c.th, &c.panicBtn, "Panic").Layout),
		set(gtx, material.Button(c.th, &c.connectBtn, label).Layout),
	)
	row := layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, children...)

	status := c.status
	if c.dev != nil {
		status = "● " + snap.State.String()
		if snap.Err != nil {
			status += ": " + snap.Err.Error()
		}
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return row }),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Top: unit.Dp(4), Bottom: unit.Dp(4)}.Layout(gtx,
				material.Caption(c.th, status).Layout)
		}),
	)
}

func (c *Controller) layoutMetrics(gtx C, snap device.Snapshot) D {
	if c.dev == nil || snap.State != device.Synced {
		return D{}
	}
	s := snap.Stat
	txt := fmt.Sprintf("CPU %.1f%%    underruns %d    heap %d КБ    uptime %d с",
		float64(s.CPUPermille)/10, s.Underruns, s.Heap/1024, s.UptimeMS/1000)
	return card(gtx, c.th, func(gtx C) D { return material.Body2(c.th, txt).Layout(gtx) })
}

func (c *Controller) layoutToneBanner(gtx C, snap device.Snapshot) D {
	if c.dev == nil {
		return D{}
	}
	p, ok := snap.Param(mustID(snap, "test_tone"))
	if !ok || p.Cur <= 0.5 {
		return D{}
	}
	return card(gtx, c.th, func(gtx C) D {
		return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
			layout.Flexed(1, material.Body2(c.th, "Тест-тон включён — ноты не слышны.").Layout),
			layout.Rigid(material.Button(c.th, &c.toneOffBtn, "Выключить тон").Layout),
		)
	})
}

func (c *Controller) layoutParams(gtx C) D {
	if c.dev == nil {
		return center(gtx, material.Body1(c.th, "Не подключено. Выбери порт и нажми «Подключить».").Layout)
	}
	items := c.items()
	if len(items) == 0 {
		return center(gtx, material.Body1(c.th, "Загрузка реестра параметров…").Layout)
	}
	return material.List(c.th, &c.plist).Layout(gtx, len(items), func(gtx C, i int) D {
		it := items[i]
		if it.ctrl == nil { // block header
			return layout.Inset{Top: unit.Dp(12), Bottom: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
				h := material.Subtitle1(c.th, it.title)
				h.Color = c.th.Palette.ContrastBg
				return h.Layout(gtx)
			})
		}
		return layout.Inset{Top: unit.Dp(3), Bottom: unit.Dp(3)}.Layout(gtx, func(gtx C) D {
			return it.ctrl.row(gtx, c.th, c.setParam)
		})
	})
}

func (c *Controller) setParam(id uint16, val float32) {
	if c.dev != nil {
		c.dev.SetParam(id, val)
	}
}

type listItem struct {
	title string
	ctrl  *control
}

func (c *Controller) items() []listItem {
	byBlock := map[string][]*control{}
	for _, ct := range c.controls {
		byBlock[ct.fld.Block] = append(byBlock[ct.fld.Block], ct)
	}
	var out []listItem
	for _, b := range blk.Blocks {
		cs := byBlock[b.Key]
		if len(cs) == 0 {
			continue
		}
		out = append(out, listItem{title: b.Title})
		for _, ct := range cs {
			out = append(out, listItem{ctrl: ct})
		}
	}
	return out
}

// --- helpers ---

func card(gtx C, th *material.Theme, w layout.Widget) D {
	return layout.Inset{Top: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
		return widget.Border{Color: rgb(0x3A3C44), Width: unit.Dp(1), CornerRadius: unit.Dp(4)}.Layout(gtx,
			func(gtx C) D {
				return layout.UniformInset(unit.Dp(8)).Layout(gtx, w)
			})
	})
}

func center(gtx C, w layout.Widget) D {
	return layout.Center.Layout(gtx, w)
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
