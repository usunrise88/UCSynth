package ui

import (
	"fmt"
	"image"
	"strconv"
	"strings"

	"gioui.org/io/event"
	"gioui.org/io/pointer"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"ucsynth/app/device"
	"ucsynth/app/proto"
)

// The sequencer tab is a pure editor over a device-side engine (stage 7): the синт owns the clock and
// the live 16-step pattern. `c.pattern` is a local working copy — seeded from the device (watched via
// Snapshot.SeqRev) and uploaded one step at a time on every edit (SeqSetStep). Transport and tempo are
// ordinary registry params (seq_playing / seq_on / seq_bpm), so nothing here touches the DSP directly.
//
// No live playhead: the current step lives on Core 0 and there is no fast channel to surface it — STAT
// polls at 2 Hz while a 16th note at 120 BPM is 125 ms, so any cursor drawn from serial would be a lie.
// The device plays standalone; the editor shows the pattern, not the position (см. tech-debt D-023).

// rollGeom is the grid geometry from the last frame, used to decode a click into a (step, pitch).
type rollGeom struct {
	x0, cellW, cellH int
	steps, hi        int
}

func isBlackKey(pitch int) bool {
	switch ((pitch % 12) + 12) % 12 {
	case 1, 3, 6, 8, 10:
		return true
	}
	return false
}

// handleSeq runs once per frame (from handleButtons): it keeps the editor copy in sync with the device,
// lists patterns once per connection, and drains the transport / step-select / p-lock / browser clicks.
func (c *Controller) handleSeq(gtx C) {
	if c.dev == nil {
		c.seqSyncedFor = nil
		c.seqSelOK = false
		return
	}
	snap := c.dev.Snapshot()

	// GET the live pattern + LIST the stored ones once whenever we (re)connect.
	if c.dev != c.seqSyncedFor {
		c.dev.SeqGet()
		c.dev.SeqList()
		c.seqSyncedFor = c.dev
		c.seqSeenRev = snap.SeqRev // adopt current rev; copy again only when it advances past this
	}
	// Reseed the working copy exactly when a fresh dump lands (connect or load), never mid-edit: the
	// firmware never pushes step edits, so SeqRev is stable between our own SeqGet calls.
	if snap.SeqRev != c.seqSeenRev {
		c.pattern = snap.SeqPattern
		c.seqSeenRev = snap.SeqRev
	}

	if c.seqStepSel < 0 || c.seqStepSel >= proto.SeqSteps {
		c.seqStepSel = 0
	}

	c.handleTransport(gtx, snap)
	c.handleStepSelect(gtx)
	c.handlePlock(gtx, snap)
	c.handleSeqBrowser(gtx, snap)
}

func (c *Controller) handleTransport(gtx C, snap device.Snapshot) {
	if c.playBtn.Clicked(gtx) {
		playing, _ := paramVal(snap, "seq_playing")
		if playing > 0.5 {
			c.setParamByName(snap, "seq_playing", 0)
		} else {
			c.setParamByName(snap, "seq_on", 1) // pressing Play here means "play this pattern"
			c.setParamByName(snap, "seq_playing", 1)
		}
	}
	if c.clearBtn.Clicked(gtx) {
		for i := range c.pattern {
			c.pattern[i] = proto.SeqStep{}
			c.dev.SeqSetStep(uint8(i), c.pattern[i])
		}
	}
	if c.tempoDec.Clicked(gtx) {
		c.nudgeBPM(snap, -5)
	}
	if c.tempoInc.Clicked(gtx) {
		c.nudgeBPM(snap, +5)
	}
}

func (c *Controller) nudgeBPM(snap device.Snapshot, d float32) {
	cur, ok := paramVal(snap, "seq_bpm")
	if !ok {
		return
	}
	v := cur + d
	if v < 20 {
		v = 20
	}
	if v > 300 {
		v = 300
	}
	c.setParamByName(snap, "seq_bpm", v)
}

func (c *Controller) handleStepSelect(gtx C) {
	for i := range c.stepBtns {
		if c.stepBtns[i].Clicked(gtx) {
			c.seqStepSel = i
		}
	}
}

