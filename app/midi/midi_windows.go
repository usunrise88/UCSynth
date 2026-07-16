//go:build windows

package midi

// WinMM MIDI-input via stdlib syscall — no cgo, so the controller still cross-builds from Linux.
//
// Mechanism: CALLBACK_WINDOW. WinMM PostMessages MM_MIM_DATA to a hidden message-only window; a
// GetMessage loop on a goroutine locked to that window's OS thread retrieves it. We do NOT use:
//   - CALLBACK_FUNCTION — its callback fires from a thread WinMM created, and syscall.NewCallback
//     has no extra "m" for a foreign thread without cgo, so it hangs in lockextra (golang/go#20823).
//   - CALLBACK_THREAD — confirmed on the user's Arturia MME driver to open/start fine but never post
//     to the thread queue (open=0 start=0 pump msgs=0). Driver support for it is not universal.
// CALLBACK_WINDOW is the broadly-supported path, and the window's messages are pumped by our own
// thread, so no cgo is needed. Untestable here (needs a Windows host + MIDI device); parse.go holds
// the unit-tested decoder.

import (
	"errors"
	"runtime"
	"sync"
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

	pGetModuleHandleW   = kernel32.NewProc("GetModuleHandleW")
	pRegisterClassExW   = user32.NewProc("RegisterClassExW")
	pCreateWindowExW    = user32.NewProc("CreateWindowExW")
	pDestroyWindow      = user32.NewProc("DestroyWindow")
	pDefWindowProcW     = user32.NewProc("DefWindowProcW")
	pGetMessageW        = user32.NewProc("GetMessageW")
	pDispatchMessageW   = user32.NewProc("DispatchMessageW")
	pPostThreadMessageW = user32.NewProc("PostThreadMessageW")
	pGetCurrentThreadId = kernel32.NewProc("GetCurrentThreadId")
)

const (
	callbackWindow = 0x00010000 // CALLBACK_WINDOW
	mimData        = 0x3C3      // MM_MIM_DATA
	wmQuit         = 0x0012     // WM_QUIT
)

func hwndMessage() uintptr { return ^uintptr(2) } // HWND_MESSAGE == (HWND)-3

type midiInCapsW struct {
	wMid           uint16
	wPid           uint16
	vDriverVersion uint32
	szPname        [32]uint16
	dwSupport      uint32
}

// wndClassExW mirrors WNDCLASSEXW.
type wndClassExW struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

// winMsg mirrors MSG (x64: Go inserts the same padding after `message`).
type winMsg struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	ptX     int32
	ptY     int32
}

// live diagnostics, surfaced in the MIDI status line (the WinMM transport can't run here). Each
// counter/flag makes a failure legible from the running app.
var (
	dbgStage atomic.Uint32 // 0 none, 1 window✗, 2 window✓, 3 open✗, 4 open✓, 5 start✗, 6 pumping
	dbgCode  atomic.Uint32 // last MMRESULT (open/start)
	dbgCount atomic.Uint64 // MM_MIM_DATA messages seen
	dbgLast  atomic.Uint32 // last packed message
)

// Debug reports the transport stage + message count so a failure is diagnosable on the user's box.
func Debug() string {
	st := dbgStage.Load()
	if st == 0 {
		return ""
	}
	stage := []string{"", "окно✗", "окно✓", "open✗ код=", "open✓", "start✗ код=", "работает"}[st]
	s := "MIDI: " + stage
	if st == 3 || st == 5 {
		s += utoa(uint64(dbgCode.Load()))
	}
	if st >= 4 {
		s += " · msgs=" + utoa(dbgCount.Load())
		if dbgCount.Load() > 0 {
			s += " last=" + hex6(dbgLast.Load())
		}
	}
	return s
}

var classOnce sync.Once
var className, _ = syscall.UTF16PtrFromString("UCSynthMidiWin")

func ensureClass() {
	classOnce.Do(func() {
		hInst, _, _ := pGetModuleHandleW.Call(0)
		wc := wndClassExW{
			lpfnWndProc:   pDefWindowProcW.Addr(),
			hInstance:     hInst,
			lpszClassName: className,
		}
		wc.cbSize = uint32(unsafe.Sizeof(wc))
		pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))) // ignore "already registered"
	})
}

type winInput struct {
	handle uintptr
	hwnd   uintptr
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
	dbgStage.Store(0)
	dbgCode.Store(0)
	dbgCount.Store(0)
	dbgLast.Store(0)
	ensureClass()

	w := &winInput{done: make(chan struct{})}
	ready := make(chan struct{})
	started := make(chan error, 1)

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(w.done)

		// The window must be created on (and pumped by) this locked thread.
		hInst, _, _ := pGetModuleHandleW.Call(0)
		hwnd, _, _ := pCreateWindowExW.Call(
			0, uintptr(unsafe.Pointer(className)), 0, 0,
			0, 0, 0, 0, hwndMessage(), 0, hInst, 0,
		)
		tid, _, _ := pGetCurrentThreadId.Call()
		w.hwnd = hwnd
		w.tid = uint32(tid)
		close(ready)

		if err := <-started; err != nil {
			if hwnd != 0 {
				pDestroyWindow.Call(hwnd)
			}
			return
		}
		dbgStage.Store(6)
		var msg winMsg
		for {
			r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
			if int32(r) <= 0 { // 0 = WM_QUIT, -1 = error
				break
			}
			if msg.message == mimData {
				raw := pickMidi(msg.wParam, msg.lParam, w.handle)
				dbgCount.Add(1)
				dbgLast.Store(raw)
				handler(ParseWord(raw))
			} else {
				pDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
			}
		}
		pDestroyWindow.Call(w.hwnd)
	}()

	<-ready
	if w.hwnd == 0 {
		dbgStage.Store(1)
		started <- errors.New("window")
		return nil, errors.New("не удалось создать окно для MIDI")
	}
	dbgStage.Store(2)

	ret, _, _ := pMidiInOpen.Call(uintptr(unsafe.Pointer(&w.handle)), uintptr(index), w.hwnd, 0, callbackWindow)
	dbgCode.Store(uint32(ret))
	if ret != 0 {
		dbgStage.Store(3)
		started <- errors.New("open")
		return nil, errors.New("midiInOpen не удалось (код " + utoa(uint64(ret)) + ")")
	}
	dbgStage.Store(4)
	r, _, _ := pMidiInStart.Call(w.handle)
	dbgCode.Store(uint32(r))
	if r != 0 {
		dbgStage.Store(5)
		pMidiInClose.Call(w.handle)
		started <- errors.New("start")
		return nil, errors.New("midiInStart не удалось (код " + utoa(uint64(r)) + ")")
	}
	started <- nil
	return w, nil
}

// pickMidi returns the packed short MIDI message from MM_MIM_DATA — the param that isn't the known
// device handle. Fallback: whichever looks like packed MIDI (3 bytes, status byte in the low byte).
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
