"""Host checks for `t5 new` (run by `t5 host-test`)."""
import argparse
import contextlib
import io
import os
import re
import sys
import tempfile
import tomllib
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import t5dev  # noqa: E402


def new(dest, name, python=False, app_id=None):
    a = argparse.Namespace(name=name, python=python, id=app_id, dir=dest)
    with contextlib.redirect_stdout(io.StringIO()):
        t5dev.cmd_new(a)


def read(dest, fn):
    with open(os.path.join(dest, fn), encoding="utf-8") as f:
        return f.read()


class NewApp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def check_filled(self, dest):
        for fn in os.listdir(dest):
            self.assertIsNone(re.search(r"@[A-Z0-9_]+@", read(dest, fn)), fn)

    def test_c_app(self):
        dest = os.path.join(self.tmp.name, "c")
        new(dest, "Air Log", app_id="com.you.airlog")
        self.check_filled(dest)
        m = tomllib.loads(read(dest, "manifest.toml"))
        self.assertEqual((m["id"], m["name"]), ("com.you.airlog", "Air Log"))
        cm = read(dest, "CMakeLists.txt")
        self.assertIn("project(air_log C)", cm)
        self.assertIn("t5_app(air_log ", cm)
        self.assertIn('set(T5_SDK "%s")' % t5dev.SDK, cm)
        self.assertIn('"Air Log"', read(dest, "main.c"))

    def test_python_app(self):
        dest = os.path.join(self.tmp.name, "py")
        new(dest, "Air Log", python=True)
        self.check_filled(dest)
        m = tomllib.loads(read(dest, "manifest.toml"))
        self.assertEqual((m["id"], m["name"], m["runtime"]), ("com.example.air_log", "Air Log", "micropython"))
        self.assertIn('t5.ui.screen("Air Log")', read(dest, "main.py"))

    def test_unknown_placeholder(self):
        tmpl = os.path.join(self.tmp.name, "tmpl")
        os.mkdir(tmpl)
        with open(os.path.join(tmpl, "main.c"), "w") as f:
            f.write('"@APP_NAME@" @APP_COLOUR@')
        dest = os.path.join(self.tmp.name, "out")
        with self.assertRaises(t5dev.CliError):
            t5dev.render_template(tmpl, dest, {"APP_NAME": "x"})
        self.assertFalse(os.path.exists(dest))

    def test_quote_in_name(self):
        with self.assertRaises(t5dev.CliError):
            new(os.path.join(self.tmp.name, "q"), 'Say "hi"')


if __name__ == "__main__":
    unittest.main()