// handlePlock drives the compact p-lock editor for the selected step: param picker, value entry,
// add/replace, per-lock remove, and clear-all. Every change re-uploads the whole step.
func (c *Controller) handlePlock(gtx C, snap device.Snapshot) {
	n := len(snap.Params)
	if n > 0 {
		if c.plockParam >= n {
			c.plockParam = n - 1
		}
		if c.plockPrev.Clicked(gtx) {
			c.plockParam = (c.plockParam - 1 + n) % n
		}
		if c.plockNext.Clicked(gtx) {
			c.plockParam = (c.plockParam + 1) % n
		}
	}

	st := c.pattern[c.seqStepSel]
	if len(c.plockRmBtns) != len(st.Plocks) {
		c.plockRmBtns = make([]widget.Clickable, len(st.Plocks))
	}
	for i := range c.plockRmBtns {
		if i < len(st.Plocks) && c.plockRmBtns[i].Clicked(gtx) {
			pl := append([]proto.SeqPlock{}, st.Plocks[:i]...)
			st.Plocks = append(pl, st.Plocks[i+1:]...)
			c.uploadStep(c.seqStepSel, st)
			return
		}
	}
	if c.plockClear.Clicked(gtx) {
		st.Plocks = nil
		c.uploadStep(c.seqStepSel, st)
		return
	}
	if c.plockAdd.Clicked(gtx) && n > 0 {
		p := snap.Params[c.plockParam]
		val := p.Cur
		if v, err := strconv.ParseFloat(strings.TrimSpace(c.plockVal.Text()), 32); err == nil {
			val = float32(v)
		}
		if val < p.Min {
			val = p.Min
		}
		if val > p.Max {
			val = p.Max
		}
		st.Plocks = setPlock(st.Plocks, p.ID, val)
		c.uploadStep(c.seqStepSel, st)
	}
}

func (c *Controller) handleSeqBrowser(gtx C, snap device.Snapshot) {
	if len(c.seqEntryBtns) != len(snap.Patterns) {
		c.seqEntryBtns = make([]widget.Clickable, len(snap.Patterns))
	}
	if c.seqListBtn.Clicked(gtx) {
		c.dev.SeqList()
	}
	if c.seqSaveBtn.Clicked(gtx) {
		name := strings.TrimSpace(c.seqName.Text())
		if name == "" {
			c.seqMsg = "введи имя паттерна (можно Папка/Имя)"
		} else {
			c.dev.SeqSave(proto.PresetSlotNew, name)
			c.seqMsg = "сохранён в синт: " + name
		}
	}
	if c.seqLoadBtn.Clicked(gtx) {
		if !c.seqSelOK {
			c.seqMsg = "выбери паттерн в списке"
		} else {
			c.dev.SeqLoad(c.seqSel) // device applies it + re-GETs → SeqRev bumps → reseed
			c.seqMsg = "загружен из синта"
		}
	}
	if c.seqRenameBtn.Clicked(gtx) {
		name := strings.TrimSpace(c.seqName.Text())
		switch {
		case !c.seqSelOK:
			c.seqMsg = "выбери паттерн и задай новый путь"
		case name == "":
			c.seqMsg = "задай новый путь (Папка/Имя)"
		default:
			c.dev.SeqRename(c.seqSel, name)
			c.seqMsg = "переименован: " + name
		}
	}
	if c.seqDeleteBtn.Clicked(gtx) {
		if !c.seqSelOK {
			c.seqMsg = "выбери паттерн в списке"
		} else {
			c.dev.SeqDelete(c.seqSel)
			c.seqSelOK = false
			c.seqMsg = "удалён из синта"
		}
	}
	for i := range c.seqEntryBtns {
		if i < len(snap.Patterns) && c.seqEntryBtns[i].Clicked(gtx) {
			c.seqSel = snap.Patterns[i].Slot
			c.seqSelOK = true
			c.seqName.SetText(snap.Patterns[i].Path)
			c.seqMsg = "выбран: " + snap.Patterns[i].Path
		}
	}
}

// uploadStep stores the edited step in the working copy and pushes it to the device.
func (c *Controller) uploadStep(step int, st proto.SeqStep) {
	c.pattern[step] = st
	if c.dev != nil {
		c.dev.SeqSetStep(uint8(step), st)
	}
}

