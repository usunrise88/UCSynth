package ui

import (
	"fmt"
	"image"
	"strings"

	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"ucsynth/app/patch"
	"ucsynth/app/proto"
)

func (c *Controller) refreshPatches() {
	list, err := c.store.List()
	if err != nil {
		c.patchMsg = "список: " + err.Error()
	}
	c.patchList = list
	c.patchBtns = make([]widget.Clickable, len(list))
	c.patchInit = true
}

// currentPatch snapshots the live parameter values into a patch keyed by param name.
func (c *Controller) currentPatch(name string) patch.Patch {
	p := patch.Patch{Name: name, Params: map[string]float32{}}
	if c.dev != nil {
		for _, pr := range c.dev.Snapshot().Params {
			p.Params[pr.Name] = pr.Cur
		}
	}
	return p
}

// applyPatch SETs every known param from p and returns how many were applied.
func (c *Controller) applyPatch(p patch.Patch) int {
	if c.dev == nil {
		return 0
	}
	idByName := map[string]uint16{}
	for _, pr := range c.dev.Snapshot().Params {
		idByName[pr.Name] = pr.ID
	}
	n := 0
	for name, val := range p.Params {
		if id, ok := idByName[name]; ok {
			c.dev.SetParam(id, val)
			n++
		}
	}
	return n
}

func (c *Controller) handlePatches(gtx C) {
	if c.saveBtn.Clicked(gtx) {
		name := strings.TrimSpace(c.patchName.Text())
		switch {
		case name == "":
			c.patchMsg = "введи имя патча (можно Папка/Имя)"
		case c.dev == nil:
			c.patchMsg = "нет подключения — нечего сохранять"
		default:
			if err := c.store.Save(name, c.currentPatch(name)); err != nil {
				c.patchMsg = "сохранение: " + err.Error()
			} else {
				c.patchSel = name
				c.patchMsg = "сохранён: " + name
				c.refreshPatches()
			}
		}
	}
	for i := range c.patchBtns {
		if i < len(c.patchList) && c.patchBtns[i].Clicked(gtx) {
			rel := c.patchList[i].Rel
			if p, err := c.store.Load(rel); err != nil {
				c.patchMsg = "загрузка: " + err.Error()
			} else {
				n := c.applyPatch(p)
				c.patchSel = rel
				c.patchName.SetText(p.Name)
				c.patchMsg = fmt.Sprintf("загружен %s (%d парам.)", rel, n)
			}
		}
	}
	if c.reloadBtn.Clicked(gtx) && c.patchSel != "" {
		if p, err := c.store.Load(c.patchSel); err == nil {
			c.patchMsg = fmt.Sprintf("перезагружен %s (%d парам.)", c.patchSel, c.applyPatch(p))
		}
	}
	if c.deleteBtn.Clicked(gtx) && c.patchSel != "" {
		if err := c.store.Delete(c.patchSel); err != nil {
			c.patchMsg = "удаление: " + err.Error()
		} else {
			c.patchMsg = "удалён: " + c.patchSel
			c.patchSel = ""
			c.refreshPatches()
		}
	}
	if c.importBtn.Clicked(gtx) {
		path := strings.TrimSpace(c.patchPath.Text())
		if path == "" {
			c.patchMsg = "укажи путь к .json"
		} else if p, err := patch.ImportFile(path); err != nil {
			c.patchMsg = "импорт: " + err.Error()
		} else {
			c.patchMsg = fmt.Sprintf("импортирован (%d парам.)", c.applyPatch(p))
		}
	}
	if c.exportBtn.Clicked(gtx) {
		path := strings.TrimSpace(c.patchPath.Text())
		name := strings.TrimSpace(c.patchName.Text())
		if name == "" {
			name = "patch"
		}
		if path == "" {
			c.patchMsg = "укажи путь к .json"
		} else if err := patch.ExportFile(path, c.currentPatch(name)); err != nil {
			c.patchMsg = "экспорт: " + err.Error()
		} else {
			c.patchMsg = "экспортирован → " + path
		}
	}

	c.handleDevPresets(gtx)
}

