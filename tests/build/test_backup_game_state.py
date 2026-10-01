"""Backup tests use only temporary fixture files; no real game/process inventory.

Windows handle tests exercise actual file sharing, final-path checks and junction
rejection on private test paths. Process enumeration has a metadata-only fake.
"""
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import MagicMock, patch
import zlib


spec = importlib.util.spec_from_file_location(
    "backup_game_state", Path(__file__).resolve().parents[2] / "tools/backup_game_state.py")
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


@unittest.skipUnless(os.name == "nt", "Real Win32 handle validation")
class BackupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bone-state-backup-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.checks = []
        self.stopped = lambda root: self.checks.append(root)
        xml = b"<state>opaque calibrated native bytes\r\n</state>"
        self.write(backup.CALIBRATION_XML, xml)
        self.write(backup.CALIBRATION_CRC, struct.pack("<I", zlib.crc32(xml)))
        self.write("runtime/game/desktop/bone-eater-controls.xml", b"custom controls\r\n")
        self.write("runtime/game/desktop/controls.xml", b"")
        self.write("runtime/game/conf/raw/nddrank/nddrank_st_nml.dat", b"opaque ranking" * 30)
        self.write("runtime/game/conf/raw/bookkeeping/0/BOOKKEEPINGS", b"native counter bytes")
        self.write("runtime/game/prop/ark-config.xml", b"custom boot settings")
        self.profile = "desktop/profiles/\u9078\u629e.json"
        self.write("runtime/game/" + self.profile, b'{"unknown_fixture_bytes":true}\r\n')
        self.settings = {"schema_version": 1, "view": "balanced125", "main_dof_off": False,
                         "input_profile": self.profile}
        self.write_settings()
        self.write("runtime/game/conf/raw/aska/AHSLDiskCacheDX11", b"not state: graphics cache")
        self.write("runtime/game/desktop/game.log", b"not state: log")
        self.write("runtime/game/modules/arkndd.dll", b"inert module sentinel")
        self.write("original game/Silent Scope Bone Eater/conf/nvram/testmode-v.xml", b"original untouched")

    def write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, ns=(1_700_000_000_000_000_000, 1_700_000_000_000_000_000))
        return path

    def write_settings(self):
        self.write("launch-settings.json", json.dumps(self.settings, ensure_ascii=False).encode("utf8"))

    def inventory(self):
        return {path.relative_to(self.root).as_posix():
                (path.read_bytes(), path.stat().st_mtime_ns, path.stat().st_file_attributes)
                for path in self.root.rglob("*") if path.is_file() and "backups" not in path.relative_to(self.root).parts}

    def run_backup(self, **kwargs):
        return backup.backup(self.root, check_stopped=self.stopped, **kwargs)

    def test_dry_run_no_writes_and_explicit_inventory(self):
        before = self.inventory()
        result = self.run_backup()
        self.assertEqual(result["mode"], "dry-run")
        self.assertEqual(before, self.inventory())
        self.assertFalse((self.root / "backups").exists())
        paths = {f["source"] for f in result["files"]}
        self.assertIn("runtime/game/" + self.profile, paths)
        self.assertNotIn("runtime/game/desktop/game.log", paths)
        self.assertNotIn("runtime/game/conf/raw/aska/AHSLDiskCacheDX11", paths)
        self.assertIn("runtime/game/card0.txt", result["missing_optional"])
        self.assertEqual(len(self.checks), 2)

    def test_created_snapshot_hashes_bytes_metadata_and_no_overwrite(self):
        controls = self.root / "runtime/game/desktop/bone-eater-controls.xml"
        os.chmod(controls, 0o444)
        self.addCleanup(os.chmod, controls, 0o666)
        before = self.inventory()
        first = self.run_backup(create=True)
        folder = Path(first["backup_directory"])
        manifest = json.loads((folder / "manifest.json").read_text("utf8"))
        for record in manifest["files"]:
            source = self.root / record["source"]
            copied = (folder / "files" / record["source"]).read_bytes()
            self.assertEqual(copied, source.read_bytes())
            self.assertEqual(backup.hashlib.sha256(copied).hexdigest(), record["sha256"])
            self.assertEqual(len(copied), record["size"])
        saved = (folder / "manifest.json").read_bytes()
        second = self.run_backup(create=True)
        self.assertNotEqual(first["backup_directory"], second["backup_directory"])
        self.assertEqual(saved, (folder / "manifest.json").read_bytes())
        self.assertEqual(before, self.inventory())
        self.assertTrue(manifest["calibration_crc32_valid"])
        self.assertEqual(len(self.checks), 6)

    def test_corrupt_or_missing_crc_pair_refuses_before_output(self):
        for change in (b"\0\0\0\0", b"\0", None):
            with self.subTest(change=change):
                crc = self.root / backup.CALIBRATION_CRC
                if change is None:
                    crc.unlink()
                else:
                    crc.write_bytes(change)
                with self.assertRaises(RuntimeError):
                    self.run_backup(create=True)
                self.assertFalse((self.root / "backups").exists())

    def test_process_refusal_initial_final_collection_and_final_publication(self):
        for failure_at in (1, 2, 3):
            calls = [0]
            def check(_):
                calls[0] += 1
                if calls[0] == failure_at:
                    raise RuntimeError("fixture game/companion is running")
            with self.subTest(failure_at=failure_at), self.assertRaisesRegex(RuntimeError, "running"):
                backup.backup(self.root, create=True, check_stopped=check)
            container = self.root / "backups/game-state"
            if container.exists():
                self.assertTrue(all(p.name.endswith(".incomplete") for p in container.iterdir()))

    def test_copy_failure_retains_only_incomplete_and_sources_unchanged(self):
        before = self.inventory()
        original = backup.write_exclusive
        count = [0]
        def fail(path, data):
            count[0] += 1
            if count[0] == 3:
                raise OSError("fixture disk full")
            original(path, data)
        with patch.object(backup, "write_exclusive", fail), self.assertRaisesRegex(RuntimeError, "not published"):
            self.run_backup(create=True)
        stages = list((self.root / "backups/game-state").iterdir())
        self.assertEqual(len(stages), 1)
        self.assertTrue(stages[0].name.endswith(".incomplete"))
        self.assertFalse((stages[0] / "manifest.json").exists())
        self.assertEqual(before, self.inventory())

    def test_manifest_failure_and_publish_failure_never_claim_success(self):
        original = backup.write_exclusive
        def fail(path, data):
            if path.name == "manifest.json":
                raise OSError("manifest flush failed")
            original(path, data)
        with patch.object(backup, "write_exclusive", fail), self.assertRaisesRegex(RuntimeError, "not published"):
            self.run_backup(create=True)
        with patch.object(Path, "rename", side_effect=OSError("fixture rename failed")), self.assertRaisesRegex(RuntimeError, "not published"):
            self.run_backup(create=True)
        self.assertTrue(all(p.name.endswith(".incomplete") for p in (self.root / "backups/game-state").iterdir()))

    def test_file_and_total_byte_limits_precede_output(self):
        with patch.object(backup, "MAX_TOTAL", 10), self.assertRaisesRegex(RuntimeError, "total"):
            self.run_backup(create=True)
        self.write("runtime/game/desktop/resize.json", b"x" * (backup.MAX_FILE + 1))
        with self.assertRaisesRegex(RuntimeError, "byte limit"):
            self.run_backup(create=True)
        self.assertFalse((self.root / "backups").exists())

    def test_optional_appearance_prevents_publication(self):
        count = [0]
        def check(_):
            count[0] += 1
            if count[0] == 2:
                self.write("runtime/game/card0.txt", b"fixture private ID")
        with self.assertRaisesRegex(RuntimeError, "appeared"):
            backup.backup(self.root, create=True, check_stopped=check)
        self.assertFalse((self.root / "backups").exists())

    def test_profile_missing_external_traversal_ads_device_and_original_refused(self):
        external = self.root.parent / "outside-profile.json"
        for value in (str(external), "../../../outside-profile.json", "C:relative.json", "\\rooted.json",
                      "\\\\server\\share\\profile.json", "desktop/profile.json:stream", "desktop/CON.json",
                      "desktop/profiles/absent.json", "../../original game/old.json", "desktop/bad. /profile.json"):
            with self.subTest(value=value):
                self.settings["input_profile"] = value
                self.write_settings()
                with self.assertRaises((RuntimeError, OSError)):
                    self.run_backup(create=True)
        self.assertFalse((self.root / "backups").exists())

    def test_explicit_profile_absolute_workspace_unicode_and_size_gate(self):
        self.settings["input_profile"] = None
        self.write_settings()
        result = self.run_backup(profiles=[str(self.root / "runtime/game" / self.profile)])
        self.assertEqual(result["input_profiles"], ["runtime/game/" + self.profile])
        self.write("runtime/game/desktop/alternate.config", b"opaque malformed config worth preserving")
        result = self.run_backup(profiles=["desktop/alternate.config"])
        self.assertIn("runtime/game/desktop/alternate.config", result["input_profiles"])
        self.write("runtime/game/" + self.profile, b"x" * (backup.MAX_PROFILE + 1))
        with self.assertRaisesRegex(RuntimeError, "byte limit"):
            self.run_backup(profiles=[self.profile])
        with self.assertRaisesRegex(RuntimeError, "eight"):
            self.run_backup(profiles=[self.profile] * 9)

    def test_case_insensitive_profile_aliases_do_not_duplicate_snapshot_files(self):
        result = self.run_backup(create=True, profiles=[self.profile.upper(), self.profile])
        sources = [entry["source"].casefold() for entry in result["files"]]
        self.assertEqual(len(sources), len(set(sources)))
        self.assertEqual(len(result["input_profiles"]), 1)
        copied = Path(result["backup_directory"]) / "files" / result["input_profiles"][0]
        self.assertEqual(copied.read_bytes(), (self.root / "runtime/game" / self.profile).read_bytes())
        # Python casefold() merges these names; the filesystem's distinct file
        # identities must keep both explicitly nominated profiles.
        self.write("runtime/game/desktop/stra\u00dfe.config", b"first")
        self.write("runtime/game/desktop/strasse.config", b"second")
        result = self.run_backup(profiles=["desktop/stra\u00dfe.config", "desktop/strasse.config"])
        self.assertEqual(len(result["input_profiles"]), 3)

    def test_source_handles_deny_writes_deletion_and_directory_rename(self):
        relative = "runtime/game/desktop/bone-eater-controls.xml"
        source = self.root / relative
        with backup.WindowsFiles(self.root) as handles:
            handles.read(relative, required=True)
            with self.assertRaises(OSError): source.write_bytes(b"overwrite")
            with self.assertRaises(OSError): source.unlink()
            with self.assertRaises(OSError): source.parent.rename(source.parent.with_name("moved"))
        self.assertEqual(source.read_bytes(), b"custom controls\r\n")
        # Handles must be released on leaving the context.
        source.write_bytes(b"allowed only after test context closed")

    def test_hardlink_and_source_directory_junction_refused(self):
        import _winapi
        source = self.root / "runtime/game/desktop/controls.xml"
        extra = self.root / "hardlink.xml"
        os.link(source, extra)
        with self.assertRaisesRegex(RuntimeError, "Hardlinked"):
            self.run_backup()
        extra.unlink()
        real = source.parent.with_name("real-desktop")
        source.parent.rename(real)
        _winapi.CreateJunction(str(real), str(source.parent))
        try:
            with self.assertRaisesRegex(RuntimeError, "linked"):
                self.run_backup(create=True)
        finally:
            os.rmdir(source.parent)  # Remove only the fixture junction, never its target.
            real.rename(source.parent)

    def test_backup_directory_junction_refused_without_external_writes(self):
        import _winapi
        target = self.root / "unrelated-output"
        target.mkdir()
        _winapi.CreateJunction(str(target), str(self.root / "backups"))
        try:
            with self.assertRaisesRegex(RuntimeError, "linked"):
                self.run_backup(create=True)
            self.assertEqual(list(target.iterdir()), [])
        finally:
            os.rmdir(self.root / "backups")


