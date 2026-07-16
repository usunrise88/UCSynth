// Package patch stores synth patches as JSON files on the PC — a tree of .json files under a root
// directory. A patch is a name→value map keyed by firmware parameter name (stable per control.h);
// unknown names are skipped on load, so patches survive registry changes. No device storage yet.
package patch

import (
	"encoding/json"
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Patch is one saved sound: a display name and parameter values keyed by firmware param name.
type Patch struct {
	Name   string             `json:"name"`
	Params map[string]float32 `json:"params"`
}

// Marshal/Unmarshal are the on-disk JSON codec (indented, human-readable).
func Marshal(p Patch) ([]byte, error) { return json.MarshalIndent(p, "", "  ") }

func Unmarshal(b []byte) (Patch, error) {
	var p Patch
	if err := json.Unmarshal(b, &p); err != nil {
		return Patch{}, err
	}
	if p.Params == nil {
		p.Params = map[string]float32{}
	}
	return p, nil
}

// Store is a directory of patch files. Rel paths use "/" and omit the .json suffix; subdirectories
// form the tree.
type Store struct{ Root string }

// Entry is one patch in the tree listing.
type Entry struct {
	Rel  string // "Leads/Saw" — forward slashes, no extension
	Name string // last path element (display name)
	Dir  string // parent folder ("" at root)
}

// DefaultRoot is <user-config>/UCSynth/patches (…/AppData/Roaming/UCSynth/patches on Windows).
func DefaultRoot() string {
	if d, err := os.UserConfigDir(); err == nil {
		return filepath.Join(d, "UCSynth", "patches")
	}
	return "patches"
}

// path resolves a rel name to an absolute .json path, rejecting traversal outside Root.
func (s Store) path(rel string) (string, error) {
	rel = strings.TrimSpace(strings.TrimSuffix(rel, ".json"))
	if rel == "" {
		return "", errors.New("пустое имя патча")
	}
	relSlash := filepath.ToSlash(rel)
	for _, seg := range strings.Split(relSlash, "/") {
		if seg == ".." {
			return "", errors.New("недопустимое имя патча")
		}
	}
	clean := strings.TrimPrefix(filepath.ToSlash(filepath.Clean("/"+relSlash)), "/")
	if clean == "" {
		return "", errors.New("недопустимое имя патча")
	}
	return filepath.Join(s.Root, filepath.FromSlash(clean)+".json"), nil
}

func (s Store) Save(rel string, p Patch) error {
	fp, err := s.path(rel)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(fp), 0o755); err != nil {
		return err
	}
	b, err := Marshal(p)
	if err != nil {
		return err
	}
	return os.WriteFile(fp, b, 0o644)
}

func (s Store) Load(rel string) (Patch, error) {
	fp, err := s.path(rel)
	if err != nil {
		return Patch{}, err
	}
	b, err := os.ReadFile(fp)
	if err != nil {
		return Patch{}, err
	}
	return Unmarshal(b)
}

func (s Store) Delete(rel string) error {
	fp, err := s.path(rel)
	if err != nil {
		return err
	}
	return os.Remove(fp)
}

// List walks the tree and returns patches sorted by rel path. A missing root yields an empty list.
func (s Store) List() ([]Entry, error) {
	if _, err := os.Stat(s.Root); errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	var out []Entry
	err := filepath.WalkDir(s.Root, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() || !strings.HasSuffix(strings.ToLower(d.Name()), ".json") {
			return nil
		}
		rel, rerr := filepath.Rel(s.Root, p)
		if rerr != nil {
			return nil
		}
		rel = strings.TrimSuffix(filepath.ToSlash(rel), ".json")
		dir, name := "", rel
		if i := strings.LastIndex(rel, "/"); i >= 0 {
			dir, name = rel[:i], rel[i+1:]
		}
		out = append(out, Entry{Rel: rel, Name: name, Dir: dir})
		return nil
	})
	sort.Slice(out, func(i, j int) bool { return out[i].Rel < out[j].Rel })
	return out, err
}

// ExportFile writes a patch as JSON to an arbitrary path (for sharing outside the store).
func ExportFile(path string, p Patch) error {
	b, err := Marshal(p)
	if err != nil {
		return err
	}
	return os.WriteFile(path, b, 0o644)
}

// ImportFile reads a patch JSON from an arbitrary path.
func ImportFile(path string) (Patch, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return Patch{}, err
	}
	return Unmarshal(b)
}