// toggleNote adds/removes a pitch from a step's chord and uploads the step. A freshly-activated step
// gets a usable default velocity and always-on trig probability so it actually sounds.
func (c *Controller) toggleNote(step, pitch int) {
	if c.dev == nil {
		return
	}
	st := c.pattern[step]
	notes := append([]uint8{}, st.Notes...)
	idx := -1
	for i, nn := range notes {
		if int(nn) == pitch {
			idx = i
			break
		}
	}
	if idx >= 0 {
		notes = append(notes[:idx], notes[idx+1:]...)
	} else if len(notes) < proto.SeqMaxNotes {
		notes = append(notes, uint8(pitch))
	} else {
		return // chord full
	}
	st.Notes = notes
	st.Active = len(notes) > 0
	if st.Active {
		if st.Velocity == 0 {
			st.Velocity = 100
		}
		if st.TrigProb == 0 {
			st.TrigProb = 1
		}
	}
	c.uploadStep(step, st)
}

func (c *Controller) noteOn(step, pitch int) bool {
	for _, n := range c.pattern[step].Notes {
		if int(n) == pitch {
			return true
		}
	}
	return false
}

// --- layout ---

func (c *Controller) layoutPianoRoll(gtx C, snap device.Snapshot) D {
	if c.dev == nil {
		return card(gtx, colLine, colPanel, func(gtx C) D {
			return centerMsg(gtx, c.th, "Не подключено. Секвенсор живёт в синте — подключись, чтобы редактировать.")
		})
	}
	return card(gtx, colLine, colPanel, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D { return c.rollTransport(gtx, snap) }),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(c.stepSelectRow),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Flexed(1, c.rollGrid),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D { return c.plockPanel(gtx, snap) }),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(c.seqBrowserBar),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D { return fixedH(gtx, gtx.Dp(96), func(gtx C) D { return c.layoutSeqTree(gtx, snap) }) }),
		)
	})
}

func (c *Controller) rollTransport(gtx C, snap device.Snapshot) D {
	playing, _ := paramVal(snap, "seq_playing")
	on := playing > 0.5
	playLbl := "▶ Играть"
	if on {
		playLbl = "■ Стоп"
	}
	bpm, _ := paramVal(snap, "seq_bpm")
	return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.playBtn, playLbl, true, false, on) }),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.clearBtn, "Очистить", false, false, false) }),
		layout.Flexed(1, func(gtx C) D { return D{Size: gtx.Constraints.Min} }),
		layout.Rigid(label(c.th, unit.Sp(12), "Темп", colMuted).Layout),
		layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.tempoDec, "−", false, false, false) }),
		layout.Rigid(func(gtx C) D {
			return layout.Inset{Left: unit.Dp(10), Right: unit.Dp(10)}.Layout(gtx,
				label(c.th, unit.Sp(14), fmtInt(bpm)+" BPM", colTxt).Layout)
		}),
		layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.tempoInc, "+", false, false, false) }),
	)
}

// stepSelectRow is a row of 16 buttons; the selected one is where p-lock edits land.
func (c *Controller) stepSelectRow(gtx C) D {
	children := make([]layout.FlexChild, 0, proto.SeqSteps)
	for i := range c.stepBtns {
		i := i
		children = append(children, layout.Flexed(1, func(gtx C) D {
			return layout.Inset{Right: unit.Dp(3)}.Layout(gtx, func(gtx C) D {
				st := segPill(i == c.seqStepSel)
				if c.pattern[i].Active { // mark steps that carry notes
					st.text = colAccentB
				}
				return c.stepBtns[i].Layout(gtx, func(gtx C) D { return st.draw(gtx, c.th, strconv.Itoa(i+1)) })
			})
		}))
	}
	return layout.Flex{Axis: layout.Horizontal}.Layout(gtx, children...)
}

