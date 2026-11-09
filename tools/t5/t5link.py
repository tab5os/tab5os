# SPDX-License-Identifier: Apache-2.0
"""The Tab5's USB console (USB Serial/JTAG) from a computer: finding the port,
resetting the board, waiting for it to come back, running console commands,
sending a file and fetching screenshots. Every host tool and bench script
that talks to the board over USB goes through here.

    from t5link import connect
    with connect() as t:
        print(t.cmd("sys"))

Needs pyserial (the ESP-IDF Python env has it).
"""
import base64
import glob
import os
import re
import time
import zlib

try:
    import serial
    SERIAL_ERRORS = (serial.SerialException, OSError)
except ImportError:          # pure helpers (and their tests) work without it
    serial = None
    SERIAL_ERRORS = (OSError,)

import t5const
import t5img

_PATHS = "t5_storage/include/t5_storage/paths.h"
TMP_DIR = t5const.define(_PATHS, "T5_TMP_DIR")

PORT_GLOBS = ("/dev/cu.usbmodem*", "/dev/ttyACM*")
# The console prompt: repl_cfg.prompt in firmware/components/t5_diag/t5_diag.c.
# The recovery image's is "recovery> " (recovery/main/recovery.c): set T5_PROMPT.
PROMPT = os.environ.get("T5_PROMPT", "tab5> ")

# `app put PATH SIZE`: the protocol is described with T5_DIAG_PUT_CHUNK in
# firmware/components/t5_diag/include/t5_diag/t5_diag.h.
PUT_CHUNK = t5const.define("t5_diag/include/t5_diag/t5_diag.h", "T5_DIAG_PUT_CHUNK")
PUT_MAX = t5const.define("t5_dev/include/t5_dev/control.h", "T5_DEV_PUSH_MAX")
PUT_DIRS = (TMP_DIR + "/", t5const.define(_PATHS, "T5_SD_PATH") + "/")


def find_port():
    """$T5_PORT if set (`t5 --port`), else the first USB serial port."""
    if os.environ.get("T5_PORT"):
        return os.environ["T5_PORT"]
    ports = sorted(p for g in PORT_GLOBS for p in glob.glob(g))
    return ports[0] if ports else None


def wait_port(timeout=60.0):
    end = time.time() + timeout
    while True:
        p = find_port()
        if p or time.time() >= end:
            return p
        time.sleep(0.2)


def wait_port_gone(port, timeout):
    """True once port has disappeared (the board reset and USB re-enumerates)."""
    end = time.time() + timeout
    while time.time() < end:
        if not os.path.exists(port):
            return True
        time.sleep(0.05)
    return False


def reply_text(raw, line):
    """The console's reply to `line` from what was read up to and including the
    prompt: without the echo of the command and without the prompt."""
    text = raw.decode(errors="replace").replace("\r", "")
    body = text.rsplit(PROMPT, 1)[0]
    lines = body.split("\n")
    if lines and line and line in lines[0]:
        lines = lines[1:]
    return "\n".join(lines).strip()


def _redact(line, text):
    """Name a command by its first two words only: arguments can be secrets
    (net connect <ssid> <password>)."""
    words = line.split()
    name = " ".join(words[:2])
    return name, (text.replace(line, name + " ...") if len(words) > 2 else text)


def _pulse_reset(ser):
    # USB Serial/JTAG: RTS pulses the chip's reset, DTR low keeps it out of
    # download mode. Held 200 ms, as esptool's USB hard reset does.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.2)
    ser.rts = False


