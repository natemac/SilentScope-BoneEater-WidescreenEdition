"""Regression tests for overlapping integration patch stacks."""

from contextlib import redirect_stdout
import difflib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "build_runtime", Path(__file__).resolve().parents[2] / "tools/build_runtime.py")
runtime = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runtime)


class PatchReplayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bone-patch-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "repo"
        self.checkout.mkdir()
        self.patches = self.root / "patches"
        self.patches.mkdir()
        self.git("init", "--quiet")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "user.name", "Patch replay test")
        self.git("config", "user.email", "patch-test@example.invalid")
        self.original = '#include "one.h"\n#include "two.h"\n\nint main() { return 0; }\n'
        self.first = self.original.replace('\nint main', '\n#ifdef APP\n#include "layout.h"\n#endif\n\nint main')
        self.second = self.first.replace('#include "layout.h"', '#include "layout.h"\n#include "capture.h"')
        (self.checkout / "main.cpp").write_text(self.original, newline="\n")
        (self.checkout / "other.cpp").write_text("value = 1;\n", newline="\n")
        (self.checkout / "untouched.txt").write_text("keep me\n", newline="\n")
        self.git("add", ".")
        self.git("commit", "--quiet", "-m", "fixture")
        self.pin = self.git("rev-parse", "HEAD").decode().strip()
        self.index_before = (self.checkout / ".git/index").read_bytes()
        self.write_patch("one.patch", "main.cpp", self.original, self.first)
        self.write_patch("two.patch", "main.cpp", self.first, self.second)
        self.write_patch("three.patch", "other.cpp", "value = 1;\n", "value = 2;\n")
        self.manifest(["one.patch", "two.patch", "three.patch"])

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.checkout), *args], stderr=subprocess.STDOUT)

    def write_patch(self, name, target, old, new):
        contents = "".join(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
                                              fromfile="a/" + target, tofile="b/" + target))
        (self.patches / name).write_text(contents, newline="\n")

    def manifest(self, names):
        (self.patches / "series.json").write_text(json.dumps({"patches": names}))

    def replay(self):
        with redirect_stdout(io.StringIO()):
            runtime.apply_runtime_patches(self.checkout, self.patches, self.pin)

    def assert_final(self):
        self.assertEqual((self.checkout / "main.cpp").read_text(), self.second)
        self.assertEqual((self.checkout / "other.cpp").read_text(), "value = 2;\n")
        self.assertEqual((self.checkout / ".git/index").read_bytes(), self.index_before)

    def test_fresh_stack_then_idempotent_overlapping_replay(self):
        self.replay()
        self.assert_final()
        # Demonstrate the original reverse-check strategy fails on this stack.
        result = subprocess.run(["git", "-C", str(self.checkout), "apply", "--reverse", "--check",
                                 str(self.patches / "one.patch")], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.replay()
        self.assert_final()

    def test_partial_prefix_appends_remaining_patches(self):
        self.git("apply", str(self.patches / "one.patch"))
        self.replay()
        self.assert_final()

    def test_new_patch_appended_to_previously_complete_series(self):
        self.manifest(["one.patch", "two.patch"])
        self.replay()
        self.manifest(["one.patch", "two.patch", "three.patch"])
        self.replay()
        self.assert_final()

    def test_unrelated_local_edits_and_untracked_files_are_preserved(self):
        (self.checkout / "untouched.txt").write_text("user edit\n")
        (self.checkout / "user-notes.txt").write_text("keep this too\n")
        self.replay()
        self.assert_final()
        self.assertEqual((self.checkout / "untouched.txt").read_text(), "user edit\n")
        self.assertEqual((self.checkout / "user-notes.txt").read_text(), "keep this too\n")

    def test_unknown_edit_to_any_touched_file_refuses_before_writing(self):
        self.git("apply", str(self.patches / "one.patch"))
        modified = "// unrelated local edit inside a touched file\n" + self.first
        (self.checkout / "main.cpp").write_text(modified, newline="\n")
        with self.assertRaisesRegex(RuntimeError, "unknown edits"):
            self.replay()
        self.assertEqual((self.checkout / "main.cpp").read_text(), modified)
        self.assertEqual((self.checkout / "other.cpp").read_text(), "value = 1;\n")
        self.assertEqual((self.checkout / ".git/index").read_bytes(), self.index_before)

    def test_nonprefix_combination_is_refused(self):
        self.git("apply", str(self.patches / "three.patch"))
        with self.assertRaisesRegex(RuntimeError, "complete patch prefix"):
            self.replay()
        self.assertEqual((self.checkout / "main.cpp").read_text(), self.original)
        self.assertEqual((self.checkout / "other.cpp").read_text(), "value = 2;\n")

    def test_bad_later_patch_leaves_checkout_untouched(self):
        (self.patches / "two.patch").write_text("not a patch\n")
        with self.assertRaises(subprocess.CalledProcessError):
            self.replay()
        self.assertEqual((self.checkout / "main.cpp").read_text(), self.original)
        self.assertEqual((self.checkout / "other.cpp").read_text(), "value = 1;\n")

    def test_completed_crlf_checkout_matches_pinned_lf_snapshots(self):
        self.replay()
        for name in ("main.cpp", "other.cpp"):
            file = self.checkout / name
            file.write_bytes(file.read_bytes().replace(b"\n", b"\r\n"))
        before = {name: (self.checkout / name).read_bytes() for name in ("main.cpp", "other.cpp")}
        self.replay()
        self.assertEqual(before, {name: (self.checkout / name).read_bytes() for name in before})

    def test_fresh_git_crlf_checkout_accepts_pending_patches(self):
        self.git("config", "core.autocrlf", "true")
        for name in ("main.cpp", "other.cpp"):
            file = self.checkout / name
            file.write_bytes(file.read_bytes().replace(b"\n", b"\r\n"))
        self.replay()
        self.assert_final()


if __name__ == "__main__":
    unittest.main()
