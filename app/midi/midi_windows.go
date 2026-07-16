//go:build windows

package midi

// WinMM MIDI-input via stdlib syscall — no cgo, so the controller still cross-builds from Linux.
//
// Mechanism: CALLBACK_WINDOW to a hidden message-only window, pumped by GetMessage. EVERYTHING
// (create window, midiInOpen, midiInStart, the pump, and teardown) runs on ONE goroutine locked to
// one OS thread — WinMM's window messages are thread-affine, so opening from a different thread than
// the pump can silently deliver nothing. Not CALLBACK_FUNCTION (foreign-thread callback hangs a
// cgo-free binary, golang/go#20823) and not CALLBACK_THREAD (the user's Arturia MME driver opened
// fine but posted nothing to the thread queue). Untestable here; parse.go holds the tested decoder.

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
	pGetCurrentThreadId = kernel32.NewProc("GetCurrentThreadId")
	pRegisterClassExW   = user32.NewProc("RegisterClassExW")
	pCreateWindowExW    = user32.NewProc("CreateWindowExW")
	pDestroyWindow      = user32.NewProc("DestroyWindow")
	pDefWindowProcW     = user32.NewProc("DefWindowProcW")
	pGetMessageW        = user32.NewProc("GetMessageW")
	pDispatchMessageW   = user32.NewProc("DispatchMessageW")
	pPostThreadMessageW = user32.NewProc("PostThreadMessageW")
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

type winMsg struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	ptX     int32
	ptY     int32
}

// live diagnostics, surfaced in the MIDI status line (the WinMM transport can't run here).
var (
	dbgStage   atomic.Uint32 // 0 none,1 win✗,2 win✓,3 open✗,4 open✓,5 start✗,6 pumping
	dbgCode    atomic.Uint32 // last MMRESULT
	dbgTotal   atomic.Uint64 // any window message retrieved
	dbgLastMsg atomic.Uint32 // last message id
	dbgData    atomic.Uint64 // MM_MIM_DATA messages
	dbgLast    atomic.Uint32 // last packed MIDI message
)

// Debug reports the transport stage + counters so a failure is diagnosable on the user's box:
//
//	MIDI: работает · data=<n> всего=<n> id=<hex> last=<hex>
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
	if st >= 6 {
		s += " · data=" + utoa(dbgData.Load()) + " всего=" + utoa(dbgTotal.Load())
		if dbgTotal.Load() > 0 {
			s += " id=" + hex4(dbgLastMsg.Load())
		}
		if dbgData.Load() > 0 {
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
		wc := wndClassExW{lpfnWndProc: pDefWindowProcW.Addr(), hInstance: hInst, lpszClassName: className}
		wc.cbSize = uint32(unsafe.Sizeof(wc))
		pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))) // ignore "already registered"
	})
}

type winInput struct {
	tid  uint32
	done chan struct{}
}

func (w *winInput) Close() error {
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

// Open runs the whole WinMM lifecycle on one locked OS thread and returns once the device is started
// (or failed). The pump keeps running until Close posts WM_QUIT.
func Open(index int, handler func(Message)) (Input, error) {
	dbgStage.Store(0)
	dbgCode.Store(0)
	dbgTotal.Store(0)
	dbgLastMsg.Store(0)
	dbgData.Store(0)
	dbgLast.Store(0)
	ensureClass()

	w := &winInput{done: make(chan struct{})}
	res := make(chan error, 1)

	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(w.done)

		hInst, _, _ := pGetModuleHandleW.Call(0)
		hwnd, _, _ := pCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), 0, 0, 0, 0, 0, 0, hwndMessage(), 0, hInst, 0)
		tid, _, _ := pGetCurrentThreadId.Call()
		w.tid = uint32(tid)
		if hwnd == 0 {
			dbgStage.Store(1)
			res <- errors.New("не удалось создать окно для MIDI")
			return
		}
		dbgStage.Store(2)

		var handle uintptr
		ret, _, _ := pMidiInOpen.Call(uintptr(unsafe.Pointer(&handle)), uintptr(index), hwnd, 0, callbackWindow)
		dbgCode.Store(uint32(ret))
		if ret != 0 {
			dbgStage.Store(3)
			pDestroyWindow.Call(hwnd)
			res <- errors.New("midiInOpen не удалось (код " + utoa(uint64(ret)) + ")")
			return
		}
		dbgStage.Store(4)
		if r, _, _ := pMidiInStart.Call(handle); r != 0 {
			dbgStage.Store(5)
			dbgCode.Store(uint32(r))
			pMidiInClose.Call(handle)
			pDestroyWindow.Call(hwnd)
			res <- errors.New("midiInStart не удалось (код " + utoa(uint64(r)) + ")")
			return
		}
		dbgStage.Store(6)
		res <- nil // started OK — Open returns; the pump continues below

		var msg winMsg
		for {
			r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
			if int32(r) <= 0 { // 0 = WM_QUIT, -1 = error
				break
			}
			dbgTotal.Add(1)
			dbgLastMsg.Store(msg.message)
			if msg.message == mimData {
				raw := pickMidi(msg.wParam, msg.lParam, handle)
				dbgData.Add(1)
				dbgLast.Store(raw)
				handler(ParseWord(raw))
			} else {
				pDispatchMessageW.Call(uintptr(unsafe.Pointer(&msg)))
			}
		}
		pMidiInStop.Call(handle)
		pMidiInReset.Call(handle)
		pMidiInClose.Call(handle)
		pDestroyWindow.Call(hwnd)
	}()

	if err := <-res; err != nil {
		return nil, err
	}
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

const hexDigits = "0123456789ABCDEF"

func hex4(v uint32) string {
	return string([]byte{hexDigits[(v>>12)&0xF], hexDigits[(v>>8)&0xF], hexDigits[(v>>4)&0xF], hexDigits[v&0xF]})
}

func hex6(v uint32) string {
	return string([]byte{hexDigits[(v>>20)&0xF], hexDigits[(v>>16)&0xF], hexDigits[(v>>12)&0xF], hexDigits[(v>>8)&0xF], hexDigits[(v>>4)&0xF], hexDigits[v&0xF]})
}
