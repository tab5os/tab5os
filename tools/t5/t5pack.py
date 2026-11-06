#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build or inspect a Tab5 OS app package (.t5app).

  t5pack.py <app_dir> [-o out.t5app]   validate the manifest and write the zip
  t5pack.py --inspect file.t5app        list and validate a package

Stdlib only (Python 3.11+ for tomllib). The checks mirror the firmware's
manifest validator (firmware/components/t5_pkg/manifest.c), including the
TOML subset it understands, so a package that packs here installs there.
"""
import argparse
import os
import re
import sys
import tomllib
import zipfile

API_LEVEL = 1
NAME_MAX = 127           # zip entry names, bytes (T5_ZIP_NAME_MAX - 1)
MAX_PORTS = 8
MAX_HEAP_KB = 32 * 1024
TOP_KEYS = {"id", "name", "version", "api", "runtime", "entry", "icon", "min_heap_mb", "min_heap_kb",
            "orientation", "description", "author", "homepage", "license"}
PERM_BOOLS = {"camera", "mic", "net", "ble", "usb", "storage", "background_wake", "system"}
ID_RE = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
IDENT = r"[0-9A-Za-z-]+"
VERSION_RE = re.compile(rf"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-{IDENT}(\.{IDENT})*)?(\+{IDENT}(\.{IDENT})*)?$")
PORT_RE = re.compile(r"^[a-z0-9_]{1,15}$")
FUNC_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,30}$")
SKIP_NAMES = {".DS_Store", "Thumbs.db", "__pycache__", ".git", ".install"}


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
        if key in ("permissions", "background"):
            if not isinstance(value, dict):
                raise PackError(f"{key}: must be a table")
            for k, v in value.items():
                check_subset(v, f"{key}.{k}")
        elif key not in TOP_KEYS:
            raise PackError(f"unknown key '{key}'")
        else:
            check_subset(value, key)

    id_ = string(m, "id", True, 63)
    if not ID_RE.match(id_):
        raise PackError(f"id: '{id_}' is not reverse-DNS (lowercase a-z, 0-9, _; at least two parts)")
    string(m, "name", True, 47)
    version = string(m, "version", True, 15)
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
    if runtime not in ("native", "micropython"):
        raise PackError(f"runtime: unknown '{runtime}' (native or micropython)")
    for key in ("entry", "icon"):
        v = string(m, key, key == "entry", 63)
        if v is not None and (not name_ok(v) or v.endswith("/")):
            raise PackError(f"{key}: '{v}' is not a relative file path")
    has_mb = integer(m, "min_heap_mb", 0, MAX_HEAP_KB // 1024) is not None
    has_kb = integer(m, "min_heap_kb", 0, MAX_HEAP_KB) is not None
    if has_mb and has_kb:
        raise PackError("min_heap_mb and min_heap_kb are exclusive")
    orient = string(m, "orientation", False, 15)
    if orient is not None and orient not in ("any", "landscape", "portrait"):
        raise PackError(f"orientation: unknown '{orient}' (any, landscape or portrait)")

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
            if k not in ("entry", "max_seconds"):
                raise PackError(f"unknown key 'background.{k}'")
        entry = string(bg, "entry", False, 31)
        if entry is not None and not FUNC_RE.match(entry):
            raise PackError(f"background.entry: '{entry}' is not a function name")
        integer(bg, "max_seconds", 1, 300)
    return m


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


def pack(app_dir, out_path):
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
    tmp = out_path + ".tmp"
    total = len(raw)
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr(zipfile.ZipInfo("manifest.toml", date_time=(2026, 1, 1, 0, 0, 0)), raw,
                   compress_type=zipfile.ZIP_DEFLATED)
        for rel, full in files:
            z.write(full, rel)
            total += os.path.getsize(full)
    os.replace(tmp, out_path)
    print(f"{out_path}: {m['id']} {m['version']} ({m['runtime']}), {len(files) + 1} files, "
          f"{total} bytes -> {os.path.getsize(out_path)} bytes")
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
                    print(f"  background: entry={m['background'].get('entry', 'on_alarm')} "
                          f"max_seconds={m['background'].get('max_seconds', 30)}")
                if m["entry"] not in names:
                    problems.append(f"entry '{m['entry']}' is not in the package")
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
    args = ap.parse_args(argv)
    try:
        if args.inspect:
            return 0 if inspect(args.inspect) else 1
        if not args.app_dir:
            ap.error("app_dir or --inspect is required")
        pack(args.app_dir, args.output)
        return 0
    except PackError as e:
        print(f"t5pack: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