// handleDevPresets drives the "пресеты синта" (on-device NVS) section. The name box (patchName) doubles
// as the tree path for both file patches and device presets.
func (c *Controller) handleDevPresets(gtx C) {
	// Keep the local mirror and the button slice in sync with the device once per frame, and fetch the
	// directory once whenever we (re)connect to a device.
	if c.dev != nil {
		c.devPresets = c.dev.Snapshot().Presets
		if c.dev != c.devPresetListedFor {
			c.dev.PresetList()
			c.devPresetListedFor = c.dev
		}
	} else {
		c.devPresets = nil
		c.devPresetListedFor = nil
		c.devPresetSelOK = false
	}
	if len(c.devPresetBtns) != len(c.devPresets) {
		c.devPresetBtns = make([]widget.Clickable, len(c.devPresets))
	}

	if c.devListBtn.Clicked(gtx) && c.dev != nil {
		c.dev.PresetList()
	}
	if c.devSaveBtn.Clicked(gtx) {
		name := strings.TrimSpace(c.patchName.Text())
		switch {
		case name == "":
			c.patchMsg = "введи имя пресета (можно Папка/Имя)"
		case c.dev == nil:
			c.patchMsg = "нет подключения к синту"
		default:
			c.dev.PresetSave(proto.PresetSlotNew, name)
			c.patchMsg = "сохранён в синт: " + name
		}
	}
	if c.devLoadBtn.Clicked(gtx) {
		if c.dev == nil || !c.devPresetSelOK {
			c.patchMsg = "выбери пресет синта в списке"
		} else {
			c.dev.PresetLoad(c.devPresetSel)
			c.patchMsg = "загружен из синта"
		}
	}
	if c.devRenameBtn.Clicked(gtx) {
		name := strings.TrimSpace(c.patchName.Text())
		switch {
		case c.dev == nil || !c.devPresetSelOK:
			c.patchMsg = "выбери пресет синта и задай новый путь"
		case name == "":
			c.patchMsg = "задай новый путь (Папка/Имя)"
		default:
			c.dev.PresetRename(c.devPresetSel, name)
			c.patchMsg = "переименован в синте: " + name
		}
	}
	if c.devDeleteBtn.Clicked(gtx) {
		if c.dev == nil || !c.devPresetSelOK {
			c.patchMsg = "выбери пресет синта в списке"
		} else {
			c.dev.PresetDelete(c.devPresetSel)
			c.devPresetSelOK = false
			c.patchMsg = "удалён из синта"
		}
	}
	for i := range c.devPresetBtns {
		if i < len(c.devPresets) && c.devPresetBtns[i].Clicked(gtx) {
			c.devPresetSel = c.devPresets[i].Slot
			c.devPresetSelOK = true
			c.patchName.SetText(c.devPresets[i].Path)
			c.patchMsg = "выбран пресет синта: " + c.devPresets[i].Path
		}
	}
}

func (c *Controller) layoutPatches(gtx C) D {
	if !c.patchInit {
		c.refreshPatches()
	}
	return card(gtx, colLine, colPanel, func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
					layout.Rigid(label(c.th, unit.Sp(12), "Имя", colMuted).Layout),
					layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
					layout.Flexed(1, func(gtx C) D { return c.editorBox(gtx, &c.patchName, "имя или Папка/Имя") }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.saveBtn, "Сохранить", true, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.reloadBtn, "Перезагрузить", false, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.deleteBtn, "Удалить", false, true, false) }),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
					layout.Rigid(label(c.th, unit.Sp(12), "Файл", colMuted).Layout),
					layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
					layout.Flexed(1, func(gtx C) D {
						return c.editorBox(gtx, &c.patchPath, `путь к .json (например C:\patches\lead.json)`)
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.importBtn, "Импорт", false, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.exportBtn, "Экспорт", false, false, false) }),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			// Device presets (stage 6): reuse the Name box above as the tree path.
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Axis: layout.Horizontal, Alignment: layout.Middle}.Layout(gtx,
					layout.Rigid(label(c.th, unit.Sp(12), "Синт", colMuted).Layout),
					layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.devSaveBtn, "Сохранить в синт", true, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.devLoadBtn, "Загрузить", false, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.devRenameBtn, "Переименовать", false, false, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.devDeleteBtn, "Удалить", false, true, false) }),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D { return c.obtn(gtx, &c.devListBtn, "Обновить", false, false, false) }),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				msg := c.patchMsg
				if msg == "" {
					msg = "Файлы — JSON в " + c.store.Root + "; пресеты синта — в его памяти (папки = дерево)"
				}
				return label(c.th, unit.Sp(11.5), msg, colFaint).Layout(gtx)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Horizontal}.Layout(gtx,
					layout.Flexed(1, func(gtx C) D {
						return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
							layout.Rigid(label(c.th, unit.Sp(12), "Файлы (ПК)", colMuted).Layout),
							layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
							layout.Flexed(1, c.layoutPatchTree),
						)
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(12)}.Layout),
					layout.Flexed(1, func(gtx C) D {
						return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
							layout.Rigid(label(c.th, unit.Sp(12), "Пресеты синта", colMuted).Layout),
							layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
							layout.Flexed(1, c.layoutDevPresetTree),
						)
					}),
				)
			}),
		)
	})
}

