#!/usr/bin/env python3
"""Bench 0.4: change each expander output and record what the board does.

Observables without extra equipment: which I2C addresses answer, and the
battery current through the INA226 (2.5 uV/LSB across the shunt; positive =
discharging). Every pin is put back afterwards and the board is
reset at the end so services start from the boot defaults.

    tools/t5/t5 hil expander_map
"""
import re
import time

import t5const
from t5link import connect

INA226 = t5const.define("t5_pins/include/t5_pins/pins.h", "T5_I2C_ADDR_INA226")
SHUNT_OHM = t5const.define("t5_battery/include/t5_battery/ina226.h", "T5_INA226_SHUNT_UOHM") / 1e6


def scan(t):
    out = t.cmd("i2c scan", 30)
    return sorted(int(a, 16) for a in re.findall(r"^\s+0x([0-9a-f]{2})\s", out, re.M))


def ina(t):
    """Average of 8 reads: (current mA, bus V)."""
    cur, bus = [], []
    for _ in range(8):
        s = t.cmd(f"i2c r {INA226:#x} 0x01 2")
        b = t.cmd(f"i2c r {INA226:#x} 0x02 2")
        sv = [int(x, 16) for x in re.findall(r": 0x([0-9a-f]{2})", s)]
        bv = [int(x, 16) for x in re.findall(r": 0x([0-9a-f]{2})", b)]
        raw = (sv[0] << 8) | sv[1]
        if raw & 0x8000:
            raw -= 0x10000
        cur.append(raw * 2.5e-6 / SHUNT_OHM * 1000)
        bus.append(((bv[0] << 8) | bv[1]) * 1.25e-3)
        time.sleep(0.05)
    return sum(cur) / len(cur), sum(bus) / len(bus)


def main():
    rows = []
    with connect(cmd_timeout=30) as t:
        t.cmd("touch dots off")
        base_scan = scan(t)
        base_i, base_v = ina(t)
        print(f"baseline: devices {[hex(a) for a in base_scan]}  battery {base_i:+.1f} mA at {base_v:.2f} V")

        def out(name, level):
            addr, pin = t5const.xpin("T5_XP_" + name)
            return f"exp out {addr:#x} {pin} {level}"

        def restore(name, level):
            if name == "LCD_RST":   # back to the boot state: an input, pulled up
                return "exp in %#x %d up" % t5const.xpin("T5_XP_LCD_RST")
            return out(name, 1 - level)

        # (label, set command, restore command, settle seconds)
        tests = [(f"{n}={v}{note}", out(n, v), restore(n, v), settle) for n, v, note, settle in (
            ("SPK_EN", 1, "", 0.5),
            ("EXT5V_EN", 1, "", 0.5),
            ("RF_ANT_SEL", 1, "", 0.3),
            ("CAM_RST", 1, " (release)", 0.5),
            ("WLAN_PWR_EN", 1, "", 2.0),
            ("USB5V_EN", 1, "", 0.5),
            ("NCHG_QC_EN", 1, " (QC off)", 1.5),
            ("CHG_EN", 0, " (charging off)", 1.5),
            ("TP_RST", 0, " (touch reset)", 0.5),
            ("LCD_RST", 0, " (lcd reset)", 0.5),
        )]
        for label, on, off, settle in tests:
            t.cmd(on)
            time.sleep(settle)
            s = scan(t)
            i, v = ina(t)
            stat = t.cmd("exp in %#x %d" % t5const.xpin("T5_XP_CHG_STAT"))
            t.cmd(off)
            time.sleep(0.6 if "RST" in label else 0.3)
            added = [hex(a) for a in s if a not in base_scan]
            gone = [hex(a) for a in base_scan if a not in s]
            chg = re.search(r"level (\d)", stat)
            rows.append((label, added, gone, i - base_i, v, chg.group(1) if chg else "?"))
            print(f"{label:26s} +{added} -{gone}  dI {i - base_i:+7.1f} mA  bus {v:.2f} V  CHG_STAT {rows[-1][5]}")
        t.reset()
    print("\n| Change | New I2C | Gone | Battery current change | CHG_STAT |")
    print("| --- | --- | --- | --- | --- |")
    for label, added, gone, di, v, chg in rows:
        print(f"| {label} | {' '.join(added) or '-'} | {' '.join(gone) or '-'} | {di:+.1f} mA | {chg} |")


if __name__ == "__main__":
    main()
