"""Host checks for t5img (run by `t5 host-test`)."""
import base64
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t5img  # noqa: E402


def frame(pixels):
    return struct.pack("<%dH" % len(pixels), *pixels)


def read_png(path):
    """(w, h, rgb bytes) of a PNG written by t5img (one IDAT, filter 0)."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, {}
    while pos < len(data):
        n, = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        assert crc == zlib.crc32(kind + body) & 0xFFFFFFFF
        chunks[kind] = body
        pos += 12 + n
    w, h = struct.unpack(">II", chunks[b"IHDR"][:8])
    raw = zlib.decompress(chunks[b"IDAT"])
    rows = [raw[y * (w * 3 + 1):(y + 1) * (w * 3 + 1)] for y in range(h)]
    assert all(r[0] == 0 for r in rows)
    return w, h, b"".join(r[1:] for r in rows)


class Rgb565(unittest.TestCase):
    def test_extremes_and_primaries(self):
        rgb = t5img.rgb565_to_rgb(frame([0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F]), 5, 1)
        self.assertEqual(rgb, bytes([0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 0, 255]))

    def test_every_level_is_close_to_linear(self):
        for v in range(32):
            r = t5img.rgb565_to_rgb(frame([v << 11]), 1, 1)[0]
            self.assertLessEqual(abs(r - v * 255 / 31), 1)
        for v in range(64):
            g = t5img.rgb565_to_rgb(frame([v << 5]), 1, 1)[1]
            self.assertLessEqual(abs(g - v * 255 / 63), 1)

    def test_short_frame(self):
        with self.assertRaises(ValueError):
            t5img.rgb565_to_rgb(b"\0" * 5, 2, 2)


class Rotate(unittest.TestCase):
    # A 2x3 native portrait frame; each pixel's red channel is its index.
    W, H = 2, 3
    RGB = b"".join(bytes([i, 0, 0]) for i in range(6))

    def grid(self, rgb, w, h):
        return [[rgb[(y * w + x) * 3] for x in range(w)] for y in range(h)]

    def test_0_and_180(self):
        self.assertEqual(t5img.rotate(self.RGB, 2, 3, 0), (self.RGB, 2, 3))
        rgb, w, h = t5img.rotate(self.RGB, 2, 3, 180)
        self.assertEqual(self.grid(rgb, w, h), [[5, 4], [3, 2], [1, 0]])

    def test_90_and_270_swap_sides(self):
        rgb, w, h = t5img.rotate(self.RGB, 2, 3, 90)
        self.assertEqual((w, h), (3, 2))
        self.assertEqual(self.grid(rgb, w, h), [[1, 3, 5], [0, 2, 4]])
        rgb, w, h = t5img.rotate(self.RGB, 2, 3, 270)
        self.assertEqual(self.grid(rgb, w, h), [[4, 2, 0], [5, 3, 1]])

    def test_90_then_270_is_identity(self):
        rgb, w, h = t5img.rotate(self.RGB, 2, 3, 90)
        # Turning back: 270 of the rotated frame, treated as native.
        back, bw, bh = t5img.rotate(rgb, w, h, 270)
        self.assertEqual((back, bw, bh), (self.RGB, 2, 3))

    def test_bad_rotation(self):
        with self.assertRaises(ValueError):
            t5img.rotate(self.RGB, 2, 3, 45)


class Save(unittest.TestCase):
    def test_png_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "x.png")
            self.assertEqual(t5img.save(path, frame([0xF800, 0x07E0, 0x001F, 0xFFFF, 0, 0]), 2, 3, 90), (3, 2))
            w, h, rgb = read_png(path)
        self.assertEqual((w, h), (3, 2))
        self.assertEqual(rgb[:3], bytes([0, 255, 0]))      # native (1, 0) is top left at 90


class ConsoleDump(unittest.TestCase):
    def dump(self, data, w, h, rot=270, noise=()):
        lines = [f"rotation {rot}", f"SHOT {w} {h}"]
        for i in range(0, len(data), 48):
            lines.append(base64.b64encode(data[i:i + 48]).decode())
            lines.extend(noise)
        return "\n".join(lines + ["END", ""])

    def test_parse(self):
        data = bytes(range(256)) * 3
        self.assertEqual(t5img.parse_console_dump(self.dump(data, 16, 24)), (270, 16, 24, data))

    def test_log_lines_between_are_skipped(self):
        data = bytes(range(96))
        text = self.dump(data, 6, 8, 0, noise=["I (1234) t5.net: connected", "abc"])
        self.assertEqual(t5img.parse_console_dump(text)[3], data)

    def test_incomplete(self):
        with self.assertRaises(ValueError):
            t5img.parse_console_dump(self.dump(bytes(48), 8, 8))


if __name__ == "__main__":
    unittest.main()
