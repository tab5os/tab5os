"""Device facts for the host tools, read from the firmware sources.

The firmware's headers and partitions.csv are the only definition of these
values; the tools read them here instead of keeping their own copies.

    from t5const import define, partition
    define("t5_dev/include/t5_dev/dev.h", "T5_DEV_PORT")   # -> 8350
    partition("recovery")                                  # -> (0x50000, 0x180000)
    permissions()                                          # -> manifest permission groups
    strings("t5_pkg/manifest.c", "s_orients")              # -> ("any", "landscape", "portrait")
    xpin("T5_XP_SPK_EN")                                   # -> (0x43, 1)

Command line (for shell scripts):
    t5const.py define <header> <NAME>
    t5const.py partition <name> offset|size
    t5const.py flash-args <build dir>     esptool write-flash arguments, file names without directories
    t5const.py snapshot OUT.json          the facts the SDK's tools use, for the SDK release

The SDK release has no firmware sources: there the same calls answer from
sdk/device.json, which `snapshot` writes by loading the tools the SDK ships
and recording what they read (tools/release.sh does this).
"""
import functools
import importlib
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COMPONENTS = os.path.join(ROOT, "firmware", "components")
PARTITIONS = os.path.join(ROOT, "firmware", "partitions.csv")
SNAPSHOT = os.path.join(ROOT, "sdk", "device.json")
# The SDK's tools, which read their constants when they are loaded.
SDK_TOOLS = ("t5pack", "t5dev", "t5link", "t5shot", "t5ota", "abi", "sign")
_recording = None          # {key: value} while `snapshot` runs


@functools.lru_cache(maxsize=None)
def _snapshot():
    try:
        with open(SNAPSHOT) as f:
            return json.load(f)
    except OSError:
        raise KeyError(f"no firmware sources ({COMPONENTS}) and no {SNAPSHOT}") from None


def _fact(fn):
    """From the firmware sources when they're here, else from the snapshot.
    The snapshot keeps whole files, not single answers: every constant of a
    header that was read (define, defines), every string table of a source
    (strings), so a tool that reads another name of the same file later
    still finds it."""
    @functools.wraps(fn)
    def wrapper(*args):
        name = fn.__name__
        if name in ("define", "defines"):
            key, pick = json.dumps(["header", args[0], *args[2:]]), (lambda t: _pick(name, t, args[1]))
        elif name == "strings":
            key, pick = json.dumps(["strings", args[0]]), (lambda t: tuple(t[args[1]]))
        else:
            key, pick = json.dumps([name, *args]), (lambda v: _decode(name, v))
        if not os.path.isdir(COMPONENTS):
            facts = _snapshot()
            try:
                return pick(facts[key])
            except KeyError:
                raise KeyError(f"{name}{args} isn't in {SNAPSHOT}: rebuild the SDK release") from None
        v = fn(*args)
        if _recording is not None and key not in _recording:
            _recording[key] = _whole(name, args) if name in ("define", "defines", "strings") else _encode(v)
        return v
    return wrapper


def _pick(name, table, arg):
    if name == "define":
        return table[arg]
    return {k: v for k, v in table.items() if k.startswith(arg)}


def _whole(name, args):
    """Everything in the file a define/defines/strings call read."""
    if name == "strings":
        with open(os.path.join(COMPONENTS, args[0])) as f:
            text = _strip_comments(f.read())
        out = {}
        for m in re.finditer(r"\b(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S):
            lits = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2))
            if lits:
                out[m.group(1)] = lits
        return out
    t = _table(args[0], *args[2:])
    out = {}
    for k in _table(args[0]):
        try:
            out[k] = _resolve(k, t)
        except (KeyError, SyntaxError, NameError, TypeError, ZeroDivisionError):
            pass                  # not a plain constant (a cast, a type...)
    return out


def _encode(v):
    if isinstance(v, tuple) and v and isinstance(v[0], Perm):
        return [p.__dict__ for p in v]
    return v


def _decode(name, v):
    if name == "permissions":
        return tuple(Perm(**p) for p in v)
    if name in ("strings", "runtimes"):
        return tuple(v)
    if name == "xpins":
        return {k: tuple(x) for k, x in v.items()}
    if name == "partitions":
        return {k: tuple(x) for k, x in v.items()}
    return v

_DEFINE = re.compile(r"^\s*#\s*define\s+(\w+)\s+(.+?)\s*$")
_ENUM = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*([^,]+?)\s*,?\s*$")


