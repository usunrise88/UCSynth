package device

import (
	"net"
	"testing"
	"time"

	"ucsynth/app/proto"
)

func testRegistry() []proto.Param {
	return []proto.Param{
		{ID: 0, Type: proto.TypeFloat, Min: 0, Max: 1, Def: 0.8, Cur: 0.8, Name: "master_volume"},
		{ID: 20, Type: proto.TypeFloat, Min: 20, Max: 20000, Def: 20000, Cur: 880, Name: "cutoff"},
		{ID: 31, Type: proto.TypeInt, Min: 1, Max: 8, Def: 1, Cur: 1, Name: "poly_voices"},
		{ID: 22, Type: proto.TypeEnum, Min: 0, Max: 3, Def: 0, Cur: 0, Name: "filter_mode"},
	}
}

func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for: %s", what)
}

func TestSyncSetClampStat(t *testing.T) {
	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{Heap: 8_650_000, CPUPermille: 618})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()

	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })
	if got := len(d.Snapshot().Params); got != 4 {
		t.Fatalf("registry has %d params, want 4", got)
	}

	// SET cutoff above max → firmware clamps to 20000; the VALUE echo updates the model.
	d.SetParam(20, 99999)
	waitFor(t, "cutoff clamped to 20000", func() bool { p, _ := d.Snapshot().Param(20); return p.Cur == 20000 })

	// SET int poly_voices=3.7 → quantized to 4.
	d.SetParam(31, 3.7)
	waitFor(t, "poly_voices quantized to 4", func() bool { p, _ := d.Snapshot().Param(31); return p.Cur == 4 })

	// STAT poll populates metrics.
	waitFor(t, "STAT cpu", func() bool { return d.Snapshot().Stat.CPUPermille == 618 })

	// Notes get ACKed; exercise the priority path (no model change, no crash).
	d.NoteOn(60, 100)
	d.NoteOff(60)
}

func TestErroredOnConnClose(t *testing.T) {
	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	// Close even on the error path: leaving the device open leaks reader/writer/syncer for the rest
	// of the package run, which both hides real leaks and races later tests.
	defer d.Close()
	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })

	_ = c2.Close() // simulate unplug → device reader Read errors
	waitFor(t, "Errored", func() bool { return d.Snapshot().State == Errored })
	if d.Snapshot().Err == nil {
		t.Fatal("expected an error recorded on connection loss")
	}
}

func TestPresetFlow(t *testing.T) {
	c1, c2 := net.Pipe()
	fake := NewFake(c2, testRegistry(), proto.Stat{})
	go fake.Run()

	d := New(c1, nil)
	d.Start()
	defer d.Close()
	waitFor(t, "Synced", func() bool { return d.Snapshot().State == Synced })

	// Move a param, wait for the echo, then save the registry as a new preset in a tree path.
	d.SetParam(0, 0.3)
	waitFor(t, "master_volume=0.3", func() bool { p, _ := d.Snapshot().Param(0); return p.Cur == 0.3 })
	d.PresetSave(proto.PresetSlotNew, "Leads/Test")
	waitFor(t, "preset listed", func() bool {
		ps := d.Snapshot().Presets
		return len(ps) == 1 && ps[0].Slot == 0 && ps[0].Path == "Leads/Test"
	})

	// Change the param, then load → device re-LISTs → cur restored from the preset.
	d.SetParam(0, 0.9)
	waitFor(t, "master_volume=0.9", func() bool { p, _ := d.Snapshot().Param(0); return p.Cur == 0.9 })
	d.PresetLoad(0)
	waitFor(t, "load restored 0.3", func() bool { p, _ := d.Snapshot().Param(0); return p.Cur == 0.3 })

	// Rename moves it in the tree (values untouched).
	d.PresetRename(0, "Bass/Test")
	waitFor(t, "renamed", func() bool {
		ps := d.Snapshot().Presets
		return len(ps) == 1 && ps[0].Path == "Bass/Test"
	})

	// Loading an empty slot surfaces ERR_NO_PRESET.
	d.PresetLoad(99)
	waitFor(t, "ERR_NO_PRESET", func() bool { return d.Snapshot().LastErr == proto.ErrNoPreset })

	// Delete empties the directory.
	d.PresetDelete(0)
	waitFor(t, "deleted", func() bool { return len(d.Snapshot().Presets) == 0 })
}
