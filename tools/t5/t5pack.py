#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build or inspect a Tab5 OS app package (.t5app).

  t5pack.py <app_dir> [-o out.t5app] [--sign key.pem]
                                        validate the manifest and write the zip
  t5pack.py --inspect file.t5app        list and validate a package (and its signature)

Signed packages (plan 8.6) carry signature.t5sig: an ECDSA P-256 signature,
made with the openssl command, over a sorted list of every other entry's
SHA-256. Make a key with `t5 keygen`. The design allows unsigned apps (with a
warning at install); the signature names the publisher.

Stdlib only (Python 3.11+ for tomllib). The checks mirror the firmware's
manifest validator (firmware/components/t5_pkg/manifest.c), including the
TOML subset it understands, so a package that packs here installs there; the
limits, permission names and API level are read from the firmware sources
(t5const). A native app's imports are checked against sdk/abi/api_v1.sym, as
the installer does.
"""
import argparse
import base64
import hashlib
import os
import re
import sys
import struct
import tempfile
import tomllib
import zipfile

from t5const import MANIFEST_C, MANIFEST_H, define, permissions, runtimes, strings

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "abi"))
sys.path.insert(0, os.path.join(ROOT, "tools", "ota"))
import abi  # noqa: E402  (parse_sym)
import sign  # noqa: E402  (openssl, the key id)

ABI_SYM = os.path.join(ROOT, "sdk", "abi", "api_v1.sym")


def _m(name):
    return define(MANIFEST_H, name)


API_LEVEL = _m("T5_API_LEVEL")
NAME_MAX = define("t5_pkg/include/t5_pkg/zip.h", "T5_ZIP_NAME_MAX") - 1   # zip entry names, bytes
MAX_PORTS = _m("T5_MANIFEST_MAX_PORTS")
MAX_HEAP_KB = _m("T5_MANIFEST_MAX_HEAP_KB")
BG_MAX_SECONDS = _m("T5_MANIFEST_BG_MAX_SECONDS")
TOP_KEYS = set(strings(MANIFEST_C, "s_top_keys") + strings(MANIFEST_C, "s_info_keys"))
BG_KEYS = strings(MANIFEST_C, "s_bg_keys")
SHELL_KEYS = strings(MANIFEST_C, "s_shell_keys")
ORIENTATIONS = strings(MANIFEST_C, "s_orients")
ENTRY_SYMBOL = define("t5_app/include/t5_app/app.h", "T5_APP_ENTRY_SYMBOL")
PERM_BOOLS = {p.name for p in permissions() if p.name != "ports"}
RUNTIMES = runtimes()
ID_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
IDENT = r"[0-9A-Za-z-]+"
VERSION_RE = re.compile(rf"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-{IDENT}(\.{IDENT})*)?(\+{IDENT}(\.{IDENT})*)?$")
PORT_RE = re.compile(r"^[a-z0-9_.]{1,%d}$" % (_m("T5_MANIFEST_PORT_LEN") - 1))
FUNC_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,%d}$" % (_m("T5_MANIFEST_FUNC_LEN") - 2))
SKIP_NAMES = {".DS_Store", "Thumbs.db", "__pycache__", ".git", ".install", "build", "signature.t5sig"}
SIG_NAME = "signature.t5sig"


class PackError(Exception):
    pass


def name_ok(name):
    """Same rule as t5_zip_name_ok()."""
    if not name or len(name.encode()) > NAME_MAX:
        return False
    body = name[:-1] if name.endswith("/") else name
    if not body:
        return False
    for part in body.split("/"):
        if part in ("", ".", ".."):
            return False
        if any(ord(c) < 0x20 or c in "\\:\x7f" for c in part):
            return False
    return True


def check_subset(value, key):
    """Reject TOML the firmware's subset parser can't read."""
    if isinstance(value, bool) or isinstance(value, str):
        return
    if isinstance(value, int):
        if not -2**63 <= value < 2**63:
            raise PackError(f"{key}: integer out of range")
        return
    if isinstance(value, list):
        kinds = {type(v) for v in value}
        if kinds - {str, int} or len(kinds) > 1:
            raise PackError(f"{key}: arrays may hold only strings or only integers")
        for v in value:
            if isinstance(v, str) and "\x00" in v:
                raise PackError(f"{key}: NUL in string")
        return
    if isinstance(value, dict):
        raise PackError(f"{key}: inline tables and nested tables are not supported")
    raise PackError(f"{key}: {type(value).__name__} values are not supported (floats, dates)")


def string(m, key, required, maxlen):
    if key not in m:
        if required:
            raise PackError(f"{key}: missing")
        return None
    v = m[key]
    if not isinstance(v, str):
        raise PackError(f"{key}: must be a string")
    if not v:
        raise PackError(f"{key}: must not be empty")
    if len(v.encode()) > maxlen:
        raise PackError(f"{key}: longer than {maxlen} bytes")
    if any(ord(c) < 0x20 or c == "\x7f" for c in v):
        raise PackError(f"{key}: control characters")
    return v


def integer(m, key, lo, hi):
    if key not in m:
        return None
    v = m[key]
    if not isinstance(v, int) or isinstance(v, bool):
        raise PackError(f"{key}: must be an integer")
    if not lo <= v <= hi:
        raise PackError(f"{key}: {v} out of range {lo}..{hi}")
    return v


def validate(text):
    """Parse and validate manifest text; returns the dict. Raises PackError."""
    try:
        m = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise PackError(f"manifest.toml: {e}") from None
    if re.search(r'^\s*[A-Za-z0-9_.-]*\s*=\s*("""|\'\'\')', text, re.M):
        raise PackError("manifest.toml: multi-line strings are not supported on the device")
    if re.search(r'^\s*(\[\s*)?["\'][^"\']*["\']\s*[=.\]]', text, re.M):
        raise PackError("manifest.toml: quoted keys are not supported on the device")

    for key, value in m.items():
        if key in ("permissions", "background", "shell"):
            if not isinstance(value, dict):
                raise PackError(f"{key}: must be a table")
            for k, v in value.items():
                check_subset(v, f"{key}.{k}")
        elif key not in TOP_KEYS:
            raise PackError(f"unknown key '{key}'")
        else:
            check_subset(value, key)

    id_ = string(m, "id", True, _m("T5_APP_ID_LEN") - 1)
    if not ID_RE.match(id_):
        raise PackError(f"id: '{id_}' is not reverse-DNS (lowercase a-z, 0-9, _; at least two parts)")
    string(m, "name", True, _m("T5_APP_NAME_LEN") - 1)
    version = string(m, "version", True, _m("T5_APP_VERSION_LEN") - 1)
    if not VERSION_RE.match(version):
        raise PackError(f"version: '{version}' is not MAJOR.MINOR.PATCH")
    if "api" not in m:
        raise PackError("api: missing")
    api = integer(m, "api", -2**63, 2**63 - 1)
    if api < 1:
        raise PackError("api: must be at least 1")
    if api > API_LEVEL:
        raise PackError(f"api: app needs API level {api}, this firmware has {API_LEVEL}")
    runtime = string(m, "runtime", True, 15)
    if runtime not in RUNTIMES:
        raise PackError(f"runtime: unknown '{runtime}' ({' or '.join(RUNTIMES)})")
    for key in ("entry", "icon"):
        v = string(m, key, key == "entry", _m("T5_MANIFEST_FILE_LEN") - 1)
        if v is not None and (not name_ok(v) or v.endswith("/")):
            raise PackError(f"{key}: '{v}' is not a relative file path")
    has_mb = integer(m, "min_heap_mb", 0, MAX_HEAP_KB // 1024) is not None
    has_kb = integer(m, "min_heap_kb", 0, MAX_HEAP_KB) is not None
    if has_mb and has_kb:
        raise PackError("min_heap_mb and min_heap_kb are exclusive")
    orient = string(m, "orientation", False, 15)
    if orient is not None and orient not in ORIENTATIONS:
        raise PackError(f"orientation: unknown '{orient}' ({', '.join(ORIENTATIONS[:-1])} or {ORIENTATIONS[-1]})")

    perms = m.get("permissions", {})
    for k, v in perms.items():
        if k == "ports":
            if not isinstance(v, list) or not all(isinstance(p, str) for p in v):
                raise PackError("permissions.ports: must be an array of port names")
            if len(v) > MAX_PORTS:
                raise PackError(f"permissions.ports: at most {MAX_PORTS} ports")
            for p in v:
                if not PORT_RE.match(p):
                    raise PackError(f"permissions.ports: bad port name '{p}'")
            if len(set(v)) != len(v):
                raise PackError("permissions.ports: a port is listed twice")
        elif k in PERM_BOOLS:
            if not isinstance(v, bool):
                raise PackError(f"permissions.{k}: must be true or false")
        else:
            raise PackError(f"unknown key 'permissions.{k}'")

    bg = m.get("background")
    if bg is not None:
        if perms.get("background_wake") is not True:
            raise PackError("[background] needs background_wake = true in [permissions]")
        for k in bg:
            if k not in BG_KEYS:
                raise PackError(f"unknown key 'background.{k}'")
        entry = string(bg, "entry", False, _m("T5_MANIFEST_FUNC_LEN") - 1)
        if entry is not None and not FUNC_RE.match(entry):
            raise PackError(f"background.entry: '{entry}' is not a function name")
        integer(bg, "max_seconds", 1, BG_MAX_SECONDS)

    shell = m.get("shell", {})
    for k, v in shell.items():
        if k not in SHELL_KEYS:
            raise PackError(f"unknown key 'shell.{k}'")
        if k != "title" and not isinstance(v, bool):
            raise PackError(f"shell.{k}: must be true or false")
    string(shell, "title", False, _m("T5_APP_NAME_LEN") - 1)
    return m


def elf_imports(data):
    """(undefined non-weak symbols, defined functions) of an ELF32 app, as the
    installer reads them (t5_app/elfsym.c): .dynsym, then .symtab."""
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise PackError("entry is not a 32-bit little-endian ELF")
    shoff, = struct.unpack_from("<I", data, 32)
    shentsize, shnum = struct.unpack_from("<HH", data, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    undef, defined = set(), set()
    for want in (11, 2):    # SHT_DYNSYM, SHT_SYMTAB
        for _, typ, _, _, off, size, link, _, _, entsize in secs:
            if typ != want or entsize != 16:
                continue
            stroff = secs[link][4]
            for k in range(1, size // 16):
                st_name, _, _, info, _, shndx = struct.unpack_from("<IIIBBH", data, off + k * 16)
                end = data.index(b"\0", stroff + st_name)
                name = data[stroff + st_name:end].decode(errors="replace")
                bind = info >> 4
                if not name or bind == 0:     # STB_LOCAL
                    continue
                if shndx == 0:
                    if bind != 2:             # STB_WEAK imports may stay unresolved
                        undef.add(name)
                elif info & 0xF == 2:         # STT_FUNC
                    defined.add(name)
    return undef, defined


def exported_symbols(path=ABI_SYM):
    return {s.name for s in abi.parse_sym(path)[1]}


def check_imports(data, sym=ABI_SYM):
    """Refuse an app ELF the installer would refuse: imports the OS doesn't
    export, or no entry symbol (T5_APP)."""
    try:
        undef, defined = elf_imports(data)
    except (struct.error, ValueError, IndexError):
        raise PackError("entry is not a valid ELF") from None
    missing = sorted(undef - exported_symbols(sym))
    if missing:
        raise PackError("the app calls functions Tab5 OS doesn't export (sdk/abi/api_v1.sym): " + ", ".join(missing))
    if ENTRY_SYMBOL not in defined:
        raise PackError(f"the app has no {ENTRY_SYMBOL} (missing T5_APP?)")


def collect(app_dir, out_path):
    files = []
    out_real = os.path.realpath(out_path) if out_path else None
    for root, dirs, names in os.walk(app_dir):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_NAMES and not d.startswith("."))
        for n in sorted(names):
            if n in SKIP_NAMES or n.endswith(".t5app"):
                continue
            full = os.path.join(root, n)
            if os.path.islink(full) or not os.path.isfile(full) or os.path.realpath(full) == out_real:
                continue
            rel = os.path.relpath(full, app_dir).replace(os.sep, "/")
            if not name_ok(rel):
                raise PackError(f"file name not allowed in a package: {rel}")
            files.append((rel, full))
    return files


def digest_text(entries):
    """The text a package signature covers: one line per entry but the signature."""
    lines = ["t5app-digest 1"]
    for name, data in sorted(entries):
        if name != SIG_NAME:
            lines.append(f"{hashlib.sha256(data).hexdigest()} {name}")
    return ("\n".join(lines) + "\n").encode()


def openssl(args):
    try:
        return sign.openssl(args)
    except sign.SignError as e:
        raise PackError(str(e)) from None


def sign_entries(entries, key_path):
    try:
        pub = sign.public_der(key_path)
        sig = sign.sign_sha256(key_path, digest_text(entries))
    except sign.SignError as e:
        raise PackError(str(e)) from None
    fp = sign.key_id(pub).hex()
    text = (f"t5sig 1\nkey {fp}\npub {base64.b64encode(pub).decode()}\n"
            f"sig {base64.b64encode(sig).decode()}\n")
    return text.encode(), fp


def verify_entries(entries):
    """Returns the signer's key fingerprint, or raises."""
    sig_text = dict(entries).get(SIG_NAME)
    fields = dict(l.split(" ", 1) for l in sig_text.decode().splitlines() if " " in l)
    pub = base64.b64decode(fields["pub"])
    sig = base64.b64decode(fields["sig"])
    if sign.key_id(pub).hex() != fields.get("key"):
        raise PackError("signature: key fingerprint doesn't match the key")
    with tempfile.TemporaryDirectory() as d:
        kp, sp, dp = (os.path.join(d, n) for n in ("pub.der", "sig.der", "digest"))
        for path, data in ((kp, pub), (sp, sig), (dp, digest_text(entries))):
            with open(path, "wb") as f:
                f.write(data)
        openssl(["dgst", "-sha256", "-verify", kp, "-keyform", "DER", "-signature", sp, dp])
    return fields["key"]


def pack(app_dir, out_path, key_path=None):
    mpath = os.path.join(app_dir, "manifest.toml")
    if not os.path.isfile(mpath):
        raise PackError(f"{app_dir}: no manifest.toml")
    with open(mpath, "rb") as f:
        raw = f.read()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise PackError("manifest.toml: not UTF-8") from None
    m = validate(text)
    if out_path is None:
        out_path = f"{m['id']}-{m['version']}.t5app"
    files = [f for f in collect(app_dir, out_path) if f[0] != "manifest.toml"]
    names = {rel for rel, _ in files}
    if m["entry"] not in names:
        raise PackError(f"entry '{m['entry']}' not found in {app_dir}")
    if "icon" in m and m["icon"] not in names:
        raise PackError(f"icon '{m['icon']}' not found in {app_dir}")
    if m["runtime"] == "native":
        with open(dict(files)[m["entry"]], "rb") as f:
            check_imports(f.read())
    tmp = out_path + ".tmp"
    total = len(raw)
    entries = [("manifest.toml", raw)]
    for rel, full in files:
        with open(full, "rb") as f:
            entries.append((rel, f.read()))
    signer = None
    if key_path:
        sig, signer = sign_entries(entries, key_path)
        entries.append((SIG_NAME, sig))
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in entries:
            z.writestr(zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)), data,
                       compress_type=zipfile.ZIP_DEFLATED)
            total += len(data) if name != "manifest.toml" else 0
    os.replace(tmp, out_path)
    print(f"{out_path}: {m['id']} {m['version']} ({m['runtime']}), {len(entries)} files, "
          f"{total} bytes -> {os.path.getsize(out_path)} bytes" + (f", signed by {signer[:16]}" if signer else ", unsigned"))
    return out_path