class Tab5:
    def __init__(self, port=None, timeout=20.0):
        self.port = port or find_port()
        if not self.port:
            raise RuntimeError("no Tab5 found; pass --port")
        self.timeout = timeout
        self.ser = None
        self.boot_log = ""
        self.drops = 0
        self._rx = b""             # read past the last match

    def open(self, console=True):
        """Open the port and wait briefly for a prompt; doesn't fail if none
        comes (the board may still be booting): see connect(). console=False
        only opens it, for firmware without the console (test/device)."""
        self._open_raw()
        if console:
            self.ser.write(b"\n")
            self.read_until(PROMPT, 2)
        return self

    def _open_raw(self):
        if serial is None:
            raise RuntimeError("pyserial is missing: run with the ESP-IDF Python (. tools/env.sh)")
        self.ser = serial.Serial(self.port, 115200, timeout=0.1)

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except SERIAL_ERRORS:
                pass
            self.ser = None

    def __enter__(self):
        return self if self.ser else self.open()

    def __exit__(self, *exc):
        self.close()

    def send(self, line):
        """Write a command line without waiting for its reply. "\n" only: the
        console ends a line at "\r" or "\n", so "\r\n" would run an empty
        command after it, whose late prompt can be taken for this one's."""
        self.ser.write(line.encode() + b"\n")

    def flush_input(self):
        """Drop what the board has printed and not been read yet."""
        self.ser.reset_input_buffer()
        self._rx = b""

    def read_until(self, pattern, timeout, skip=0, reconnect=False):
        """Read until pattern (a str/bytes literal, or a compiled bytes regex)
        shows up past the first `skip` bytes. Returns (data up to the end of
        the match, match or None on timeout). What was read past the match
        is kept for the next read; cmd() drops it (log lines from other
        tasks may follow a prompt). With reconnect, a USB drop (a panic
        reboot) is waited out and counted in self.drops instead of raised."""
        if isinstance(pattern, str):
            pattern = pattern.encode()
        rx = pattern if hasattr(pattern, "search") else re.compile(re.escape(pattern))
        buf, self._rx = self._rx, b""
        end = time.time() + timeout
        while True:
            m = rx.search(buf, skip)
            if m:
                self._rx = buf[m.end():]
                return buf[:m.end()], m
            if time.time() >= end:
                return buf, None
            try:
                chunk = self.ser.read(4096)
            except SERIAL_ERRORS:
                if not reconnect:
                    raise
                self.drops += 1
                buf += f"\n[USB dropped at {time.strftime('%H:%M:%S')}]\n".encode()
                self.close()
                self._reopen(max(1.0, end - time.time()))
                continue
            buf += chunk

    def _reopen(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            p = wait_port(max(0.0, end - time.time()))
            if not p:
                break
            self.port = p
            try:
                self._open_raw()
                return
            except SERIAL_ERRORS:
                time.sleep(0.2)
        raise RuntimeError("the Tab5 did not come back on USB")

    def read_for(self, seconds):
        """Everything the board prints in the next seconds."""
        return self.read_until(re.compile(rb"(?!)"), seconds)[0]

    def read_until_gone(self, timeout):
        """Keep reading the output (so the board's console never blocks on a
        full buffer) until the port goes away, as at a power cut. True if it
        did within timeout; the port is closed either way."""
        end = time.time() + timeout
        try:
            while time.time() < end and os.path.exists(self.port):
                self.ser.read(4096)
        except SERIAL_ERRORS:
            pass
        self.close()
        return wait_port_gone(self.port, max(0.0, end - time.time()))

    def cmd(self, line, timeout=None):
        """Run one command; return its output without the echo and prompt."""
        self.flush_input()
        self.send(line)
        out, m = self.read_until(PROMPT, timeout or self.timeout, skip=len(line))
        if not m:
            name, tail = _redact(line, out.decode(errors="replace").replace("\r", ""))
            raise TimeoutError(f"no prompt after '{name} ...': {tail[-300:]}")
        return reply_text(out, line)

    def poll(self, line, pattern, timeout, every=0.5, cmd_timeout=10):
        """Run line until its reply matches pattern (a regex, re.M); the match,
        or None after timeout."""
        end = time.time() + timeout
        while True:
            m = re.search(pattern, self.cmd(line, cmd_timeout), re.M)
            if m or time.time() >= end:
                return m
            time.sleep(every)

    def uptime(self):
        """Seconds since boot, from `sys` (firmware/components/t5_diag/diag_sys.c)."""
        m = re.search(r"^uptime\s+([\d.]+)", self.cmd("sys", 10), re.M)
        if not m:
            raise RuntimeError("no uptime in `sys`")
        return float(m.group(1))

    def reset(self, console=True, boot_timeout=20):
        """Chip reset, then reopen once the board is back. Returns self, with
        what it printed up to the first prompt in self.boot_log. With
        console=False it returns as soon as the port is open again."""
        self._go_down("reset")
        self._reopen(30)
        if console:
            boot, _ = self.read_until(PROMPT, boot_timeout, reconnect=True)
            self.boot_log = boot.decode(errors="replace")
            self.ser.write(b"\n")
            self.read_until(PROMPT, 2)
        return self

    def _go_down(self, how):
        """Chip reset ("reset") or a console command that restarts the board;
        closes the port once the board has dropped it."""
        port = self.port
        if how == "reset":
            _pulse_reset(self.ser)
        else:
            try:
                self.send(how)
            except SERIAL_ERRORS:
                pass
        self.close()
        wait_port_gone(port, 3)

    def put(self, data, remote, progress=None):
        """Send data to the file remote on the device (`app put`)."""
        if not remote.startswith(PUT_DIRS) or ".." in remote:
            raise ValueError(f"{remote}: app put only writes under {' or '.join(PUT_DIRS)}")
        if not 0 < len(data) <= PUT_MAX:
            raise ValueError(f"app put takes 1 byte to {PUT_MAX // (1024 * 1024)} MB")
        self.flush_input()
        self.send(f"app put {remote} {len(data)}")
        failed = rb"put: [^\r\n]*\r?\n"
        _, m = self.read_until(re.compile(rb"READY|" + failed), 10)
        if not m:
            raise RuntimeError("the Tab5 didn't answer `app put` (update its firmware?)")
        if m.group(0).startswith(b"put:"):
            raise RuntimeError(m.group(0).decode(errors="replace").strip())
        acc = re.compile(rb"@ok|@re|" + failed)
        i, retries = 0, 0
        while i < len(data):
            part = data[i:i + PUT_CHUNK]
            self.ser.write(base64.b64encode(part) + b" %08x\n" % (zlib.crc32(part) & 0xFFFFFFFF))
            _, m = self.read_until(acc, 3)
            if not m:
                # The line's newline may have been dropped: end it; the
                # damaged line is refused ("@re") and sent again.
                self.ser.write(b"\n")
                _, m = self.read_until(acc, 3)
            if not m:
                raise RuntimeError("transfer stalled")
            if m.group(0).startswith(b"put:"):
                raise RuntimeError("transfer failed: " + m.group(0).decode(errors="replace").strip())
            if m.group(0) == b"@ok":
                i += PUT_CHUNK
                retries = 0
                if progress:
                    progress(min(i, len(data)), len(data))
            else:
                retries += 1
                if retries > 5:
                    raise RuntimeError(f"transfer keeps failing at byte {i}")
        _, m = self.read_until(re.compile(rb"OK (\d+) ([0-9a-f]{8})"), 5)
        if not m or int(m.group(2), 16) != (zlib.crc32(data) & 0xFFFFFFFF):
            raise RuntimeError("transfer check failed")
        self.read_until(PROMPT, 5)

    def install(self, data, name, progress=None):
        """Install the package data (a .t5app) over the console: `app put` to
        the temp folder, then `app install`. Raises RuntimeError if refused."""
        remote = f"{TMP_DIR}/{name}.t5app"
        self.put(data, remote, progress)
        out = self.cmd(f"app install {remote}", 60)
        if "OK" not in out:
            raise RuntimeError("install failed:\n" + out)
        return out

    def screenshot(self, div=2):
        """(rotation, w, h, rgb565) of the scanned-out framebuffer at 1/div size,
        in the panel's native portrait orientation."""
        return t5img.parse_console_dump(self.cmd(f"shot {div}", 90))

    def camera(self, w, h):
        """(0, w, h, rgb565) of the camera preview scaled to w x h."""
        _, w, h, data = t5img.parse_console_dump(self.cmd(f"cam snap {w} {h}", 180))
        return 0, w, h, data


def connect(port=None, timeout=60.0, cmd_timeout=20.0, booted_after=None):
    """An open Tab5 whose console answers, waiting up to timeout for the port
    to appear and the board to finish booting. With booted_after (a
    time.time() value), only a board that has restarted since then counts:
    the console of the instance being restarted doesn't."""
    end = time.time() + timeout
    why = "no USB port"
    while time.time() < end:
        p = port or wait_port(min(2.0, max(0.0, end - time.time())))
        if p:
            t = None
            try:
                t = Tab5(p, timeout=cmd_timeout).open()
                up = t.uptime()
                if booted_after is None or up <= time.time() - booted_after + 1.0:
                    return t
                why = f"still the instance from before the restart (up {up:.0f} s)"
            except (*SERIAL_ERRORS, TimeoutError, RuntimeError) as e:
                why = str(e)
            if t:
                t.close()
        time.sleep(0.5)
    raise RuntimeError(f"Tab5 not reachable: {why}")


def restart(t, how="reboot", timeout=120.0):
    """Restart the board (`reboot` on the console, or a chip "reset") and
    return a new connection to the restarted instance; t is closed."""
    t0 = time.time()
    t._go_down(how)
    return connect(timeout=timeout, booted_after=t0)
