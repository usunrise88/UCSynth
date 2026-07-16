//go:build windows

package midi

// WinMM MIDI-input binding via stdlib syscall — no cgo, so the controller still cross-builds from
// Linux. Untestable in this environment (needs a Windows host + a MIDI device); the pure decoder in
// parse.go carries the logic that can be unit-tested.

import (
	"errors"
	"sync"
	"syscall"
	"unsafe"
)

var (
	winmm              = syscall.NewLazyDLL("winmm.dll")
	pMidiInGetNumDevs  = winmm.NewProc("midiInGetNumDevs")
	pMidiInGetDevCapsW = winmm.NewProc("midiInGetDevCapsW")
	pMidiInOpen        = winmm.NewProc("midiInOpen")
	pMidiInStart       = winmm.NewProc("midiInStart")
	pMidiInStop        = winmm.NewProc("midiInStop")
	pMidiInReset       = winmm.NewProc("midiInReset")
	pMidiInClose       = winmm.NewProc("midiInClose")
)

const (
	callbackFunction = 0x00030000 // CALLBACK_FUNCTION
	mimData          = 0x3C3      // MIM_DATA
)

// midiInCapsW mirrors MIDIINCAPSW.
type midiInCapsW struct {
	wMid           uint16
	wPid           uint16
	vDriverVersion uint32
	szPname        [32]uint16
	dwSupport      uint32
}

// callback registry — the WinMM callback is a single fixed function pointer; dwCallbackInstance
// carries an id back to the owning input so we can route messages without a per-open closure.
var (
	regMu   sync.Mutex
	regMap  = map[uint32]*winInput{}
	regNext uint32
	cbOnce  sync.Once
	cbPtr   uintptr
)

func regAdd(w *winInput) uint32 {
	regMu.Lock()
	defer regMu.Unlock()
	regNext++
	id := regNext
	regMap[id] = w
	return id
}

func regDel(id uint32) {
	regMu.Lock()
	delete(regMap, id)
	regMu.Unlock()
}

func regGet(id uint32) *winInput {
	regMu.Lock()
	defer regMu.Unlock()
	return regMap[id]
}

// midiInProc is the WinMM callback (runs on a driver thread — keep it non-blocking).
func midiInProc(hMidiIn, wMsg, dwInstance, dwParam1, dwParam2 uintptr) uintptr {
	if uint32(wMsg) == mimData {
		if w := regGet(uint32(dwInstance)); w != nil {
			w.push(ParseWord(uint32(dwParam1)))
		}
	}
	return 0
}

type winInput struct {
	handle  uintptr
	id      uint32
	ch      chan Message
	done    chan struct{}
	handler func(Message)
}

func (w *winInput) push(m Message) {
	select {
	case w.ch <- m:
	default: // drop under backpressure rather than stall the driver thread
	}
}

func (w *winInput) Close() error {
	pMidiInStop.Call(w.handle)
	pMidiInReset.Call(w.handle)
	pMidiInClose.Call(w.handle)
	close(w.done)
	regDel(w.id)
	return nil
}

// List returns the WinMM input device names.
func List() ([]string, error) {
	n, _, _ := pMidiInGetNumDevs.Call()
	var names []string
	for i := uintptr(0); i < n; i++ {
		var caps midiInCapsW
		ret, _, _ := pMidiInGetDevCapsW.Call(i, uintptr(unsafe.Pointer(&caps)), unsafe.Sizeof(caps))
		if ret != 0 {
			continue
		}
		names = append(names, syscall.UTF16ToString(caps.szPname[:]))
	}
	return names, nil
}

// Open opens input device `index` and delivers decoded messages to handler until Close.
func Open(index int, handler func(Message)) (Input, error) {
	cbOnce.Do(func() { cbPtr = syscall.NewCallback(midiInProc) })

	w := &winInput{ch: make(chan Message, 256), done: make(chan struct{}), handler: handler}
	w.id = regAdd(w)

	ret, _, _ := pMidiInOpen.Call(
		uintptr(unsafe.Pointer(&w.handle)),
		uintptr(index),
		cbPtr,
		uintptr(w.id),
		callbackFunction,
	)
	if ret != 0 {
		regDel(w.id)
		return nil, errors.New("midiInOpen не удалось (код " + itoa(ret) + ")")
	}
	if ret, _, _ := pMidiInStart.Call(w.handle); ret != 0 {
		pMidiInClose.Call(w.handle)
		regDel(w.id)
		return nil, errors.New("midiInStart не удалось")
	}

	// forward messages off the driver thread
	go func() {
		for {
			select {
			case <-w.done:
				return
			case m := <-w.ch:
				w.handler(m)
			}
		}
	}()
	return w, nil
}

func itoa(v uintptr) string {
	if v == 0 {
		return "0"
	}
	var b [20]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	return string(b[i:])
}