def inspect(path):
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        raise PackError(f"{path}: {e}") from None
    problems = []
    with z:
        infos = z.infolist()
        print(f"{path}: {len(infos)} entries")
        for i in infos:
            method = {0: "stored", 8: "deflate"}.get(i.compress_type, f"method {i.compress_type}")
            print(f"  {i.file_size:>10}  {i.compress_size:>10}  {method:<8}  {i.filename}")
            if not name_ok(i.filename):
                problems.append(f"unsafe entry name: {i.filename!r}")
            if i.flag_bits & 0x41:
                problems.append(f"encrypted entry: {i.filename}")
            if i.compress_type not in (0, 8):
                problems.append(f"unsupported compression: {i.filename}")
            if i.file_size >= 0xFFFFFFFF or i.compress_size >= 0xFFFFFFFF or i.header_offset >= 0xFFFFFFFF:
                problems.append(f"zip64 entry: {i.filename}")
        if len(infos) >= 0xFFFF:
            problems.append("too many entries (zip64)")
        if len({i.filename for i in infos}) != len(infos):
            problems.append("duplicate entry names")
        bad = None
        try:
            bad = z.testzip()
        except Exception as e:  # noqa: BLE001 - report any decode failure
            problems.append(f"corrupt data: {e}")
        if bad:
            problems.append(f"CRC error in {bad}")
        names = {i.filename for i in infos}
        if SIG_NAME in names and not problems:
            try:
                fp = verify_entries([(i.filename, z.read(i.filename)) for i in infos])
                print(f"  signed by {fp}")
            except (PackError, KeyError, ValueError) as e:
                problems.append(f"bad signature: {e}")
        elif not problems:
            print("  unsigned")
        if "manifest.toml" not in names:
            problems.append("no manifest.toml at the root")
        else:
            try:
                m = validate(z.read("manifest.toml").decode("utf-8"))
                perms = m.get("permissions", {})
                granted = [k for k in sorted(PERM_BOOLS) if perms.get(k) is True]
                if perms.get("ports"):
                    granted.append("ports(" + ", ".join(perms["ports"]) + ")")
                print(f"  id={m['id']} name={m['name']!r} version={m['version']} api={m['api']} "
                      f"runtime={m['runtime']} entry={m['entry']}")
                print(f"  permissions: {', '.join(granted) or 'none'}")
                if "background" in m:
                    print(f"  background: entry={m['background'].get('entry', _m('T5_MANIFEST_BG_DEFAULT_ENTRY'))} "
                          f"max_seconds={m['background'].get('max_seconds', _m('T5_MANIFEST_BG_DEFAULT_SECONDS'))}")
                if m["entry"] not in names:
                    problems.append(f"entry '{m['entry']}' is not in the package")
                elif m["runtime"] == "native":
                    try:
                        check_imports(z.read(m["entry"]))
                    except PackError as e:
                        problems.append(str(e))
                if "icon" in m and m["icon"] not in names:
                    problems.append(f"icon '{m['icon']}' is not in the package")
            except (PackError, UnicodeDecodeError) as e:
                problems.append(str(e))
    for p in problems:
        print(f"  ERROR: {p}")
    print("  OK" if not problems else "  INVALID")
    return not problems


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build or inspect a Tab5 OS .t5app package")
    ap.add_argument("app_dir", nargs="?", help="app directory containing manifest.toml")
    ap.add_argument("-o", "--output", help="output .t5app (default <id>-<version>.t5app)")
    ap.add_argument("--inspect", metavar="FILE", help="list and validate a .t5app")
    ap.add_argument("--sign", metavar="KEY", help="sign with an ECDSA P-256 private key (PEM)")
    args = ap.parse_args(argv)
    try:
        if args.inspect:
            return 0 if inspect(args.inspect) else 1
        if not args.app_dir:
            ap.error("app_dir or --inspect is required")
        pack(args.app_dir, args.output, args.sign)
        return 0
    except PackError as e:
        print(f"t5pack: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
