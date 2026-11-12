"""release_rules.py against a throwaway git repository."""
import os
import subprocess
import tempfile
import unittest

import release_rules as rr


def lock(hosted):
    return ("dependencies:\n"
            "  espressif/cjson:\n    version: 1.0.0\n"
            "  espressif/esp_hosted:\n    component_hash: abc\n    dependencies:\n"
            "    - name: x\n      version: '>=1.6.0'\n"
            f"    source:\n      type: service\n    version: {hosted}\n"
            "  espressif/zz:\n    version: 9.9.9\n")


class ReleaseRules(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.old_root = rr.ROOT
        rr.ROOT = self.root
        self.git("init", "-q")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.write("c6/VERSION", "0.2.0\n")
        self.write("c6/main/main.c", "int x;\n")
        self.write("c6/README.md", "readme\n")
        self.write("firmware/idf_version.txt", "v6.1\n")
        self.write(rr.P4_LOCK, lock("3.1.0"))
        self.write(rr.C6_LOCK, lock("3.1.0"))
        self.commit("release")
        self.git("tag", "v0.2.0")

    def tearDown(self):
        rr.ROOT = self.old_root
        self.tmp.cleanup()

    def git(self, *args):
        subprocess.run(["git", "-C", self.root, *args], check=True, capture_output=True)

    def write(self, path, text):
        p = os.path.join(self.root, path)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)

    def commit(self, msg="change"):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", msg)

    def errs(self):
        return rr.check(rr.previous_tag())

    def test_lock_parsing(self):
        self.assertEqual("3.1.0", rr.hosted_version(lock("3.1.0")))
        self.assertIsNone(rr.hosted_version("dependencies:\n  espressif/cjson:\n    version: 1.0.0\n"))

    def test_nothing_changed(self):
        self.write("README.md", "os only\n")
        self.commit()
        self.assertEqual([], self.errs())

    def test_c6_change_needs_a_bump(self):
        self.write("c6/main/main.c", "int y;\n")
        self.commit()
        self.assertIn("still 0.2.0", " ".join(self.errs()))
        self.write("c6/VERSION", "0.2.1\n")
        self.commit()
        self.assertEqual([], self.errs())

    def test_idf_change_needs_a_bump(self):
        self.write("firmware/idf_version.txt", "v6.2\n")
        self.commit()
        self.assertTrue(self.errs())

    def test_docs_under_c6_dont(self):
        self.write("c6/README.md", "more\n")
        self.commit()
        self.assertEqual([], self.errs())

    def test_version_never_goes_down(self):
        self.write("c6/VERSION", "0.1.9\n")
        self.commit()
        self.assertIn("went down", " ".join(self.errs()))

    def test_bad_version(self):
        self.write("c6/VERSION", "next\n")
        self.commit()
        self.assertTrue(self.errs())

    def test_hosted_must_match(self):
        self.write(rr.C6_LOCK, lock("3.2.0"))
        self.write("c6/VERSION", "0.3.0\n")
        self.commit()
        self.assertIn("the P4 uses esp_hosted 3.1.0 and the C6 3.2.0", " ".join(self.errs()))

    def test_hosted_upgrade_needs_a_tried_update(self):
        self.write(rr.P4_LOCK, lock("3.2.0"))
        self.write(rr.C6_LOCK, lock("3.2.0"))
        self.write("c6/VERSION", "0.3.0\n")
        self.commit()
        self.assertIn("3.1.0 -> 3.2.0", " ".join(self.errs()))
        self.write(rr.COMPAT, "3.1.0 -> 3.2.0: updated from 0.2.0 on Unit A, Wi-Fi back\n")
        self.commit()
        self.assertEqual([], self.errs())

    def test_hosted_upgrade_also_needs_a_c6_bump(self):
        self.write(rr.P4_LOCK, lock("3.2.0"))
        self.write(rr.C6_LOCK, lock("3.2.0"))
        self.write(rr.COMPAT, "3.1.0 -> 3.2.0: tried\n")
        self.commit()
        self.assertIn("still 0.2.0", " ".join(self.errs()))

    def test_first_release_with_c6_version(self):
        self.git("tag", "-d", "v0.2.0")
        self.git("rm", "-q", "c6/VERSION")
        self.commit("before c6 versions")
        self.git("tag", "v0.1.3")
        self.write("c6/VERSION", "0.2.0\n")
        self.write("c6/main/main.c", "int z;\n")
        self.commit()
        self.assertEqual([], self.errs())


if __name__ == "__main__":
    unittest.main()
