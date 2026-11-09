"""Host checks for t5const (run by `t5 host-test`)."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t5const  # noqa: E402

HEADER = """
/* a comment with #define NOT_THIS 1 */
#define BASE 0x40u
#define NEXT (BASE + 2)   /* trailing comment */
#define SHIFTED (1 << 3)
#define NAME "com.t5." "x"
#define JOINED NAME ".y"
#define FN(a) ((a) * 2)
typedef enum {
    E_FIRST = 5,
    E_SECOND = NEXT,
} e_t;
"""


class Defines(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp(suffix=".h")
        with os.fdopen(fd, "w") as f:
            f.write(HEADER)
        self.addCleanup(os.remove, self.path)

    def test_values(self):
        self.assertEqual(t5const.define(self.path, "NEXT"), 0x42)
        self.assertEqual(t5const.define(self.path, "SHIFTED"), 8)
        self.assertEqual(t5const.define(self.path, "JOINED"), "com.t5.x.y")
        self.assertEqual(t5const.define(self.path, "E_SECOND"), 0x42)

    def test_prefix(self):
        self.assertEqual(t5const.defines(self.path, "E_"), {"E_FIRST": 5, "E_SECOND": 0x42})

    def test_missing(self):
        for name in ("NOT_THIS", "FN"):
            with self.assertRaises(KeyError):
                t5const.define(self.path, name)


class Firmware(unittest.TestCase):
    def test_xpins(self):
        exps = {t5const.define(t5const.PINS_H, n) for n in ("T5_I2C_ADDR_EXP0", "T5_I2C_ADDR_EXP1")}
        pins = t5const.xpins()
        self.assertIn("T5_XP_CHG_EN", pins)
        for name, (addr, pin) in pins.items():
            self.assertIn(addr, exps, name)
            self.assertTrue(0 <= pin < 8, name)
        self.assertEqual(len(set(pins.values())), len(pins))

    def test_partitions_dont_overlap(self):
        parts = sorted(t5const.partitions().values())
        for (off, size), (nxt, _) in zip(parts, parts[1:]):
            self.assertLessEqual(off + size, nxt)


if __name__ == "__main__":
    unittest.main()
