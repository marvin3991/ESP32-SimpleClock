#!/usr/bin/env python3
"""Capture what the clock is showing, over USB serial, as a PNG.

    .venv/bin/python tools/screenshot.py out.png [--port /dev/cu.usbmodemXXXX]

The firmware renders the current frame again band by band and streams it
(the ESP32-C6 has no PSRAM, so there is no frame buffer to read back). The
stream ends with a CRC-32 of the pixels; a damaged capture is retried.
"""
import argparse
import glob
import re
import sys
import time
import zlib

import serial
from PIL import Image

HEADER_TIMEOUT_S = 5
DATA_TIMEOUT_S = 20
ATTEMPTS = 3


class CaptureError(Exception):
    pass


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    if not ports:
        sys.exit("no USB serial port found; pass --port")
    return ports[0]


def capture(port):
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        ser.reset_input_buffer()
        ser.write(b"shot\n")
        ser.flush()
        deadline = time.time() + HEADER_TIMEOUT_S
        while True:
            if time.time() > deadline:
                raise CaptureError("timeout waiting for SHOT header")
            line = ser.readline().decode("utf-8", "replace").strip()
            if line.startswith("SHOT "):
                _, w, h, size = line.split()
                w, h, size = int(w), int(h), int(size)
                break
        data = bytearray()
        deadline = time.time() + DATA_TIMEOUT_S
        while len(data) < size:
            if time.time() > deadline:
                raise CaptureError("timeout: got %d of %d bytes" % (len(data), size))
            data += ser.read(size - len(data))
        tail = ser.read(64).decode("utf-8", "replace")
        end = re.search(r"SHOT_END ([0-9a-f]{8})", tail)
        if not end:
            raise CaptureError("capture aborted by device (%r)" % tail.strip())
        if int(end.group(1), 16) != zlib.crc32(data):
            raise CaptureError("checksum mismatch: the stream was damaged")
    return w, h, bytes(data)


def to_png(w, h, raw, path):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)   # RGB565 little endian
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px[i % w, i // w] = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", nargs="?", default="screenshot.png")
    ap.add_argument("--port")
    args = ap.parse_args()
    port = args.port or find_port()
    for attempt in range(1, ATTEMPTS + 1):
        try:
            w, h, raw = capture(port)
            break
        except CaptureError as err:
            print("attempt %d/%d failed: %s" % (attempt, ATTEMPTS, err), file=sys.stderr)
            if attempt == ATTEMPTS:
                sys.exit(1)
            time.sleep(1)
    to_png(w, h, raw, args.out)
    print("saved %s (%dx%d)" % (args.out, w, h))


if __name__ == "__main__":
    main()
