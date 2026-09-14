#!/usr/bin/env python3
"""Capture the X4 debug console for a fixed duration and dump it to stdout.

Alternative to `pio device monitor` for scripted captures (miniterm crashes in
non-TTY contexts). By default pulses RTS to reset the board first, so the
capture includes boot output; pass --no-reset to only read what the running
firmware emits.

Requires pyserial (devcontainer: apt python3-serial; the PlatformIO venv at
/opt/platformio/bin/python also carries it). Needs rw access to the port.

Usage:
    bin/capture_serial.py /dev/ttyACM0 10
    bin/capture_serial.py /dev/ttyACM0 120 --no-reset > tmp/capture.txt
"""

import argparse
import sys
import time

import serial


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", help="serial device, e.g. /dev/ttyACM0")
    parser.add_argument("seconds", type=float, help="capture duration")
    parser.add_argument(
        "--no-reset",
        action="store_true",
        help="do not pulse RTS (CHIP_PU) to reset the board before reading",
    )
    parser.add_argument(
        "--baud", type=int, default=115200, help="baud rate (default 115200)"
    )
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    if not args.no_reset:
        # Classic reset pulse over USB-Serial/JTAG: RTS drives CHIP_PU inverted.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.1)
    ser.reset_input_buffer()

    end = time.time() + args.seconds
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