func (c *Controller) layoutPatchTree(gtx C) D {
	if len(c.patchList) == 0 {
		return label(c.th, unit.Sp(13), "Патчей пока нет — задай имя и «Сохранить».", colFaint).Layout(gtx)
	}
	return material.List(c.th, &c.patchScroll).Layout(gtx, len(c.patchList), func(gtx C, i int) D {
		e := c.patchList[i]
		disp := e.Name
		if e.Dir != "" {
			disp = e.Dir + " / " + e.Name
		}
		st := pill{border: colLine2, text: colTxt, size: unit.Sp(13), padX: 10, padY: 6, radius: 6}
		if e.Rel == c.patchSel {
			st.border, st.text, st.fill = colAccent, colAccentB, colAccentDim
		}
		return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2)}.Layout(gtx, func(gtx C) D {
			return c.patchBtns[i].Layout(gtx, func(gtx C) D { return st.draw(gtx, c.th, disp) })
		})
	})
}

// layoutDevPresetTree renders the on-device (NVS) presets, tree grouped by each entry's path.
func (c *Controller) layoutDevPresetTree(gtx C) D {
	if c.dev == nil {
		return label(c.th, unit.Sp(13), "Нет подключения к синту.", colFaint).Layout(gtx)
	}
	if len(c.devPresets) == 0 {
		return label(c.th, unit.Sp(13), "В синте пресетов нет — «Сохранить в синт».", colFaint).Layout(gtx)
	}
	return material.List(c.th, &c.devPresetScroll).Layout(gtx, len(c.devPresets), func(gtx C, i int) D {
		if i >= len(c.devPresetBtns) {
			return D{}
		}
		p := c.devPresets[i]
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
		if c.devPresetSelOK && p.Slot == c.devPresetSel {
			st.border, st.text, st.fill = colAccent, colAccentB, colAccentDim
		}
		return layout.Inset{Top: unit.Dp(2), Bottom: unit.Dp(2)}.Layout(gtx, func(gtx C) D {
			return c.devPresetBtns[i].Layout(gtx, func(gtx C) D { return st.draw(gtx, c.th, disp) })
		})
	})
}

// editorBox draws a bordered text box (accent border when focused) filling the available width.
func (c *Controller) editorBox(gtx C, ed *widget.Editor, hint string) D {
	macro := op.Record(gtx.Ops)
	dims := layout.UniformInset(unit.Dp(7)).Layout(gtx, func(gtx C) D {
		e := material.Editor(c.th, ed, hint)
		e.Color = colTxt
		e.HintColor = colFaint
		e.TextSize = unit.Sp(13)
		return e.Layout(gtx)
	})
	call := macro.Stop()
	dims.Size.X = gtx.Constraints.Max.X
	if min := gtx.Dp(30); dims.Size.Y < min {
		dims.Size.Y = min
	}
	r := image.Rectangle{Max: dims.Size}
	fillRRect(gtx.Ops, r, gtx.Dp(6), colKnobBody)
	border := colLine2
	if gtx.Focused(ed) {
		border = colAccent
	}
	strokeRRect(gtx.Ops, r, gtx.Dp(6), float32(gtx.Dp(1)), border)
	call.Add(gtx.Ops)
	return dims
}
