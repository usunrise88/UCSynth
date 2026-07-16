// Package serial is the real transport: enumerate ports, open one as an io.ReadWriteCloser for
// the device package, and hold DTR/RTS low so opening the port doesn't reset the ESP32-S3 (its
// native USB-Serial-JTAG uses those lines as a reset, the way esptool reboots the chip). cgo-free
// on Windows/Linux via go.bug.st/serial (cgo is only needed for the macOS enumerator, unused here).
package serial

import (
	"fmt"
	"strings"

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

// Open opens a port (baud is irrelevant for USB-Serial-JTAG) and holds DTR/RTS low to avoid
// rebooting the board on connect. Drain the boot log briefly before sending LIST.
func Open(name string) (*Conn, error) {
	port, err := serial.Open(name, &serial.Mode{BaudRate: 115200})
	if err != nil {
		return nil, err
	}
	// Hold reset lines low: opening the port must not reset the S3 (see serialtest.py rts=False).
	_ = port.SetDTR(false)
	_ = port.SetRTS(false)
	return &Conn{port: port}, nil
}

func (c *Conn) Read(p []byte) (int, error)  { return c.port.Read(p) }
func (c *Conn) Write(p []byte) (int, error) { return c.port.Write(p) }
func (c *Conn) Close() error                { return c.port.Close() }
