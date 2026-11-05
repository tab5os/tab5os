"""Talk to the Tab5 bring-up console over USB Serial/JTAG (pyserial).

    from t5link import Tab5
    with Tab5() as t:
        print(t.cmd("sys"))
"""
import glob
import time

import serial

PROMPT = b"tab5> "


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def wait_port(timeout=60.0):
    end = time.time() + timeout
    while time.time() < end:
        p = find_port()
        if p:
            return p
        time.sleep(0.2)
    return None


class Tab5:
    def __init__(self, port=None, timeout=20.0):
        self.port = port or find_port()
        if not self.port:
            raise RuntimeError("no Tab5 found")
        self.timeout = timeout
        self.ser = None

    def open(self):
        self.ser = serial.Serial(self.port, 115200, timeout=0.1)
        self.ser.write(b"\r\n")
        self._read_until_prompt(2)
        return self

    def close(self):
        if self.ser:
            self.ser.close()
            self.ser = None

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        self.close()

    def _read_until_prompt(self, timeout):
        buf = b""
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(PROMPT.rstrip()):
                    return buf, True
        return buf, False

    def cmd(self, line, timeout=None):
        """Run one command; return its output without the echo and prompt."""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b"\r\n")
        out, ok = self._read_until_prompt(timeout or self.timeout)
        text = out.decode(errors="replace").replace("\r", "")
        body = text.rsplit(PROMPT.decode(), 1)[0]
        lines = body.split("\n")
        if lines and line in lines[0]:
            lines = lines[1:]
        if not ok:
            raise TimeoutError(f"no prompt after '{line}': {text[-300:]}")
        return "\n".join(lines).strip()

    def reset(self, settle=3.0):
        """Chip reset via RTS (USB Serial/JTAG), then reconnect."""
        self.ser.dtr = False
        self.ser.rts = True
        time.sleep(0.1)
        self.ser.rts = False
        self.close()
        time.sleep(settle)
        self.port = wait_port(30) or self.port
        end = time.time() + 15
        while time.time() < end:
            try:
                return self.open()
            except (serial.SerialException, OSError):
                time.sleep(0.5)
        raise RuntimeError("Tab5 did not come back after reset")
