#!/usr/bin/env python3
"""Check the rules that keep an OS update and its C6 radio firmware working
together (docs/releasing.md).

  release_rules.py [--since TAG]

The C6 firmware has its own version, c6/VERSION; a release only moves the
C6 when that changes. Against the previous release (TAG, by default the
newest v* tag before HEAD):

  1. c6/VERSION is a dotted version.
  2. If anything that goes into the C6 image changed (c6/, the esp_hosted
     release in either lock file, the IDF version), c6/VERSION was raised.
     It never goes down.
  3. The P4 and the C6 use the same esp_hosted release.
  4. If that release changed, c6/hosted-compat.txt has a line
     "<old> -> <new>: ..." saying how the update from the previous release
     was tried: after an OS update the new P4 has to reach the old C6
     firmware over esp_hosted, or it can't update the C6 at all.

Exit 0 when all hold; otherwise each broken rule is printed with what to do.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
C6_VERSION = "c6/VERSION"
COMPAT = "c6/hosted-compat.txt"
P4_LOCK = "firmware/dependencies.lock"
C6_LOCK = "c6/dependencies.lock"
# What goes into the C6 image besides c6/ itself.
C6_INPUTS = ("c6/", "firmware/idf_version.txt")
# Under c6/ but not part of the image.
C6_IGNORE = re.compile(r"^c6/(build/|managed_components/|sdkconfig$|README\.md$|hosted-compat\.txt$)")


def git(*args, check=True):
    r = subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True)
    if check and r.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout if r.returncode == 0 else None


def read(path, rev=None):
    """A file's text in the tree (rev None) or at a revision; None if absent."""
    if rev is None:
        p = os.path.join(ROOT, path)
        if not os.path.exists(p):
            return None
        with open(p) as f:
            return f.read()
    return git("show", f"{rev}:{path}", check=False)


def version_tuple(v):
    if not v or not re.fullmatch(r"\d+(\.\d+){0,3}", v):
        return None
    return tuple(int(x) for x in v.split("."))


def hosted_version(lock_text):
    """esp_hosted's version in an IDF dependencies.lock, or None."""
    if not lock_text:
        return None
    inside = False
    for line in lock_text.splitlines():
        if re.match(r"^  \S", line):                 # a component's entry starts
            inside = line.strip() == "espressif/esp_hosted:"
        elif inside:
            m = re.match(r"^    version: '?([^'\s]+)'?\s*$", line)
            if m:
                return m.group(1)
    return None


def previous_tag():
    t = git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*", "HEAD^", check=False)
    return t.strip() if t else None


def c6_inputs_changed(tag):
    names = git("diff", "--name-only", tag, "HEAD", "--", *C6_INPUTS).split()
    return [n for n in names if not C6_IGNORE.match(n)]


def check(tag):
    """The broken rules, as messages."""
    errs = []
    cur = (read(C6_VERSION) or "").strip()
    if not version_tuple(cur):
        errs.append(f"{C6_VERSION} must hold the C6 firmware's version (like 0.2.0), not {cur!r}")
        return errs
    p4_h, c6_h = hosted_version(read(P4_LOCK)), hosted_version(read(C6_LOCK))
    if not p4_h or not c6_h:
        errs.append(f"esp_hosted not found in {P4_LOCK} or {C6_LOCK}: build both (t5 build, t5 c6-build)")
    elif p4_h != c6_h:
        errs.append(f"the P4 uses esp_hosted {p4_h} and the C6 {c6_h}: use one release on both sides "
                    f"(firmware/components/t5_net/idf_component.yml, c6/main/idf_component.yml)")
    if not tag:
        return errs

    old = (read(C6_VERSION, tag) or "").strip()
    changed = c6_inputs_changed(tag)
    if version_tuple(old):
        if version_tuple(cur) < version_tuple(old):
            errs.append(f"{C6_VERSION} went down from {old} (in {tag}) to {cur}")
        elif changed and cur == old:
            shown = ", ".join(changed[:5]) + (" ..." if len(changed) > 5 else "")
            errs.append(f"the C6 firmware changed since {tag} ({shown}) but {C6_VERSION} is still {old}: "
                        f"raise it, or devices keep the old C6 firmware")

    old_h = hosted_version(read(P4_LOCK, tag))
    if old_h and p4_h and old_h != p4_h:
        lines = (read(COMPAT) or "").splitlines()
        want = f"{old_h} -> {p4_h}"
        if not any(line.strip().startswith(want) for line in lines):
            errs.append(f"esp_hosted moved from {old_h} (in {tag}) to {p4_h}. Install this build as an update "
                        f"over {tag} on a Tab5 and check that it updates the C6 and Wi-Fi comes back, then add "
                        f"'{want}: <how it was tried>' to {COMPAT}")
    return errs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--since", metavar="TAG", help="the previous release (default: the newest v* tag before HEAD)")
    a = ap.parse_args()
    tag = a.since or previous_tag()
    cur = (read(C6_VERSION) or "?").strip()
    if tag:
        old = (read(C6_VERSION, tag) or "").strip() or "none"
        print(f"C6 firmware {cur} (was {old} in {tag}); esp_hosted {hosted_version(read(P4_LOCK))}")
    else:
        print(f"C6 firmware {cur}; no earlier release to compare with")
    errs = check(tag)
    for e in errs:
        print(f"error: {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
