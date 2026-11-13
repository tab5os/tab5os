#!/usr/bin/env python3
"""Make or inspect a Tab5 OS update (.t5os, plan 9.1) or C6 radio firmware (.t5c6).

  t5ota.py pack IMAGE.bin [-o OUT.t5os] [--sign KEY.pem] [--chip-rev X.Y]
  t5ota.py pack-c6 C6IMAGE.bin [-o OUT.t5c6] [--sign KEY.pem]
  t5ota.py inspect FILE.t5os|FILE.t5c6 [--chip-rev X.Y]

A .t5c6 is the same header with magic "T5C6" and an ESP32-C6 image built by
c6/ (version "t5-c6-<c6/VERSION>"); releases publish it beside the .t5os,
and the OS fetches the one it was built with (t5_ota_net/c6.h).

A .t5os is a header (t5_ota_hdr_t in firmware/components/t5_ota/include/
t5_ota/ota.h, whose constants are read from there) and the OS image. The
signature is ECDSA P-256 over the image's SHA-256, stored as raw r||s.
Signing uses the openssl command (sign.py). The OS installs images signed by
a key in firmware/ota_keys (put KEY's public half there with
`openssl pkey -in KEY.pem -pubout -out firmware/ota_keys/NAME.pem`); which
sources take other images is t5_ota_source_t in ota.h. --chip-rev checks the image's chip
revision range the way the device does before it installs.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "t5"))
import t5const  # noqa: E402
import sign  # noqa: E402

OTA_H = "t5_ota/include/t5_ota/ota.h"
IMG_H = "t5_bootfmt/include/t5_bootfmt/espimg.h"
C = t5const.defines(OTA_H, "T5_OTA_")
I = t5const.defines(IMG_H, "T5_IMG_")

# t5_ota_hdr_t: magic, version, sig_len, image_len, key_id, image_sha256, sig, os_version, reserved
HDR = struct.Struct("<4sHHI32s32s%ds16s4s" % C["T5_OTA_SIG_LEN"])
assert HDR.size == C["T5_OTA_HDR_SIZE"], "t5_ota_hdr_t changed: update HDR"
MAGIC = C["T5_OTA_MAGIC"].encode()
C6_MAGIC = C["T5_OTA_C6_MAGIC"].encode()
C6_PREFIX = C["T5_OTA_C6_PREFIX"]


def cstr(b):
    return b.split(b"\0")[0]


def image_info(image):
    """(version, project, chip_id, min_rev, max_rev) from an ESP-IDF app image."""
    d = I["T5_IMG_DESC_OFF"]
    if len(image) < d + I["T5_IMG_DESC_PROJECT_OFF"] + I["T5_IMG_DESC_PROJECT_LEN"] or image[0] != I["T5_IMG_MAGIC"]:
        return None
    magic, = struct.unpack_from("<I", image, d)
    if magic != I["T5_IMG_DESC_MAGIC"]:
        return None
    chip, = struct.unpack_from("<H", image, I["T5_IMG_CHIP_ID_OFF"])
    lo, = struct.unpack_from("<H", image, I["T5_IMG_MIN_REV_OFF"])
    hi, = struct.unpack_from("<H", image, I["T5_IMG_MAX_REV_OFF"])
    v = d + I["T5_IMG_DESC_VERSION_OFF"]
    p = d + I["T5_IMG_DESC_PROJECT_OFF"]
    version = cstr(image[v:v + I["T5_IMG_DESC_VERSION_LEN"]]).decode(errors="replace")
    project = cstr(image[p:p + I["T5_IMG_DESC_PROJECT_LEN"]]).decode(errors="replace")
    return version, project, chip, lo, hi


def rev_full(text):
    """'1.3' -> 103 (major * 100 + minor, as eFuse and the image header count)."""
    major, _, minor = text.partition(".")
    return int(major) * 100 + int(minor or 0)


def rev_text(full):
    return f"v{full // 100}.{full % 100}"


def check_image(image, chip_rev=None):
    """The device's checks on the image itself (t5_ota check_head). Returns
    its version, or exits with the reason it would refuse it."""
    info = image_info(image)
    if not info:
        sys.exit("not an ESP-IDF app image (tab5os.bin from firmware/build/)")
    version, project, chip, lo, hi = info
    if chip != I["T5_IMG_CHIP_ESP32P4"]:
        sys.exit(f"not an ESP32-P4 image (chip id {chip})")
    if project != C["T5_OTA_PROJECT"]:
        sys.exit(f"not a Tab5 OS image (project {project!r})")
    if chip_rev is not None and not lo <= rev_full(chip_rev) <= hi:
        sys.exit(f"built for chip revisions {rev_text(lo)}-{rev_text(hi)}: a v{chip_rev} device refuses it")
    return version, lo, hi


def check_c6_image(image):
    """The device's checks on a C6 image (t5_ota_c6_head_refusal). Returns
    the OS version it belongs to."""
    info = image_info(image)
    if not info:
        sys.exit("not an ESP-IDF app image (tab5os_c6.bin from c6/build/)")
    version, _project, chip, _lo, _hi = info
    if chip != I["T5_IMG_CHIP_ESP32C6"]:
        sys.exit(f"not an ESP32-C6 image (chip id {chip})")
    if not version.startswith(C6_PREFIX):
        sys.exit(f"not Tab5 OS radio firmware (version {version!r}, expected {C6_PREFIX}<version>)")
    return version


def header(magic, image, version, key):
    digest = hashlib.sha256(image).digest()
    kid, sig = b"\0" * 32, b"\0" * C["T5_OTA_SIG_LEN"]
    sig_len = 0
    if key:
        try:
            kid = sign.key_id(sign.public_der(key))
            sig = sign.der_to_raw(sign.sign_sha256(key, image))
        except sign.SignError as e:
            sys.exit(str(e))
        sig_len = C["T5_OTA_SIG_LEN"]
    hdr = HDR.pack(magic, C["T5_OTA_HDR_VERSION"], sig_len, len(image), kid, digest, sig, version.encode()[:16],
                   b"\0" * 4)
    return hdr, (f"signed by {kid.hex()[:16]}" if sig_len else "unsigned")


def pack(a):
    image = open(a.image, "rb").read()
    version, lo, hi = check_image(image, a.chip_rev)
    hdr, how = header(MAGIC, image, version, a.sign)
    out = a.output or f"tab5os-{version}.t5os"
    with open(out, "wb") as f:
        f.write(hdr + image)
    print(f"{out}: Tab5 OS {version} for chip {rev_text(lo)}-{rev_text(hi)}, {len(image)} bytes, {how}")


def pack_c6(a):
    image = open(a.image, "rb").read()
    version = check_c6_image(image)
    hdr, how = header(C6_MAGIC, image, version, a.sign)
    out = a.output or f"tab5os-c6-{version[len(C6_PREFIX):]}.t5c6"
    with open(out, "wb") as f:
        f.write(hdr + image)
    print(f"{out}: radio firmware {version}, {len(image)} bytes, {how}")


def inspect(a):
    data = open(a.file, "rb").read()
    if len(data) < HDR.size:
        sys.exit("not a .t5os file")
    magic, ver, sig_len, n, kid, digest, _sig, osv, _ = HDR.unpack_from(data)
    if magic not in (MAGIC, C6_MAGIC) or ver != C["T5_OTA_HDR_VERSION"]:
        sys.exit("not a .t5os or .t5c6 file")
    image = data[HDR.size:]
    ok = len(image) == n and hashlib.sha256(image).digest() == digest
    if magic == C6_MAGIC:
        what = f"radio firmware {check_c6_image(image)}"
    else:
        _, lo, hi = check_image(image, a.chip_rev)
        what = f"Tab5 OS {cstr(osv).decode()} for chip {rev_text(lo)}-{rev_text(hi)}"
    print(f"{a.file}: {what}, image {n} bytes, "
          f"hash {'ok' if ok else 'BAD'}, " + (f"signed by {kid.hex()}" if sig_len else "unsigned"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("image")
    p.add_argument("-o", "--output")
    p.add_argument("--sign", metavar="KEY")
    p.add_argument("--chip-rev", metavar="X.Y")
    p.set_defaults(fn=pack)
    p = sub.add_parser("pack-c6")
    p.add_argument("image")
    p.add_argument("-o", "--output")
    p.add_argument("--sign", metavar="KEY")
    p.set_defaults(fn=pack_c6)
    p = sub.add_parser("inspect")
    p.add_argument("file")
    p.add_argument("--chip-rev", metavar="X.Y")
    p.set_defaults(fn=inspect)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
