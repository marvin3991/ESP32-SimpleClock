#!/usr/bin/env python3
"""Send commands to the clock's USB serial console and print the replies.

    .venv/bin/python tools/console.py status
    .venv/bin/python tools/console.py "btn key" "level 3" status
    .venv/bin/python tools/console.py --listen 10      # just print the log
"""
import argparse
import glob
import sys
import time

import serial


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no USB serial port found; pass --port")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--port")
    ap.add_argument("--wait", type=float, default=0.6, help="seconds to collect output per command")
    ap.add_argument("--listen", type=float, default=0, help="seconds to print the log afterwards")
    args = ap.parse_args()
    with serial.Serial(args.port or find_port(), 115200, timeout=0.1) as ser:
        ser.reset_input_buffer()
        for cmd in args.commands:
            print("> " + cmd)
            ser.write(cmd.encode() + b"\n")
            end = time.time() + args.wait
            while time.time() < end:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
        end = time.time() + args.listen
        while time.time() < end:
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()


if __name__ == "__main__":
    main()
