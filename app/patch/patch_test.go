package patch

import (
	"path/filepath"
	"testing"
)

func TestRoundTripJSON(t *testing.T) {
	p := Patch{Name: "Lead", Params: map[string]float32{"cutoff": 2400, "resonance": 0.3}}
	b, err := Marshal(p)
	if err != nil {
		t.Fatal(err)
	}
	got, err := Unmarshal(b)
	if err != nil {
		t.Fatal(err)
	}
	if got.Name != "Lead" || got.Params["cutoff"] != 2400 || got.Params["resonance"] != 0.3 {
		t.Fatalf("round-trip mismatch: %+v", got)
	}
}

func TestStoreSaveListLoadDelete(t *testing.T) {
	s := Store{Root: t.TempDir()}

	// Missing subfolders are created; the tree is walked.
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	must(s.Save("Bass/Sub", Patch{Name: "Sub", Params: map[string]float32{"cutoff": 200}}))
	must(s.Save("Lead", Patch{Name: "Lead", Params: map[string]float32{"cutoff": 8000}}))

	entries, err := s.List()
	must(err)
	if len(entries) != 2 {
		t.Fatalf("expected 2 entries, got %d: %+v", len(entries), entries)
	}
	// sorted by rel: "Bass/Sub" then "Lead"
	if entries[0].Rel != "Bass/Sub" || entries[0].Dir != "Bass" || entries[0].Name != "Sub" {
		t.Fatalf("bad first entry: %+v", entries[0])
	}
	if entries[1].Rel != "Lead" || entries[1].Dir != "" {
		t.Fatalf("bad second entry: %+v", entries[1])
	}

	got, err := s.Load("Bass/Sub")
	must(err)
	if got.Params["cutoff"] != 200 {
		t.Fatalf("loaded wrong value: %+v", got)
	}

	must(s.Delete("Lead"))
	entries, err = s.List()
	must(err)
	if len(entries) != 1 {
		t.Fatalf("expected 1 after delete, got %d", len(entries))
	}
}

func TestListMissingRootIsEmpty(t *testing.T) {
	s := Store{Root: filepath.Join(t.TempDir(), "does-not-exist")}
	entries, err := s.List()
	if err != nil {
		t.Fatalf("missing root should not error: %v", err)
	}
	if len(entries) != 0 {
		t.Fatalf("expected empty, got %+v", entries)
	}
}

func TestPathRejectsTraversal(t *testing.T) {
	s := Store{Root: t.TempDir()}
	for _, bad := range []string{"", "..", "../escape", "../../etc/passwd"} {
		if err := s.Save(bad, Patch{}); err == nil {
			t.Fatalf("expected rejection for %q", bad)
		}
	}
}
