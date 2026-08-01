package ui

import (
	"image"
	"net"
	"testing"
	"time"

	"gioui.org/io/input"
	"gioui.org/layout"
	"gioui.org/op"

	"ucsynth/app/device"
	"ucsynth/app/proto"
)

// TestPatchesTabDevicePresets wires a connected fake device into the controller and lays out the
// patches tab, checking that the on-device preset directory is auto-fetched on connect and rendered
// (the non-nil path of handleDevPresets/layoutDevPresetTree that the disconnected smoke test misses).
func TestPatchesTabDevicePresets(t *testing.T) {
	c := New(func() {})
	c1, c2 := net.Pipe()
	fake := device.NewFake(c2, smokeParams, proto.Stat{})
	go fake.Run()
	dev := device.New(c1, nil)
	dev.Start()
	defer dev.Close()
	c.dev = dev
	c.sink.set(dev)

	var r input.Router
	gtx := layout.Context{Ops: new(op.Ops), Metric: testMetric, Source: r.Source()}
	frame := func() {
		gtx.Reset()
		gtx.Metric = testMetric
		gtx.Constraints = layout.Exact(image.Pt(1100, 760))
		c.tab = tabPatches
		c.Layout(gtx) // handleButtons → handleDevPresets (auto-list); layoutDevPresetTree
		r.Frame(gtx.Ops)
	}

	deadline := time.Now().Add(2 * time.Second)
	synced := func() bool { return dev.Snapshot().State == device.Synced }
	for time.Now().Before(deadline) && !synced() {
		frame()
		time.Sleep(5 * time.Millisecond)
	}
	if !synced() {
		t.Fatal("device never synced")
	}

	// Save a preset on the device; the controller must pick it up via its auto-refresh and render it.
	dev.PresetSave(proto.PresetSlotNew, "Leads/Test")
	got := false
	for time.Now().Before(deadline) {
		frame()
		if len(c.devPresets) == 1 && c.devPresets[0].Path == "Leads/Test" {
			got = true
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if !got {
		t.Fatalf("device preset not reflected in patches tab (devPresets=%v)", c.devPresets)
	}
}
