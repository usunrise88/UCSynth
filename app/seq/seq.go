// Package seq is a PC-side step sequencer (piano-roll): a grid of steps × MIDI pitches driven by a
// clock. On each step it releases the previous step's notes and sounds the current step's, via an
// emit callback wired to the device. The Player owns the grid and guards it with a mutex, so the UI
// goroutine and the clock goroutine can both touch it safely. The step logic (advance) is separate
// from real time, so it is unit-tested without a clock.
package seq

import (
	"sync"
	"time"
)

// Player is the sequencer. Construct with New; drive playback with Start/Stop.
type Player struct {
	mu       sync.Mutex
	steps    int
	lo, hi   int // inclusive MIDI pitch range
	cells    map[[2]int]bool
	bpm      int
	playing  bool
	cur      int
	sounding []int
	stopCh   chan struct{}

	emit   func(note int, on bool)
	redraw func()
}

// New builds a player over [lo,hi] pitches and `steps` columns at `bpm` (steps are 16th notes).
// emit sends note on/off to the device; redraw (may be nil) asks the UI to repaint the playhead.
func New(steps, lo, hi, bpm int, emit func(note int, on bool), redraw func()) *Player {
	if steps < 1 {
		steps = 1
	}
	if bpm < 20 {
		bpm = 20
	}
	return &Player{
		steps: steps, lo: lo, hi: hi, bpm: bpm, cur: -1,
		cells: map[[2]int]bool{}, emit: emit, redraw: redraw,
	}
}

func (p *Player) Steps() int    { return p.steps }
func (p *Player) Lo() int       { return p.lo }
func (p *Player) Hi() int       { return p.hi }
func (p *Player) BPM() int      { p.mu.Lock(); defer p.mu.Unlock(); return p.bpm }
func (p *Player) Playing() bool { p.mu.Lock(); defer p.mu.Unlock(); return p.playing }
func (p *Player) Cur() int      { p.mu.Lock(); defer p.mu.Unlock(); return p.cur }

func (p *Player) Toggle(step, pitch int) {
	p.mu.Lock()
	defer p.mu.Unlock()
	k := [2]int{step, pitch}
	if p.cells[k] {
		delete(p.cells, k)
	} else {
		p.cells[k] = true
	}
}

func (p *Player) On(step, pitch int) bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.cells[[2]int{step, pitch}]
}

// Clear removes all notes (does not stop playback).
func (p *Player) Clear() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.cells = map[[2]int]bool{}
}

func (p *Player) stepInterval() time.Duration {
	// 16th notes: four steps per quarter-note beat.
	return time.Minute / time.Duration(p.bpm*4)
}

// SetBPM changes tempo, restarting the clock if playing.
func (p *Player) SetBPM(bpm int) {
	if bpm < 20 {
		bpm = 20
	}
	if bpm > 300 {
		bpm = 300
	}
	p.mu.Lock()
	p.bpm = bpm
	restart := p.playing
	p.mu.Unlock()
	if restart {
		p.Stop()
		p.Start()
	}
}

// Start begins playback from before step 0 (the first tick sounds step 0).
func (p *Player) Start() {
	p.mu.Lock()
	if p.playing {
		p.mu.Unlock()
		return
	}
	p.playing = true
	p.cur = -1
	p.stopCh = make(chan struct{})
	stop := p.stopCh
	interval := p.stepInterval()
	p.mu.Unlock()

	go func() {
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				p.advance()
			}
		}
	}()
}

// Stop halts playback and releases any sounding notes.
func (p *Player) Stop() {
	p.mu.Lock()
	if !p.playing {
		p.mu.Unlock()
		return
	}
	p.playing = false
	close(p.stopCh)
	offs := p.sounding
	p.sounding = nil
	p.cur = -1
	redraw := p.redraw
	p.mu.Unlock()

	for _, n := range offs {
		p.emit(n, false)
	}
	if redraw != nil {
		redraw()
	}
}

// advance performs one step: release the previous step's notes, move to the next step, sound it.
// Emit calls happen outside the lock so a blocking device write can't stall UI access to the grid.
func (p *Player) advance() {
	p.mu.Lock()
	if !p.playing {
		p.mu.Unlock()
		return
	}
	offs := p.sounding
	p.cur = (p.cur + 1) % p.steps
	var ons []int
	for pitch := p.lo; pitch <= p.hi; pitch++ {
		if p.cells[[2]int{p.cur, pitch}] {
			ons = append(ons, pitch)
		}
	}
	p.sounding = append([]int(nil), ons...)
	redraw := p.redraw
	p.mu.Unlock()

	for _, n := range offs {
		p.emit(n, false)
	}
	for _, n := range ons {
		p.emit(n, true)
	}
	if redraw != nil {
		redraw()
	}
}
