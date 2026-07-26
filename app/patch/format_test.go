package patch

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A foreign JSON object must be rejected, not silently accepted as an empty patch. Before the format
// tag existed, "not our file" and "our empty patch" were indistinguishable and the GUI reported
// "импортирован (0 парам.)".
func TestUnmarshalRejectsForeignJSON(t *testing.T) {
	cases := []string{
		`{"hello":"world"}`,
		`{"format":"something-else","params":{"cutoff":100}}`,
		`{}`,
	}
	for _, in := range cases {
		if _, err := Unmarshal([]byte(in)); err == nil {
			t.Fatalf("Unmarshal(%s) accepted a foreign file", in)
		}
	}
}

// Patches written before the format tag existed (name + params only) must keep loading.
func TestUnmarshalAcceptsLegacyPatch(t *testing.T) {
	p, err := Unmarshal([]byte(`{"name":"Old Lead","params":{"cutoff":2400}}`))
	if err != nil {
		t.Fatalf("legacy patch rejected: %v", err)
	}
	if p.Name != "Old Lead" || p.Params["cutoff"] != 2400 {
		t.Fatalf("legacy patch decoded wrong: %+v", p)
	}
}

// A file from a future format version must be refused rather than applied with the wrong semantics.
func TestUnmarshalRejectsNewerVersion(t *testing.T) {
	_, err := Unmarshal([]byte(`{"format":"ucsynth-patch","version":99,"params":{}}`))
	if err == nil {
		t.Fatal("a newer format version was accepted")
	}
	if !strings.Contains(err.Error(), "99") {
		t.Fatalf("error should mention the version it saw: %v", err)
	}
}

func TestSaveLoadRoundTripWithFormat(t *testing.T) {
	s := Store{Root: t.TempDir()}
	want := Patch{Name: "Lead", Params: map[string]float32{"cutoff": 2400, "resonance": 0.3}}
	if err := s.Save("Leads/Lead", want); err != nil {
		t.Fatal(err)
	}
	got, err := s.Load("Leads/Lead")
	if err != nil {
		t.Fatal(err)
	}
	if got.Name != want.Name || got.Params["cutoff"] != 2400 || got.Params["resonance"] != 0.3 {
		t.Fatalf("round trip mismatch: %+v", got)
	}
	if got.Format != formatTag || got.Version != FormatVersion {
		t.Fatalf("saved file lacks the format tag: format=%q version=%d", got.Format, got.Version)
	}
}

// Saving must not destroy the previous version if it fails. The write goes to a temp file plus a
// rename, so a failing rename leaves the old file untouched — emulated here by making the target a
// directory, which is the closest reproducible stand-in for "the write could not complete".
func TestSaveDoesNotDestroyPreviousOnFailure(t *testing.T) {
	root := t.TempDir()
	s := Store{Root: root}
	first := Patch{Name: "Keep", Params: map[string]float32{"cutoff": 1000}}
	if err := s.Save("Keep", first); err != nil {
		t.Fatal(err)
	}
	original, err := os.ReadFile(filepath.Join(root, "Keep.json"))
	if err != nil {
		t.Fatal(err)
	}

	// Make the destination un-renameable-onto: replace it with a non-empty directory.
	fp := filepath.Join(root, "Keep.json")
	if err := os.Remove(fp); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(fp, "blocker"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := s.Save("Keep", Patch{Name: "New", Params: map[string]float32{"cutoff": 2}}); err == nil {
		t.Fatal("expected the save to fail")
	}
	// The directory (our stand-in for the previous content) survived: nothing was truncated first.
	if fi, err := os.Stat(fp); err != nil || !fi.IsDir() {
		t.Fatalf("target was clobbered by a failed save: err=%v", err)
	}

	// And no temp files were left behind in the tree.
	ents, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range ents {
		if strings.HasPrefix(e.Name(), ".tmp-") {
			t.Fatalf("temp file left behind: %s", e.Name())
		}
	}
	_ = original
}

// Temp files must never show up as patches in the listing.
func TestListIgnoresTempFiles(t *testing.T) {
	root := t.TempDir()
	s := Store{Root: root}
	if err := s.Save("Real", Patch{Name: "Real", Params: map[string]float32{}}); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, ".tmp-Real.json-123456"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	list, err := s.List()
	if err != nil {
		t.Fatal(err)
	}
	if len(list) != 1 || list[0].Rel != "Real" {
		t.Fatalf("listing picked up a temp file: %+v", list)
	}
}
