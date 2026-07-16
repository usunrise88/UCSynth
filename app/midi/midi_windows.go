//go:build windows

package midi

// WinMM MIDI-input via stdlib syscall — no cgo, so the controller still cross-builds from Linux.
//
// It uses CALLBACK_THREAD, NOT CALLBACK_FUNCTION: a foreign-thread C callback (what
// CALLBACK_FUNCTION needs) hangs a cgo-free Go binary — syscall.NewCallback has no extra "m" for a
// thread the runtime didn't create, so it blocks forever in lockextra (golang/go#20823, #9240).
// Instead WinMM posts MM_MIM_DATA to a thread message queue, which we pump with GetMessage on a
// goroutine locked to its own OS thread. Untestable here (needs a Windows host + a MIDI device);
// the pure decoder in parse.go carries the unit-tested logic.

import (
	"errors"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

var (
	winmm    = syscall.NewLazyDLL("winmm.dll")
	user32   = syscall.NewLazyDLL("user32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")

	pMidiInGetNumDevs  = winmm.NewProc("midiInGetNumDevs")
	pMidiInGetDevCapsW = winmm.NewProc("midiInGetDevCapsW")
	pMidiInOpen        = winmm.NewProc("midiInOpen")
	pMidiInStart       = winmm.NewProc("midiInStart")
	pMidiInStop        = winmm.NewProc("midiInStop")
	pMidiInReset       = winmm.NewProc("midiInReset")
	pMidiInClose       = winmm.NewProc("midiInClose")

	pGetCurrentThreadId = kernel32.NewProc("GetCurrentThreadId")
	pGetMessageW        = user32.NewProc("GetMessageW")
	pPeekMessageW       = user32.NewProc("PeekMessageW")
	pPostThreadMessageW = user32.NewProc("PostThreadMessageW")
)

const (
	callbackThread = 0x00020000 // CALLBACK_THREAD
	mimData        = 0x3C3      // MM_MIM_DATA
	wmQuit         = 0x0012     // WM_QUIT
	pmNoRemove     = 0x0000
)

type midiInCapsW struct {
	wMid           uint16
	wPid           uint16
	vDriverVersion uint32
	szPname        [32]uint16
	dwSupport      uint32
}

// winMsg mirrors the Win32 MSG (x64 layout; Go inserts the same padding after `message`).
type winMsg struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	ptX     int32
	ptY     int32
}

// live diagnostics, surfaced in the MIDI status line so a failure is legible on the user's machine
// (I cannot run the WinMM transport here). Each is stored +1 so 0 means "not attempted yet".
var (
	dbgOpen  atomic.Uint32 // midiInOpen MMRESULT + 1
	dbgStart atomic.Uint32 // midiInStart MMRESULT + 1
	dbgPump  atomic.Bool   // GetMessage loop is running
	dbgCount atomic.Uint64 // MM_MIM_DATA messages seen
	dbgLast  atomic.Uint32 // last packed message
)

// Debug reports the transport state so a failure is diagnosable from the running app:
// open=<code> start=<code> pump msgs=<n> last=<hex>. Empty until the first Open attempt.
func Debug() string {
	o := dbgOpen.Load()
	if o == 0 {
		return ""
	}
	s := "open=" + utoa(uint64(o-1))
	if v := dbgStart.Load(); v > 0 {
		s += " start=" + utoa(uint64(v-1))
	}
	if dbgPump.Load() {
		s += " pump"
	}
	s += " msgs=" + utoa(dbgCount.Load())
	if dbgCount.Load() > 0 {
		s += " last=" + hex6(dbgLast.Load())
	}
	return s
}

type winInput struct {
	handle uintptr
	tid    uint32
	done   chan struct{}
}

func (w *winInput) Close() error {
	pMidiInStop.Call(w.handle)
	pMidiInReset.Call(w.handle)
	pMidiInClose.Call(w.handle)
	pPostThreadMessageW.Call(uintptr(w.tid), wmQuit, 0, 0)
	<-w.done
	return nil
}

func List() ([]string, error) {
	n, _, _ := pMidiInGetNumDevs.Call()
	var names []string
	for i := uintptr(0); i < n; i++ {
		var caps midiInCapsW
		if ret, _, _ := pMidiInGetDevCapsW.Call(i, uintptr(unsafe.Pointer(&caps)), unsafe.Sizeof(caps)); ret == 0 {
			names = append(names, syscall.UTF16ToString(caps.szPname[:]))
		}
	}
	return names, nil
}

func Open(index int, handler func(Message)) (Input, error) {
	dbgOpen.Store(0)
	dbgStart.Store(0)
	dbgPump.Store(false)
	dbgCount.Store(0)
	dbgLast.Store(0)

	w := &winInput{done: make(chan struct{})}
	tidCh := make(chan uint32, 1)
	started := make(chan error, 1)

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(w.done)

		// Force this thread's message queue to exist before WinMM posts to it.
		var msg winMsg
		pPeekMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0, pmNoRemove)
		tid, _, _ := pGetCurrentThreadId.Call()
		tidCh <- uint32(tid)

		if err := <-started; err != nil {
			return // open/start failed; nothing to pump
		}
		dbgPump.Store(true)
		defer dbgPump.Store(false)
		for {
			r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
			if int32(r) <= 0 { // 0 = WM_QUIT, -1 = error
				return
			}
			if msg.message == mimData {
				raw := pickMidi(msg.wParam, msg.lParam, w.handle)
				dbgCount.Add(1)
				dbgLast.Store(raw)
				handler(ParseWord(raw))
			}
		}
	}()

	w.tid = <-tidCh
	ret, _, _ := pMidiInOpen.Call(
		uintptr(unsafe.Pointer(&w.handle)),
		uintptr(index),
		uintptr(w.tid),
		0,
		callbackThread,
	)
	dbgOpen.Store(uint32(ret) + 1)
	if ret != 0 {
		started <- errors.New("open")
		return nil, errors.New("midiInOpen не удалось (код " + utoa(uint64(ret)) + ")")
	}
	r, _, _ := pMidiInStart.Call(w.handle)
	dbgStart.Store(uint32(r) + 1)
	if r != 0 {
		pMidiInClose.Call(w.handle)
		started <- errors.New("start")
		return nil, errors.New("midiInStart не удалось (код " + utoa(uint64(r)) + ")")
	}
	started <- nil
	return w, nil
}

// pickMidi returns the packed short MIDI message from the posted MM_MIM_DATA. WinMM puts the device
// handle in one of wParam/lParam and the data (dwParam1) in the other; sources disagree on which, so
// we pick the one that ISN'T the known handle. Fallback: whichever looks like packed MIDI — 3 bytes
// with a status byte (bit 7 set) in the low byte.
func pickMidi(wp, lp, handle uintptr) uint32 {
	if wp == handle {
		return uint32(lp)
	}
	if lp == handle {
		return uint32(wp)
	}
	looksMidi := func(v uint32) bool { return v>>24 == 0 && v&0x80 != 0 }
	if looksMidi(uint32(lp)) {
		return uint32(lp)
	}
	return uint32(wp)
}

func utoa(v uint64) string {
	if v == 0 {
		return "0"
	}
	var b [24]byte
	i := len(b)
	for v > 0 {
		i--
		b[i] = byte('0' + v%10)
		v /= 10
	}
	return string(b[i:])
}

func hex6(v uint32) string {
	const d = "0123456789ABCDEF"
	return string([]byte{d[(v>>20)&0xF], d[(v>>16)&0xF], d[(v>>12)&0xF], d[(v>>8)&0xF], d[(v>>4)&0xF], d[v&0xF]})
}