def _strip_comments(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def _value(raw, table):
    raw = raw.strip()
    if '"' in raw:
        # Adjacent string literals and macros: "a" NAME "b"
        out = ""
        for tok in re.findall(r'"(?:[^"\\]|\\.)*"|\w+', raw):
            out += tok[1:-1] if tok.startswith('"') else str(_resolve(tok, table))
        return out
    expr = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", raw)
    expr = re.sub(r"\b(0[xX][0-9a-fA-F]+)[uUlL]+\b", r"\1", expr)
    expr = re.sub(r"\b[A-Za-z_]\w*", lambda m: str(_resolve(m.group(0), table)), expr)
    return eval(expr, {"__builtins__": {}})  # integer arithmetic only


def _resolve(name, table):
    if name not in table:
        raise KeyError(f"{name} is not defined in the headers read")
    v = table[name]
    if isinstance(v, tuple):  # not evaluated yet
        table[name] = v = _value(v[0], table)
    return v


@functools.lru_cache(maxsize=None)
def _table(*headers):
    table = {}
    for h in headers:
        path = h if os.path.isabs(h) else os.path.join(COMPONENTS, h)
        with open(path) as f:
            text = _strip_comments(f.read())
        for line in text.splitlines():
            m = _DEFINE.match(line) or _ENUM.match(line)
            if m and "(" not in m.group(1):
                table[m.group(1)] = (m.group(2),)
    return table


@_fact
def define(header, name, *more_headers):
    """The value of #define NAME (or an enum member with an explicit value)
    in header: an int or a str. Macros it uses may come from more_headers
    (paths under firmware/components)."""
    return _resolve(name, _table(header, *more_headers))


@_fact
def defines(header, prefix, *more_headers):
    """Every constant in header whose name starts with prefix, evaluated."""
    t = _table(header, *more_headers)
    own = _table(header)
    return {k: _resolve(k, t) for k in own if k.startswith(prefix)}


PINS_H = "t5_pins/include/t5_pins/pins.h"
_XPIN = re.compile(r"^\s*#\s*define\s+(T5_XP_\w+)\s+T5_XPIN\(\s*(\w+)\s*,\s*(\w+)\s*\)", re.M)


@functools.lru_cache(maxsize=None)
@_fact
def xpins():
    """{T5_XP_*: (expander address, pin)} from t5_pins/pins.h."""
    with open(os.path.join(COMPONENTS, PINS_H)) as f:
        text = _strip_comments(f.read())
    table = _table(PINS_H)
    return {m.group(1): (_value(m.group(2), table), _value(m.group(3), table)) for m in _XPIN.finditer(text)}


def xpin(name):
    return xpins()[name]


MANIFEST_H = "t5_pkg/include/t5_pkg/manifest.h"
_PERM_ROW = re.compile(r'\{\s*(T5_PERM_\w+)\s*,\s*"(\w+)"\s*,\s*(true|false)\s*\}')


class Perm:
    def __init__(self, name, macro, bit, prompted):
        self.name, self.macro, self.bit, self.prompted = name, macro, bit, prompted


@functools.lru_cache(maxsize=None)
@_fact
def permissions():
    """The manifest permission groups, in the firmware's order: the table in
    t5_pkg/manifest.c (name, bit, asked on first use), bits from manifest.h."""
    with open(os.path.join(COMPONENTS, "t5_pkg", "manifest.c")) as f:
        rows = _PERM_ROW.findall(_strip_comments(f.read()))
    if not rows:
        raise KeyError("no permission table in t5_pkg/manifest.c")
    return tuple(Perm(name, macro, define(MANIFEST_H, macro), flag == "true") for macro, name, flag in rows)


@functools.lru_cache(maxsize=None)
@_fact
def strings(source, name):
    """The string literals of the table `name[] = { ... };` in source (a path
    under firmware/components), in order; designated initializers are taken
    in the order written."""
    with open(os.path.join(COMPONENTS, source)) as f:
        text = _strip_comments(f.read())
    m = re.search(r"\b%s\s*\[[^\]]*\]\s*=\s*\{(.*?)\};" % re.escape(name), text, re.S)
    if not m:
        raise KeyError(f"no table {name} in {source}")
    return tuple(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


MANIFEST_C = "t5_pkg/manifest.c"


@_fact
def runtimes():
    """The manifest runtime names (the table in t5_pkg/manifest.c)."""
    return strings(MANIFEST_C, "s_runtimes")


@functools.lru_cache(maxsize=None)
@_fact
def partitions():
    """{name: (offset, size)} from firmware/partitions.csv."""
    out = {}
    with open(PARTITIONS) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            out[cols[0]] = (int(cols[3], 0), int(cols[4], 0))
    return out


def partition(name):
    return partitions()[name]


def flash_args(build):
    """The firmware build's esptool write-flash arguments (flasher_args.json,
    written by the IDF build): flash settings, then offset/file pairs, files
    by base name."""
    with open(os.path.join(build, "flasher_args.json")) as f:
        fa = json.load(f)
    out = list(fa["write_flash_args"])
    for off, path in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 0)):
        out += [off, os.path.basename(path)]
    return out


def snapshot(out):
    """Load the SDK's tools with recording on and write what they read."""
    global _recording
    _recording = {}
    for d in ("t5", "abi", "ota"):
        sys.path.insert(0, os.path.join(ROOT, "tools", d))
    for m in SDK_TOOLS:
        importlib.import_module(m)
    facts, _recording = _recording, None
    with open(out, "w") as f:
        json.dump(facts, f, indent=1, sort_keys=True)
        f.write("\n")
    print(f"{out}: {len(facts)} facts")


def main(argv):
    if len(argv) == 3 and argv[0] == "define":
        print(define(argv[1], argv[2]))
    elif len(argv) == 3 and argv[0] == "partition":
        off, size = partition(argv[1])
        print(hex(off if argv[2] == "offset" else size))
    elif len(argv) == 2 and argv[0] == "flash-args":
        print(" ".join(flash_args(argv[1])))
    elif len(argv) == 2 and argv[0] == "snapshot":
        # The tools import this file as t5const, not __main__: record there.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        importlib.import_module("t5const").snapshot(argv[1])
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
