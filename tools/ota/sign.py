# SPDX-License-Identifier: Apache-2.0
"""Signing helpers shared by the update and package tools (t5ota.py,
otakeys.py, tools/t5/t5pack.py, tools/t5/t5dev.py).

Keys are ECDSA P-256 PEM files; signing runs the openssl command. A key's
id is the SHA-256 of its public key's DER SubjectPublicKeyInfo: the device
knows OS update keys by it (t5_ota_hdr_t.key_id).

Stdlib only: otakeys.py runs at firmware build time.
"""
import hashlib
import os
import subprocess
import tempfile


class SignError(Exception):
    pass


def openssl(args, data=None):
    """Run openssl with args (stdin data), return its output, or raise SignError."""
    try:
        r = subprocess.run(["openssl"] + args, input=data, capture_output=True, check=False)
    except FileNotFoundError:
        raise SignError("signing needs the openssl command") from None
    if r.returncode != 0:
        raise SignError("openssl " + " ".join(args[:2]) + ": " + r.stderr.decode(errors="replace").strip())
    return r.stdout


def key_id(spki_der):
    return hashlib.sha256(spki_der).digest()


def public_der(key_path):
    """The public half of the key in key_path, as DER SubjectPublicKeyInfo."""
    return openssl(["pkey", "-in", key_path, "-pubout", "-outform", "DER"])


def generate(key_path):
    """A new private key in key_path (readable by its owner only)."""
    openssl(["ecparam", "-genkey", "-name", "prime256v1", "-noout", "-out", key_path])
    os.chmod(key_path, 0o600)


def sign_sha256(key_path, data):
    """ECDSA over SHA-256(data), DER-encoded."""
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
        name = t.name
    try:
        return openssl(["dgst", "-sha256", "-sign", key_path, name])
    finally:
        os.remove(name)


def der_to_raw(der):
    """A DER ECDSA signature (SEQUENCE { INTEGER r, INTEGER s }) as raw r||s, 64 bytes."""
    def integer(b, i):
        if b[i] != 0x02:
            raise SignError("not a DER ECDSA signature")
        n = b[i + 1]
        v = b[i + 2:i + 2 + n].lstrip(b"\0")
        return v.rjust(32, b"\0"), i + 2 + n
    i = 2 if der[1] < 0x80 else 3
    r, i = integer(der, i)
    s, _ = integer(der, i)
    return r + s