class PureTests(unittest.TestCase):
    def test_settings_strict_schema_reference_and_duplicates(self):
        good = {"schema_version": 1, "view": "balanced125", "main_dof_off": False, "input_profile": None}
        self.assertIsNone(backup.settings_profile(json.dumps(good).encode()))
        bad = (b"", b"[]", b"{}", b"\xff", b"\0", b'{"input_profile":"x", "input_profile":"y"}',
               json.dumps(dict(good, schema_version=True)).encode(),
               json.dumps(dict(good, main_dof_off=0)).encode(),
               json.dumps(dict(good, input_profile=42)).encode(),
               json.dumps(dict(good, input_profile="x\0y")).encode(),
               json.dumps(dict(good, input_profile="")).encode(),
               json.dumps(dict(good, unknown=True)).encode(), b"x" * 16385)
        for data in bad:
            with self.subTest(data=data[:50]), self.assertRaises((ValueError, RuntimeError)):
                backup.settings_profile(data)

    @unittest.skipUnless(os.name == "nt", "Win32 metadata ABI fixture")
    def test_process_inventory_names_paths_inaccessible_and_complete(self):
        root = Path("C:/fixture-workspace")
        def kernel(rows, inaccessible=False, incomplete=False):
            k = MagicMock()
            k.CreateToolhelp32Snapshot.return_value = 17
            index = [-1]
            def next_row(_, pointer):
                index[0] += 1
                if index[0] == len(rows):
                    ctypes.set_last_error(5 if incomplete else 18)
                    return False
                pointer._obj.name, path = rows[index[0]]
                pointer._obj.pid = 123 + index[0]
                return True
            k.Process32FirstW.side_effect = next_row
            k.Process32NextW.side_effect = next_row
            k.OpenProcess.return_value = None if inaccessible else 19
            def image(_, __, buffer, count):
                buffer.value = rows[index[0]][1]
                count._obj.value = len(buffer.value)
                return True
            k.QueryFullProcessImageNameW.side_effect = image
            return k
        for name in ("BoneEater.exe", "spice64.exe", "spice.exe", "Play Bone Eater.exe"):
            k = kernel([(name, str(root / "runtime/game" / name))])
            with patch.object(ctypes, "WinDLL", return_value=k), self.assertRaisesRegex(RuntimeError, "running"):
                backup.ensure_stopped(root)
            self.assertEqual(k.CloseHandle.call_count, 2)
        cases = [(kernel([("BoneEater.exe", "C:/another-game/BoneEater.exe")]), False),
                 (kernel([("other.exe", "C:/fixture-workspace/other.exe")]), False),
                 (kernel([("BoneEater.exe", "unknown")], inaccessible=True), True),
                 (kernel([("other.exe", "unknown")], incomplete=True), True)]
        for k, rejects in cases:
            with patch.object(ctypes, "WinDLL", return_value=k):
                if rejects:
                    with self.assertRaises(RuntimeError): backup.ensure_stopped(root)
                else: backup.ensure_stopped(root)


if __name__ == "__main__":
    unittest.main()
