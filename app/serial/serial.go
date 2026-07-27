// Package serial is the real transport: enumerate ports, open one as an io.ReadWriteCloser for
// the device package, and hold DTR/RTS low so opening the port doesn't reset the ESP32-S3 (its
// native USB-Serial-JTAG uses those lines as a reset, the way esptool reboots the chip). cgo-free
// on Windows/Linux via go.bug.st/serial (cgo is only needed for the macOS enumerator, unused here).
package serial

import (
	"fmt"
	"strings"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// espressifVID is the Espressif Systems USB vendor id; the S3 native USB-Serial-JTAG uses it.
// The CH343/CP210x UART bridge (which does NOT carry the protocol) has a different VID.
const espressifVID = "303A"

// PortInfo describes an available serial port for the connection picker.
type PortInfo struct {
	Name    string // COM7 / /dev/ttyACM0
	Label   string // friendly label with VID/PID hint
	IsSynth bool   // matches the Espressif native-USB VID (likely the right port)
}

// List enumerates serial ports, flagging likely UCSynth (Espressif native USB) ports so the UI
// can steer the user to the correct port and away from a UART-bridge port.
func List() ([]PortInfo, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	out := make([]PortInfo, 0, len(ports))
	for _, p := range ports {
		info := PortInfo{Name: p.Name, Label: p.Name}
		if p.IsUSB {
			if strings.EqualFold(p.VID, espressifVID) {
				info.IsSynth = true
				info.Label = p.Name + "  ·  UCSynth (Espressif)"
			} else if p.VID != "" {
				info.Label = fmt.Sprintf("%s  ·  VID:%s PID:%s", p.Name, p.VID, p.PID)
			}
		}
		out = append(out, info)
	}
	return out, nil
}

// Conn is an open serial connection usable as device's io.ReadWriteCloser.
type Conn struct{ port serial.Port }

const (
	// settleDelay is how long we let the port go quiet before purging it. Anything already in the
	// OS receive buffer at open time — a boot log, half a frame from a killed previous session,
	// output from a concurrently running serialtest.py — would otherwise be fed to the frame
	// decoder as the first bytes of this session and eat the head of the LIST response.
	settleDelay = 200 * time.Millisecond
	// readTimeout keeps Read from blocking forever on a half-open port, so a hung link is
	// distinguishable from an idle one. Timeouts surface as a 0-byte, nil-error read.
	readTimeout = 2 * time.Second
)

// Open opens a port (baud is irrelevant for USB-Serial-JTAG), holds DTR/RTS low so connecting does
// not reset the board, and drains whatever was already buffered before the caller sends LIST.
func Open(name string) (*Conn, error) {
	// InitialStatusBits must be set explicitly: nil means "DTR=true and RTS=true" and the Windows
	// backend applies that inside SetCommState at open time. Calling SetDTR(false) afterwards is too
	// late — the lines have already gone high, and that high→low pulse is exactly what esptool uses
	// to reboot the chip. Setting them here means they are never raised in the first place.
	port, err := serial.Open(name, &serial.Mode{
		BaudRate:          115200,
		InitialStatusBits: &serial.ModemOutputBits{DTR: false, RTS: false},
	})
	if err != nil {
		return nil, err
	}
	if err := port.SetDTR(false); err != nil {
		_ = port.Close()
		return nil, fmt.Errorf("не удалось опустить DTR (порт сбросил бы плату): %w", err)
	}
	if err := port.SetRTS(false); err != nil {
		_ = port.Close()
		return nil, fmt.Errorf("не удалось опустить RTS (порт сбросил бы плату): %w", err)
	}
	if err := port.SetReadTimeout(readTimeout); err != nil {
		_ = port.Close()
		return nil, err
	}

	time.Sleep(settleDelay)
	if err := port.ResetInputBuffer(); err != nil {
		_ = port.Close()
		return nil, err
	}
	return &Conn{port: port}, nil
}

func (c *Conn) Read(p []byte) (int, error)  { return c.port.Read(p) }
func (c *Conn) Write(p []byte) (int, error) { return c.port.Write(p) }
func (c *Conn) Close() error                { return c.port.Close() }
