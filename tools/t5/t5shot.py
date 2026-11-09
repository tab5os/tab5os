#!/usr/bin/env python3
"""Fetch a screenshot (or a camera snapshot) from a Tab5 over the console.

usage: t5shot.py [--port PORT] [--div N] [--cam] [--rot DEG] OUT.png

`shot N` dumps the scanned-out framebuffer at 1/N size; it is written turned
to the logical rotation the device reports. `--cam` uses `cam snap` (the
camera preview) instead. Run with the ESP-IDF Python (pyserial); `t5 shot`
does.
"""
import argparse

import t5img
from t5const import define
from t5link import Tab5

CAMERA_H = "t5_camera/include/t5_camera/camera.h"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--div", type=int, default=None,
                    help="scale down by N (screen: default 2; --cam: the camera's video size / N, default 4)")
    ap.add_argument("--cam", action="store_true")
    ap.add_argument("--rot", type=int, choices=(0, 90, 180, 270), help="override the rotation")
    ap.add_argument("out")
    a = ap.parse_args()
    try:
        with Tab5(a.port, timeout=60) as t:
            if a.cam:
                div = a.div or 4
                rot, w, h, data = t.camera(define(CAMERA_H, "T5_CAMERA_W") // div,
                                           define(CAMERA_H, "T5_CAMERA_H") // div)
            else:
                rot, w, h, data = t.screenshot(a.div or 2)
    except (RuntimeError, ValueError, TimeoutError) as e:
        raise SystemExit(str(e))
    if a.rot is not None:
        rot = a.rot
    ow, oh = t5img.save(a.out, data, w, h, rot)
    print("%s: %dx%d (rotation %d)" % (a.out, ow, oh, rot))


if __name__ == "__main__":
    main()
