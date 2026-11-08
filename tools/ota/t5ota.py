#!/usr/bin/env python3
"""Make or inspect a Tab5 OS update (.t5os, plan 9.1).

  t5ota.py pack IMAGE.bin [-o OUT.t5os] [--sign KEY.pem]
  t5ota.py inspect FILE.t5os

A .t5os is a 160-byte header (firmware/components/t5_ota/include/t5_ota/ota.h)
and the OS image. The signature is ECDSA P-256 over the image's SHA-256,
stored as raw r||s. Signing uses the openssl command. The OS installs images
signed by a key in firmware/ota_keys (put KEY's public half there with
`openssl pkey -in KEY.pem -pubout -out firmware/ota_keys/NAME.pem`), and
unsigned ones only in developer mode.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

HDR = struct.Struct("<4sHHI32s32s64s16s4s")
assert HDR.size == 160
ESP_APP_DESC_MAGIC = 0xABCD5432


def app_version(image):
    # esp_image_header_t (24) + first segment header (8), then esp_app_desc_t.
    magic, = struct.unpack_from("<I", image, 32)
    if image[0] != 0xE9 or magic != ESP_APP_DESC_MAGIC:
        sys.exit("not an ESP-IDF app image (tab5os.bin from firmware/build-*/)")
    version = image[48:80].split(b"\0")[0]
    project = image[80:112].split(b"\0")[0]
    if project != b"tab5os":
        sys.exit(f"not a Tab5 OS image (project {project.decode(errors='replace')!r})")
    return version.decode()


def der_to_raw(der):
    # SEQUENCE { INTEGER r, INTEGER s }
    def integer(b, i):
        assert b[i] == 0x02
        n = b[i + 1]
        v = b[i + 2:i + 2 + n].lstrip(b"\0")
        return v.rjust(32, b"\0"), i + 2 + n
    i = 2 if der[1] < 0x80 else 3
    r, i = integer(der, i)
    s, _ = integer(der, i)
    return r + s


def pack(a):
    image = open(a.image, "rb").read()
    version = app_version(image)
    digest = hashlib.sha256(image).digest()
    key_id, sig = b"\0" * 32, b"\0" * 64
    sig_len = 0
    if a.sign:
        pub = subprocess.run(["openssl", "pkey", "-in", a.sign, "-pubout", "-outform", "DER"], capture_output=True,
                             check=True).stdout
        key_id = hashlib.sha256(pub).digest()
        with tempfile.NamedTemporaryFile(delete=False) as t:
            t.write(image)
            name = t.name
        try:
            der = subprocess.run(["openssl", "dgst", "-sha256", "-sign", a.sign, name], capture_output=True,
                                 check=True).stdout
        finally:
            os.remove(name)
        sig = der_to_raw(der)
        sig_len = 64
    hdr = HDR.pack(b"T5OS", 1, sig_len, len(image), key_id, digest, sig, version.encode()[:16], b"\0" * 4)
    out = a.output or f"tab5os-{version}.t5os"
    with open(out, "wb") as f:
        f.write(hdr + image)
    print(f"{out}: Tab5 OS {version}, {len(image)} bytes, " +
          (f"signed by {key_id.hex()[:16]}" if sig_len else "unsigned (installs only in developer mode)"))


def inspect(a):
    data = open(a.file, "rb").read()
    magic, ver, sig_len, n, key_id, digest, sig, osv, _ = HDR.unpack_from(data)
    if magic != b"T5OS":
        sys.exit("not a .t5os file")
    image = data[160:]
    ok = len(image) == n and hashlib.sha256(image).digest() == digest
    version = osv.split(b"\0")[0].decode()
    print(f"{a.file}: Tab5 OS {version}, image {n} bytes, hash {'ok' if ok else 'BAD'}, " +
          (f"signed by {key_id.hex()}" if sig_len else "unsigned"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("image")
    p.add_argument("-o", "--output")
    p.add_argument("--sign", metavar="KEY")
    p.set_defaults(fn=pack)
    p = sub.add_parser("inspect")
    p.add_argument("file")
    p.set_defaults(fn=inspect)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
