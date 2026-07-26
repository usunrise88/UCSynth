// Package patch stores synth patches as JSON files on the PC — a tree of .json files under a root
// directory. A patch is a name→value map keyed by firmware parameter name (stable per control.h);
// unknown names are skipped on load, so patches survive registry changes. No device storage yet.
package patch

import (
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// FormatVersion is the on-disk patch format version. Bump it when the MEANING of stored values
// changes (a rescaled parameter, a renamed key), not when parameters are merely added — unknown names
// are skipped on load, so additions are already backwards-compatible.
const FormatVersion = 1

// Patch is one saved sound: a display name and parameter values keyed by firmware param name.
type Patch struct {
	// Format identifies the file as ours and pins the value semantics. Without it "not our JSON" and
	// "our empty patch" were indistinguishable: any object at all loaded successfully as an empty
	// patch, and the GUI reported "импортирован (0 парам.)" instead of an error.
	Format  string             `json:"format"`
	Version int                `json:"version"`
	Name    string             `json:"name"`
	Params  map[string]float32 `json:"params"`
}

const formatTag = "ucsynth-patch"

// Marshal/Unmarshal are the on-disk JSON codec (indented, human-readable).
func Marshal(p Patch) ([]byte, error) {
	p.Format = formatTag
	if p.Version == 0 {
		p.Version = FormatVersion
	}
	return json.MarshalIndent(p, "", "  ")
}

func Unmarshal(b []byte) (Patch, error) {
	var p Patch
	if err := json.Unmarshal(b, &p); err != nil {
		return Patch{}, err
	}
	// Files written before the format tag existed have no "format" key but do carry "params" — accept
	// those, reject anything that is neither.
	if p.Format != formatTag {
		if p.Format != "" {
			return Patch{}, fmt.Errorf("это не патч UCSynth (format=%q)", p.Format)
		}
		if p.Params == nil {
			return Patch{}, errors.New("это не патч UCSynth (нет ни format, ни params)")
		}
	}
	if p.Version > FormatVersion {
		return Patch{}, fmt.Errorf("патч версии %d — новее, чем понимает этот пульт (%d)",
			p.Version, FormatVersion)
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
	return writeAtomic(fp, b)
}

// writeAtomic writes via a temp file in the same directory plus a rename, so an interrupted save
// cannot destroy the previous version. os.WriteFile opens with O_TRUNC: it zeroes the file first, so a
// crash / full disk / pulled drive mid-write left an empty or truncated .json — Load then fails to
// parse and the old patch is already gone.
func writeAtomic(fp string, b []byte) error {
	f, err := os.CreateTemp(filepath.Dir(fp), ".tmp-"+filepath.Base(fp)+"-")
	if err != nil {
		return err
	}
	tmp := f.Name()
	defer os.Remove(tmp) // no-op once the rename succeeded

	if _, err := f.Write(b); err != nil {
		f.Close()
		return err
	}
	if err := f.Sync(); err != nil { // data on disk before the rename makes it visible
		f.Close()
		return err
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Chmod(tmp, 0o644); err != nil { // CreateTemp makes it 0600
		return err
	}
	return os.Rename(tmp, fp)
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
	return writeAtomic(path, b)
}

// ImportFile reads a patch JSON from an arbitrary path.
func ImportFile(path string) (Patch, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return Patch{}, err
	}
	return Unmarshal(b)
}
