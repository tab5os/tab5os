#!/usr/bin/env python3
"""Fetch a screenshot (or a camera snapshot) from a Tab5 over the console.

usage: t5shot.py [--port PORT] [--div N] [--cam] OUT.png

`shot N` dumps the scanned-out framebuffer (native 720x1280 portrait) at 1/N
size as base64 RGB565; this rotates it to the logical orientation and writes
a PNG. `--cam` uses `cam snap` (the camera preview) instead. Stdlib plus
pyserial (the ESP-IDF Python env).
"""
import argparse
import base64
import os
import struct
import sys
import re
import zlib

B64 = re.compile(r"^[A-Za-z0-9+/]{4,64}={0,2}$")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from t5link import Tab5  # noqa: E402


def parse(text):
    rot, w, h, data, inside = 0, 0, 0, bytearray(), False
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("rotation "):
            rot = int(line.split()[1])
        elif line.startswith("SHOT "):
            _, w, h = line.split()
            w, h, inside = int(w), int(h), True
        elif line == "END":
            inside = False
        elif inside and line:
            if not B64.match(line) or len(line) % 4:
                continue                      # a log line from another task, or a torn line
            try:
                data += base64.b64decode(line)
            except ValueError:
                continue
    if not w or len(data) < w * h * 2:
        raise SystemExit("incomplete dump: %d of %d bytes" % (len(data), w * h * 2))
    px = struct.unpack("<%dH" % (w * h), bytes(data[: w * h * 2]))
    rgb = [((p >> 11) << 3 | (p >> 13), ((p >> 5) & 63) << 2 | ((p >> 9) & 3), (p & 31) << 3 | ((p >> 2) & 7))
           for p in px]
    return rot, w, h, rgb


def rotate(w, h, rgb, deg):
    """The panel is portrait; logical landscape (90) is the panel turned."""
    if deg == 0:
        return w, h, rgb
    if deg == 180:
        return w, h, rgb[::-1]
    out = [None] * (w * h)
    for y in range(h):
        for x in range(w):
            if deg == 90:      # logical (x', y') = (h-1-y, x)... panel column becomes row
                nx, ny = y, w - 1 - x
            else:
                nx, ny = h - 1 - y, x
            out[ny * h + nx] = rgb[y * w + x]
    return h, w, out


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(c for p in rgb[y * w:(y + 1) * w] for c in p) for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--div", type=int, default=None,
                    help="scale down by N (screen: default 2; --cam: 1280x720 / N, default 4)")
    ap.add_argument("--cam", action="store_true")
    ap.add_argument("--rot", type=int, help="override the rotation")
    ap.add_argument("out")
    a = ap.parse_args()
    with Tab5(a.port, timeout=60) as t:
        if a.cam:
            div = a.div or 4
            text = t.cmd("cam snap %d %d" % (1280 // div, 720 // div), timeout=180)
        else:
            text = t.cmd("shot %d" % (a.div or 2), timeout=90)
    rot, w, h, rgb = parse(text)
    if a.cam:
        rot = 0
    if a.rot is not None:
        rot = a.rot
    w, h, rgb = rotate(w, h, rgb, rot)
    write_png(a.out, w, h, rgb)
    print("%s: %dx%d (rotation %d)" % (a.out, w, h, rot))


if __name__ == "__main__":
    main()
