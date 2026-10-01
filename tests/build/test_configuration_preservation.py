"""Real filesystem preservation checks; only external build commands are mocked.

All module roots and copy paths point into TemporaryDirectory. No game, compiler,
Git checkout, device, or user configuration is accessed by these entry points.
"""
from contextlib import ExitStack, redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[2] / "tools"


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, TOOLS / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load("preservation_build_runtime", "build_runtime.py")
with patch.dict(sys.modules, {"build_runtime": runtime}):
    launcher = load("preservation_build_launcher", "build_launcher.py")
prepare = load("preservation_prepare", "prepare_game_workspace.py")


def inventory(root):
    return {p.relative_to(root).as_posix(): (p.read_bytes(), p.stat().st_mtime_ns,
                                           p.stat().st_mode)
            for p in root.rglob("*") if p.is_file()}


class ConfigurationPreservationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bone-config-deploy-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.game = self.root / "runtime/game"
        self.write("launch-settings.json", b'{"view":"closer150","input_profile":"desktop/profiles/gun.json"}\r\n')
        self.write("runtime/game/desktop/bone-eater-controls.xml", b"custom controls\r\n")
        self.write("runtime/game/desktop/profiles/gun.json", b"custom profile bytes\x00\xff")
        self.write("runtime/game/desktop/resize.json", b"custom window layout\r\n")
        self.write("runtime/game/desktop/patches.json", b"custom patch choices\r\n")
        self.write("runtime/game/conf/nvram/testmode-v.xml", b"custom calibration\r\n")
        self.write("runtime/game/conf/nvram/testmode-v.crc", b"\x01\x00\xa5\xff")
        self.write("runtime/game/conf/fixture-progress.bin", b"opaque save sentinel, not a real save format")
        self.write("runtime/game/modules/arkndd.dll", b"inert required-file sentinel")
        self.write("runtime/game/BoneEater.exe", b"old inert runtime")
        self.write("runtime/game/spice64.exe", b"old inert baseline")
        self.write("runtime/game/BoneEater-runtime-LICENSE.txt", b"old license")
        self.write("Play Bone Eater.exe", b"old inert companion")
        self.write("build/upstream-x64/spicetools/64/Release/BoneEater.exe", b"new inert runtime")
        self.write("build/upstream-baseline-x64/spicetools/64/Release/spice64.exe", b"new inert baseline")
        self.write("vendor/spice2x/src/spice2x/LICENSE", b"new license")
        self.write("build/launcher-x64/Release/Play Bone Eater.exe", b"new inert companion")

    def write(self, relative, contents):
        target = self.root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(contents)
        os.utime(target, ns=(1_700_000_000_000_000_000, 1_700_000_000_000_000_000))
        return target

    def runtime_call(self, arguments, *, build_failure=False):
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            if build_failure and "--build" in command:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)

        with ExitStack() as stack:
            stack.enter_context(patch.object(runtime, "ROOT", self.root))
            stack.enter_context(patch.object(runtime, "find_cmake", return_value="fixture-cmake"))
            # Patch-stack correctness has a separate real-Git fixture suite.
            patches = stack.enter_context(patch.object(runtime, "apply_runtime_patches"))
            revision = stack.enter_context(patch.object(runtime.subprocess, "check_output", return_value=runtime.PIN + "\n"))
            stack.enter_context(patch.object(runtime.subprocess, "run", side_effect=fake_run))
            stack.enter_context(patch.object(sys, "argv", ["build_runtime.py", *arguments]))
            stack.enter_context(redirect_stdout(io.StringIO()))
            runtime.main()
        self.assertEqual(len(calls), 2)
        self.assertIn("rev-parse", revision.call_args.args[0])
        self.assertEqual(patches.call_count, 0 if "--upstream-baseline" in arguments else 1)
        return calls

    def launcher_call(self, arguments, *, test_failure=False):
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            if test_failure and "--test-dir" in command:
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)

        with ExitStack() as stack:
            stack.enter_context(patch.object(launcher, "ROOT", self.root))
            stack.enter_context(patch.object(launcher, "find_cmake", return_value="fixture-cmake.exe"))
            stack.enter_context(patch.object(launcher.subprocess, "run", side_effect=fake_run))
            stack.enter_context(patch.object(sys, "argv", ["build_launcher.py", *arguments]))
            stack.enter_context(redirect_stdout(io.StringIO()))
            launcher.main()
        self.assertEqual(len(calls), 3)
        self.assertIn("--test-dir", calls[-1])

    def assert_only_changed(self, before, expected):
        after = inventory(self.root)
        self.assertEqual(set(before), set(after), "No configuration or other file may be added/deleted")
        self.assertEqual({name for name in before if before[name] != after[name]}, set(expected))
        for name, contents in expected.items():
            self.assertEqual(after[name][0], contents)

    def test_runtime_deploy_changes_only_executable_and_license(self):
        before = inventory(self.root)
        self.runtime_call(["--deploy"])
        self.assert_only_changed(before, {
            "runtime/game/BoneEater.exe": b"new inert runtime",
            "runtime/game/BoneEater-runtime-LICENSE.txt": b"new license"})

    def test_upstream_deploy_does_not_replace_dedicated_runtime_or_settings(self):
        before = inventory(self.root)
        self.runtime_call(["--upstream-baseline", "--deploy"])
        self.assert_only_changed(before, {
            "runtime/game/spice64.exe": b"new inert baseline",
            "runtime/game/BoneEater-runtime-LICENSE.txt": b"new license"})

    def test_build_without_deploy_and_failed_build_preserve_everything(self):
        before = inventory(self.root)
        self.runtime_call([])
        self.assertEqual(inventory(self.root), before)
        with self.assertRaises(subprocess.CalledProcessError):
            self.runtime_call(["--deploy"], build_failure=True)
        self.assertEqual(inventory(self.root), before)

    def test_missing_working_copy_guard_refuses_before_any_deploy_write(self):
        (self.game / "modules/arkndd.dll").unlink()
        before = inventory(self.root)
        with self.assertRaisesRegex(RuntimeError, "Prepare runtime/game"):
            self.runtime_call(["--deploy"])
        self.assertEqual(inventory(self.root), before)

    def test_launcher_deploy_changes_only_companion_and_never_runtime(self):
        before = inventory(self.root)
        self.launcher_call(["--deploy"])
        self.assert_only_changed(before, {"Play Bone Eater.exe": b"new inert companion"})

    def test_launcher_no_deploy_or_failed_ctest_keeps_every_setting(self):
        before = inventory(self.root)
        self.launcher_call([])
        self.assertEqual(inventory(self.root), before)
        with self.assertRaises(subprocess.CalledProcessError):
            self.launcher_call(["--deploy"], test_failure=True)
        self.assertEqual(inventory(self.root), before)

    def preparation_call(self, arguments):
        source = self.root / "original game/Silent Scope Bone Eater"
        with ExitStack() as stack:
            for name, value in {"WORKSPACE": self.root, "SOURCE": source,
                                "DESTINATION": self.game,
                                "MANIFEST": self.root / "runtime/game_workspace_manifest.json",
                                "STAGING": self.root / "runtime/prepare_staging"}.items():
                stack.enter_context(patch.object(prepare, name, value))
            stack.enter_context(patch.object(sys, "argv", ["prepare_game_workspace.py", *arguments]))
            stack.enter_context(redirect_stdout(io.StringIO()))
            return prepare.main()

    def test_preparation_refuses_existing_calibration_before_copying_any_missing_file(self):
        self.write("original game/Silent Scope Bone Eater/conf/nvram/testmode-v.xml", b"factory calibration")
        self.write("original game/Silent Scope Bone Eater/data/missing-asset.bin", b"inert asset")
        before = inventory(self.root)
        self.assertEqual(self.preparation_call([]), 2)
        self.assertEqual(inventory(self.root), before)
        self.assertFalse((self.root / "runtime/prepare_staging").exists())

    def test_preparation_adds_missing_assets_without_erasing_extra_user_files(self):
        self.write("original game/Silent Scope Bone Eater/data/asset.bin", b"inert asset")
        before = inventory(self.root)
        self.assertEqual(self.preparation_call([]), 0)
        after = inventory(self.root)
        for name, saved in before.items():
            self.assertEqual(after[name], saved, name)
        self.assertEqual(set(after) - set(before), {
            "runtime/game/data/asset.bin", "runtime/game_workspace_manifest.json"})
        self.assertEqual(after["runtime/game/data/asset.bin"][0], b"inert asset")


if __name__ == "__main__":
    unittest.main()