func (c *Controller) rollGrid(gtx C) D {
	// Decode last frame's clicks against the stored geometry (position is local to this grid).
	for {
		ev, ok := gtx.Event(pointer.Filter{Target: &c.rollTag, Kinds: pointer.Press})
		if !ok {
			break
		}
		e, ok := ev.(pointer.Event)
		if !ok {
			continue
		}
		g := c.rollGeom
		x, y := int(e.Position.X), int(e.Position.Y)
		if g.cellW > 0 && g.cellH > 0 && x >= g.x0 {
			step := (x - g.x0) / g.cellW
			pitch := g.hi - y/g.cellH
			if step >= 0 && step < g.steps && pitch >= c.seqLo && pitch <= c.seqHi {
				c.toggleNote(step, pitch)
			}
		}
	}

	W := gtx.Constraints.Max.X
	rows := c.seqHi - c.seqLo + 1
	steps := proto.SeqSteps
	if rows < 1 || steps < 1 || W <= 0 {
		return D{Size: image.Pt(W, gtx.Constraints.Max.Y)}
	}
	gutter := gtx.Dp(40)
	cellW := (W - gutter) / steps
	availH := gtx.Constraints.Max.Y
	cellH := availH / rows
	if cellH < gtx.Dp(9) {
		cellH = gtx.Dp(9)
	}
	H := cellH * rows
	// Never report more than we were given (see the note kept from the host-clock version): a Flexed
	// child returning an oversized height pushes the outer Flex's Rigid children off their place.
	if H > availH {
		H = availH
	}
	c.rollGeom = rollGeom{x0: gutter, cellW: cellW, cellH: cellH, steps: steps, hi: c.seqHi}

	area := clip.Rect{Max: image.Pt(W, H)}.Push(gtx.Ops)
	event.Op(gtx.Ops, &c.rollTag)
	area.Pop()

	for r := 0; r < rows; r++ {
		pitch := c.seqHi - r
		y := r * cellH
		bg := colKnobBody
		if isBlackKey(pitch) {
			bg = rgb(0x161B22)
		}
		fillRRect(gtx.Ops, image.Rect(gutter, y, W, y+cellH-1), 0, bg)
		if ((pitch%12)+12)%12 == 0 { // C: label the octave
			off := op.Offset(image.Pt(gtx.Dp(4), y+cellH/2-gtx.Dp(7))).Push(gtx.Ops)
			label(c.th, unit.Sp(10), noteName(pitch), colFaint).Layout(gtx)
			off.Pop()
		}
		for s := 0; s < steps; s++ {
			x := gutter + s*cellW
			col := colTrack
			if s == c.seqStepSel { // faint column tint for the p-lock-selected step
				col = rgba(0x3B8EF5, 0x22)
			}
			if c.noteOn(s, pitch) {
				col = colAccent
			}
			fillRRect(gtx.Ops, image.Rect(x+1, y+1, x+cellW-1, y+cellH-2), gtx.Dp(2), col)
		}
	}
	return D{Size: image.Pt(W, H)}
}

// plockPanel is the compact per-step parameter-lock editor. p-locks override a param only while the
// clock sits on this step — the engine reads them in build_synth_params (device side).
func (c *Controller) plockPanel(gtx C, snap device.Snapshot) D {
	st := c.pattern[c.seqStepSel]
	pname, prange := "—", ""
	if c.plockParam < len(snap.Params) {
		p := snap.Params[c.plockParam]
		pname = p.Name
		prange = fmt.Sprintf("%s…%s", fmtF1(p.Min), fmtF1(p.Max))
	}
	head := fmt.Sprintf("P-lock шага %d", c.seqStepSel+1)

	// Row 1: existing locks as removable chips (or a hint).
	chips := make([]layout.FlexChild, 0, len(st.Plocks)+1)
	if len(st.Plocks) == 0 {
		chips = append(chips, layout.Rigid(label(c.th, unit.Sp(12), "нет локов", colFaint).Layout))
	}
	for i, pl := range st.Plocks {
		i, pl := i, pl
		txt := fmt.Sprintf("%s=%s ✕", paramName(snap, pl.ID), fmtF1(pl.Val))
		chips = append(chips, layout.Rigid(func(gtx C) D {
			if i >= len(c.plockRmBtns) {
				return D{}
			}
			return layout.Inset{Right: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
				stl := pill{border: colLine2, text: colTxt, size: unit.Sp(12), padX: 8, padY: 4, radius: 6}
				return c.plockRmBtns[i].Layout(gtx, func(gtx C) D { return stl.draw(gtx, c.th, txt) })
			})
		}))
	}

	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(label(c.th, unit.Sp(12), head, colMuted).Layout),
		layout.Rigid(layout.Spacer{Height: unit.Dp(5)}.Layout),
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx, chips...)
		}),
		layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.plockPrev, "◀", false, false, false) }),
				layout.Rigid(func(gtx C) D {
					return layout.Inset{Left: unit.Dp(8), Right: unit.Dp(8)}.Layout(gtx,
						label(c.th, unit.Sp(13), pname, colTxt).Layout)
				}),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.plockNext, "▶", false, false, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
				layout.Rigid(label(c.th, unit.Sp(11), prange, colFaint).Layout),
				layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
				layout.Flexed(1, func(gtx C) D { return c.editorBox(gtx, &c.plockVal, "значение") }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.plockAdd, "Добавить", true, false, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.plockClear, "Очистить", false, true, false) }),
			)
		}),
	)
}

