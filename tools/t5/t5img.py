# SPDX-License-Identifier: Apache-2.0
"""Tab5 screenshots on the host: RGB565 frames to PNG.

The device sends the scanned-out framebuffer in the panel's native portrait
orientation (720x1280, or that divided) as little-endian RGB565, with the
logical rotation alongside; this turns it the way the screen is held.
Stdlib only.
"""
import base64
import re
import struct
import zlib

_B64_LINE = re.compile(r"^[A-Za-z0-9+/]{4,64}={0,2}$")


def rgb565_to_rgb(data, w, h):
    """w*h little-endian RGB565 pixels -> packed RGB888 bytes. Channels are
    widened by bit replication, so 0 and full scale map to 0 and 255."""
    n = w * h
    if len(data) < n * 2:
        raise ValueError(f"short frame: {len(data)} of {n * 2} bytes")
    out = bytearray(n * 3)
    for i, p in enumerate(struct.unpack("<%dH" % n, bytes(data[:n * 2]))):
        r, g, b = p >> 11, (p >> 5) & 63, p & 31
        out[i * 3] = (r << 3) | (r >> 2)
        out[i * 3 + 1] = (g << 2) | (g >> 4)
        out[i * 3 + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def rotate(rgb, w, h, deg):
    """Packed RGB888, w x h, native portrait -> (rgb, w, h) at the logical
    rotation deg (0, 90, 180, 270)."""
    if deg == 0:
        return rgb, w, h
    px = [rgb[i:i + 3] for i in range(0, w * h * 3, 3)]
    if deg == 180:
        return b"".join(reversed(px)), w, h
    if deg not in (90, 270):
        raise ValueError(f"rotation {deg}")
    out = [b""] * (w * h)
    for y in range(h):
        for x in range(w):
            nx, ny = (y, w - 1 - x) if deg == 90 else (h - 1 - y, x)
            out[ny * h + nx] = px[y * w + x]
    return b"".join(out), h, w


def png(rgb, w, h):
    """Packed RGB888 -> PNG file bytes."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def save(path, data565, w, h, rot):
    """Write a native-orientation RGB565 frame as a PNG turned to rot; returns
    the PNG's (width, height)."""
    rgb, ow, oh = rotate(rgb565_to_rgb(data565, w, h), w, h, rot)
    with open(path, "wb") as f:
        f.write(png(rgb, ow, oh))
    return ow, oh


def parse_console_dump(text):
    """The console's `shot` / `cam snap` reply (firmware: t5_diag/diag_media.c):
    "rotation R", then "SHOT W H", base64 lines, "END". Lines that aren't
    base64 are log output from other tasks and are skipped.
    Returns (rotation, w, h, rgb565 bytes)."""
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
        elif inside and line and _B64_LINE.match(line) and len(line) % 4 == 0:
            try:
                data += base64.b64decode(line)
            except ValueError:
                continue
    if not w or len(data) < w * h * 2:
        raise ValueError("incomplete dump: %d of %d bytes" % (len(data), w * h * 2))
    return rot, w, h, bytes(data[:w * h * 2])
