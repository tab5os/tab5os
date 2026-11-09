"""Host checks for t5link against a scripted console (run by `t5 host-test`;
no board, no pyserial needed)."""
import base64
import os
import re
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t5link  # noqa: E402

P = t5link.PROMPT.encode()


class FakeSerial:
    """Answers what is written with replies from respond(line) -> bytes,
    handed out a few bytes per read to exercise reassembly."""

    def __init__(self, respond, chunk=7):
        self.respond, self.chunk = respond, chunk
        self.out, self.written, self.pending = b"", [], b""

    def write(self, b):
        self.pending += b
        while b"\n" in self.pending:
            line, self.pending = self.pending.split(b"\n", 1)
            line = line.rstrip(b"\r")
            self.written.append(line)
            self.out += self.respond(line)

    def read(self, n):
        n = min(n, self.chunk)
        d, self.out = self.out[:n], self.out[n:]
        return d

    def reset_input_buffer(self):
        self.out = b""

    def close(self):
        pass


def board(ser):
    t = t5link.Tab5(port="/dev/null", timeout=0.5)
    t.ser = ser
    return t


class Cmd(unittest.TestCase):
    def test_reply_without_echo_and_prompt(self):
        ser = FakeSerial(lambda l: l + b"\r\nline one\r\nline two\r\n" + P)
        self.assertEqual(board(ser).cmd("sys"), "line one\nline two")

    def test_log_after_prompt_is_not_part_of_the_reply(self):
        ser = FakeSerial(lambda l: l + b"\r\nanswer\r\n" + P + b"I (99) t5.net: late log line\r\n")
        self.assertEqual(board(ser).cmd("net"), "answer")

    def test_prompt_inside_echo_is_skipped(self):
        line = "py exec print('" + t5link.PROMPT + "')"
        ser = FakeSerial(lambda l: l + b"\r\n" + P[:-1] + b"\r\n" + P)
        self.assertEqual(board(ser).cmd(line), t5link.PROMPT.strip())

    def test_timeout_hides_arguments(self):
        ser = FakeSerial(lambda l: l + b"\r\nworking")
        with self.assertRaises(TimeoutError) as e:
            board(ser).cmd("net connect home hunter2")
        self.assertNotIn("hunter2", str(e.exception))
        self.assertIn("net connect", str(e.exception))

    def test_poll_until_match(self):
        states = iter([b"state active,", b"state active,", b"state sleep,"])
        ser = FakeSerial(lambda l: l + b"\r\n" + next(states) + b" idle 0 ms\r\n" + P)
        m = board(ser).poll("power", r"^state sleep,", 5, every=0)
        self.assertIsNotNone(m)
        self.assertEqual(len(ser.written), 3)


class Put(unittest.TestCase):
    """The device side of `app put` (diag_app.c put_file), with one damaged line."""

    def device(self, size, damage_line=None):
        got, state = bytearray(), {"n": 0, "ready": False}

        def respond(line):
            if line.startswith(b"app put "):
                state["ready"] = True
                return line + b"\r\nREADY\r\n"
            if not state["ready"]:
                return b""
            state["n"] += 1
            b64, crc = line.split(b" ")
            data = base64.b64decode(b64)
            if state["n"] == damage_line or zlib.crc32(data) != int(crc, 16):
                return b"@re\r\n"
            got.extend(data)
            if len(got) < size:
                return b"@ok\r\n"
            return b"@ok\r\nOK %d %08x\r\n" % (len(got), zlib.crc32(got)) + P
        return respond, got

    def test_sends_everything_and_resends_a_refused_line(self):
        data = bytes(range(256)) * 3
        respond, got = self.device(len(data), damage_line=2)
        ser = FakeSerial(respond, chunk=64)
        board(ser).put(data, t5link.TMP_DIR + "/x.t5app")
        self.assertEqual(bytes(got), data)
        lines = [w for w in ser.written if not w.startswith(b"app put")]
        self.assertEqual(len(lines), -(-len(data) // t5link.PUT_CHUNK) + 1)
        self.assertTrue(all(len(w) < 256 for w in lines))     # the console's receive ring

    def test_device_error_is_raised(self):
        ser = FakeSerial(lambda l: l + b"\r\nput: only under /data/tmp or /sd\r\n" + P)
        with self.assertRaisesRegex(RuntimeError, "put: only under"):
            board(ser).put(b"x", t5link.TMP_DIR + "/x")

    def test_only_where_the_device_accepts(self):
        with self.assertRaises(ValueError):
            board(FakeSerial(lambda l: b"")).put(b"x", "/data/sys/x")
        with self.assertRaises(ValueError):
            board(FakeSerial(lambda l: b"")).put(b"x", t5link.TMP_DIR + "/../sys/x")


class Constants(unittest.TestCase):
    @unittest.skipIf("T5_PROMPT" in os.environ, "prompt overridden")
    def test_prompt_is_the_firmwares(self):
        with open(os.path.join(t5link.t5const.COMPONENTS, "t5_diag", "t5_diag.c")) as f:
            m = re.search(r'repl_cfg\.prompt\s*=\s*"([^"]*)"', f.read())
        self.assertEqual(m.group(1), t5link.PROMPT)


if __name__ == "__main__":
    unittest.main()