func (c *Controller) seqBrowserBar(gtx C) D {
	msg := c.seqMsg
	if msg == "" {
		msg = "Паттерны хранятся в памяти синта (папки = дерево). Play проигрывает без ПК."
	}
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
				layout.Rigid(label(c.th, unit.Sp(12), "Имя", colMuted).Layout),
				layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
				layout.Flexed(1, func(gtx C) D { return c.editorBox(gtx, &c.seqName, "имя или Папка/Имя") }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.seqSaveBtn, "Сохранить", true, false, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.seqLoadBtn, "Загрузить", false, false, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.seqRenameBtn, "Переим.", false, false, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.seqDeleteBtn, "Удалить", false, true, false) }),
				layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
				layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.seqListBtn, "Обновить", false, false, false) }),
			)
		}),
		layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
		layout.Rigid(label(c.th, unit.Sp(11.5), msg, colFaint).Layout),
	)
}

func (c *Controller) layoutSeqTree(gtx C, snap device.Snapshot) D {
	if len(snap.Patterns) == 0 {
		return label(c.th, unit.Sp(13), "Паттернов в синте нет — задай имя и «Сохранить».", colFaint).Layout(gtx)
	}
	return material.List(c.th, &c.seqScroll).Layout(gtx, len(snap.Patterns), func(gtx C, i int) D {
		if i >= len(c.seqEntryBtns) {
			return D{}
		}
		p := snap.Patterns[i]
		disp := p.Path
		switch {
		case disp == "":
			disp = fmt.Sprintf("(слот %d)", p.Slot)
		default:
			if idx := strings.LastIndex(disp, "/"); idx >= 0 {
				disp = disp[:idx] + " / " + disp[idx+1:]
			}
		}
		st := pill{border: colLine2, text: colTxt, size: unit.Sp(13), padX: 10, padY: 6, radius: 6}
		if c.seqSelOK && p.Slot == c.seqSel {
			st.border, st.text, st.fill = colAccent, colAccentB, colAccentDim
		}
		return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2)}.Layout(gtx, func(gtx C) D {
			return c.seqEntryBtns[i].Layout(gtx, func(gtx C) D { return st.draw(gtx, c.th, disp) })
		})
	})
}

// --- small helpers ---

// fixedH lays out w with a capped height so a scrollable list can sit inside a Rigid without eating
// the flexed grid's space.
func fixedH(gtx C, h int, w layout.Widget) D {
	gtx.Constraints.Min.Y = 0
	gtx.Constraints.Max.Y = h
	return w(gtx)
}

func (c *Controller) setParamByName(snap device.Snapshot, name string, val float32) {
	if c.dev == nil {
		return
	}
	if id, ok := paramID(snap, name); ok {
		c.dev.SetParam(id, val)
	}
}

func paramVal(snap device.Snapshot, name string) (float32, bool) {
	for _, p := range snap.Params {
		if p.Name == name {
			return p.Cur, true
		}
	}
	return 0, false
}

func paramName(snap device.Snapshot, id uint16) string {
	if p, ok := snap.Param(id); ok {
		return p.Name
	}
	return fmt.Sprintf("#%d", id)
}

// setPlock replaces the lock for id if present, else appends one (capped at SeqMaxPlocks).
func setPlock(locks []proto.SeqPlock, id uint16, val float32) []proto.SeqPlock {
	out := append([]proto.SeqPlock{}, locks...)
	for i := range out {
		if out[i].ID == id {
			out[i].Val = val
			return out
		}
	}
	if len(out) < proto.SeqMaxPlocks {
		out = append(out, proto.SeqPlock{ID: id, Val: val})
	}
	return out
}
