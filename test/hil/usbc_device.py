#!/usr/bin/env python3
"""USB-C device mode (t5_usb/usbdev.h) against this computer, without
touching its keyboard or pointer.

The Tab5's `usbc device S` self-test enumerates as a composite device: a
vendor-defined HID interface that echoes 8-byte output reports back as
input reports, a mouse (never moves) and USB MIDI that echoes every
message. The console is gone meanwhile, so this:

1. starts the self-test and lets go of the serial port;
2. finds the device (Espressif VID, PID 0x400C: HID + MIDI) and checks it
   has the mouse interface too;
3. round-trips N random HID reports and N MIDI notes, timing them;
4. waits for the console to come back (the test ends itself) and reads the
   device's own counts.

Needs hidapi and python-rtmidi besides pyserial, so it isn't run with the
IDF python:

    python3 -m venv /tmp/t5io-venv
    /tmp/t5io-venv/bin/pip install hidapi python-rtmidi pyserial
    PYTHONPATH=tools/t5 /tmp/t5io-venv/bin/python test/hil/usbc_device.py
"""
import argparse
import os
import re
import sys
import time

try:
    import hid
    import rtmidi
except ImportError:
    sys.exit("needs hidapi and python-rtmidi (see the docstring)")

from t5link import connect

VID, PID = 0x303A, 0x400C


def find_device(timeout):
    end = time.time() + timeout
    while time.time() < end:
        devs = [d for d in hid.enumerate(VID, PID)]
        if any(d["usage_page"] == 0xFF00 for d in devs):
            return devs
        time.sleep(0.25)
    return []


def midi_port(cls, name, timeout):
    end = time.time() + timeout
    while time.time() < end:
        m = cls()
        for i, p in enumerate(m.get_ports()):
            if name in p:
                m.open_port(i)
                return m
        del m
        time.sleep(0.25)
    return None


def hid_echo(devs, n):
    path = next(d["path"] for d in devs if d["usage_page"] == 0xFF00)
    h = hid.device()
    h.open_path(path)
    ok, worst = 0, 0.0
    try:
        for i in range(n):
            data = list(os.urandom(8))
            t0 = time.time()
            h.write([0] + data)                 # report ID 0, then the 8 bytes
            got = h.read(8, 1000)
            dt = time.time() - t0
            worst = max(worst, dt)
            if got == data:
                ok += 1
            else:
                print(f"  hid {i}: sent {data} got {got}")
    finally:
        h.close()
    return ok, worst


def midi_echo(n):
    out = midi_port(rtmidi.MidiOut, "Tab5", 5)
    inp = midi_port(rtmidi.MidiIn, "Tab5", 5)
    if not out or not inp:
        print("  no Tab5 MIDI port")
        return 0, 0.0
    ok, worst = 0, 0.0
    for i in range(n):
        msg = [0x90, 36 + i % 48, 1 + i % 126]
        t0 = time.time()
        out.send_message(msg)
        got = None
        while time.time() - t0 < 1.0:
            m = inp.get_message()
            if m:
                got = m[0]
                break
            time.sleep(0.001)
        worst = max(worst, time.time() - t0)
        if got == msg:
            ok += 1
        else:
            print(f"  midi {i}: sent {msg} got {got}")
    out.send_message([0x80, 36, 0])
    inp.close_port()
    out.close_port()
    return ok, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=int, default=25, help="how long the device stays in device mode")
    ap.add_argument("-n", type=int, default=50, help="round trips per class")
    a = ap.parse_args()
    with connect(cmd_timeout=10) as t:
        r = t.cmd(f"usbc device {a.secs}")
        if "self-test" not in r:
            print("the self-test didn't start:\n" + r)
            return 1
    t_start = time.time()
    ok = True

    devs = find_device(10)
    if not devs:
        print("FAIL: the device didn't show up")
        ok = False
    else:
        print(f"enumerated after {time.time() - t_start:.1f} s: {devs[0]['product_string']!r}, "
              f"{len(devs)} HID collections")
        if not any(d["usage_page"] == 1 and d["usage"] == 2 for d in devs):
            print("FAIL: no mouse collection")
            ok = False
        good, worst = hid_echo(devs, a.n)
        print(f"hid echo {good}/{a.n}, worst {worst * 1000:.1f} ms")
        ok &= good == a.n
        good, worst = midi_echo(a.n)
        print(f"midi echo {good}/{a.n}, worst {worst * 1000:.1f} ms")
        ok &= good == a.n

    # The test ends itself; then the console is back.
    time.sleep(max(0, a.secs - (time.time() - t_start)) + 2)
    gone = not find_device(0.5)
    print("device gone after the test" if gone else "FAIL: the device is still there")
    ok &= gone
    end = time.time() + 20
    status = None
    while time.time() < end and status is None:
        try:
            with connect(cmd_timeout=10) as t:
                status = t.cmd("usbc device status")
                trace = t.cmd("usbc trace")
        except Exception:
            time.sleep(1)
    if status is None:
        print("FAIL: the console didn't come back")
        return 1
    print("console back: " + status.strip().splitlines()[-1])
    print(trace.strip().splitlines()[-1])
    m = re.search(r"open (\S+), mounted (\d+).*hid echo (\d+), midi echo (\d+), send errors (\d+)", status)
    if not m or m.group(1) != "OK" or m.group(5) != "0":
        print("FAIL: the device's counts")
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
