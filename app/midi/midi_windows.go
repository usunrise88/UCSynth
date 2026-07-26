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
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	// NewLazySystemDLL, а не syscall.NewLazyDLL: последний использует штатный порядок поиска
	// LoadLibrary, где каталог exe идёт РАНЬШЕ System32. user32/kernel32 процесс уже загрузил и они
	// неуязвимы, а winmm подгружается лениво — положи ucsynth-controller.exe рядом с посторонним
	// winmm.dll, и первый же midi.List() при старте GUI выполнит чужой код. NewLazySystemDLL
	// форсирует LOAD_LIBRARY_SEARCH_SYSTEM32. В stdlib его нет, берём из x/sys/windows — он и так
	// уже в зависимостях (через gio), новой поставки не добавляется.
	winmm    = windows.NewLazySystemDLL("winmm.dll")
	user32   = windows.NewLazySystemDLL("user32.dll")
	kernel32 = windows.NewLazySystemDLL("kernel32.dll")

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
		syscall.SyscallN(pRegisterClassExW.Addr(), uintptr(unsafe.Pointer(&wc))) // ignore "already registered"
		runtime.KeepAlive(&wc)
	})
}

// closeTimeout bounds the wait for the message pump to exit. Close is called from the UI goroutine
// (toggleMidi, Shutdown), so an unbounded wait here freezes the window.
const closeTimeout = 2 * time.Second

type winInput struct {
	tid     uint32
	done    chan struct{}
	pumping atomic.Bool // true only while GetMessage is actually being called
}

// Close stops the pump. Two things it deliberately does not do: post WM_QUIT to a thread id whose
// pump has already exited (Windows reuses thread ids, and Gio holds several threads with their own
// message loops — one of them runs the app window, so a stray WM_QUIT could close it), and wait
// forever if the post fails or the pump is wedged.
func (w *winInput) Close() error {
	if !w.pumping.Load() {
		w.waitDone() // already finishing on its own
		return nil
	}
	r, _, err := pPostThreadMessageW.Call(uintptr(w.tid), wmQuit, 0, 0)
	if r == 0 {
		// The pump most likely exited between the check above and this call. Give it the normal
		// grace period, then report rather than hang.
		if w.waitDone() {
			return nil
		}
		return fmt.Errorf("MIDI: PostThreadMessage(WM_QUIT) не удался и насос не завершился: %w", err)
	}
	if !w.waitDone() {
		return errors.New("MIDI: насос сообщений не завершился за " + closeTimeout.String())
	}
	return nil
}

func (w *winInput) waitDone() bool {
	select {
	case <-w.done:
		return true
	case <-time.After(closeTimeout):
		return false
	}
}

// List returns one entry per device, in device-id order.
//
// A device whose caps cannot be read gets a placeholder name and is NEVER skipped: the caller passes
// the slice index straight to Open, which passes it to midiInOpen as uDeviceID. A gap here therefore
// opens a DIFFERENT device than the one clicked — the port opens fine, MM_MIM_OPEN arrives, and the
// keys go to a port nobody is listening on. Caps are cosmetic; the id is load-bearing.
func List() ([]string, error) {
	n, _, _ := pMidiInGetNumDevs.Call()
	names := make([]string, 0, n)
	for i := uintptr(0); i < n; i++ {
		var caps midiInCapsW
		ret, _, _ := syscall.SyscallN(pMidiInGetDevCapsW.Addr(),
			i, uintptr(unsafe.Pointer(&caps)), unsafe.Sizeof(caps))
		runtime.KeepAlive(&caps)
		if ret == 0 {
			names = append(names, syscall.UTF16ToString(caps.szPname[:]))
		} else {
			names = append(names, "MIDI-вход "+utoa(uint64(i))+" (имя недоступно, код "+utoa(uint64(ret))+")")
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
		hwnd, _, _ := syscall.SyscallN(pCreateWindowExW.Addr(),
			0, uintptr(unsafe.Pointer(className)), 0, 0, 0, 0, 0, 0, hwndMessage(), 0, hInst, 0)
		tid, _, _ := pGetCurrentThreadId.Call()
		w.tid = uint32(tid)
		if hwnd == 0 {
			dbgStage.Store(1)
			res <- errors.New("не удалось создать окно для MIDI")
			return
		}
		dbgStage.Store(2)

		var handle uintptr
		ret, _, _ := syscall.SyscallN(pMidiInOpen.Addr(),
			uintptr(unsafe.Pointer(&handle)), uintptr(index), hwnd, 0, callbackWindow)
		runtime.KeepAlive(&handle)
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
		w.pumping.Store(true)
		defer w.pumping.Store(false) // Close must not post WM_QUIT to a tid we no longer own
		res <- nil                   // started OK — Open returns; the pump continues below

		var msg winMsg
		for {
			r, _, _ := syscall.SyscallN(pGetMessageW.Addr(), uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
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
				syscall.SyscallN(pDispatchMessageW.Addr(), uintptr(unsafe.Pointer(&msg)))
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
