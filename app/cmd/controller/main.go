// Command controller is the UCSynth desktop GUI pult (этап 2): connect over serial, build param
// blocks from the device registry (LIST), play notes, watch live metrics. Pure-Go Gio; the Windows
// target builds cgo-free (verified by cross-compiling from Linux).
package main

import (
	"log"
	"os"

	"gioui.org/app"
	"gioui.org/op"
	"gioui.org/unit"

	"ucsynth/app/ui"
)

func main() {
	go func() {
		w := new(app.Window)
		w.Option(app.Title("UCSynth Controller"), app.Size(unit.Dp(1120), unit.Dp(780)))
		if err := run(w); err != nil {
			log.Fatal(err)
		}
		os.Exit(0)
	}()
	app.Main()
}

func run(w *app.Window) error {
	ctl := ui.New(w.Invalidate)
	var ops op.Ops
	for {
		switch e := w.Event().(type) {
		case app.DestroyEvent:
			ctl.Shutdown()
			return e.Err
		case app.FrameEvent:
			gtx := app.NewContext(&ops, e)
			ctl.Layout(gtx)
			e.Frame(gtx.Ops)
		}
	}
}
