"""Temporary-directory evidence tests; never opens the installed game's files."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location(
    "archive_run", Path(__file__).resolve().parents[2] / "tools/archive_run.py")
archive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(archive)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.desktop = self.root / "runtime/game/desktop"
        self.desktop.mkdir(parents=True)
        self.path = self.root / "run.json"
        self.serial = 0
        self.pre = b"pid,value\r\n46096,older-reused-pid\r\n17,other-old\r\n"
        self.post = self.pre + b'46096,"new,quoted"\r\n18,other-new\r\n46096,new-last\r\n'
        self.make_manifest()

    def write_csv(self, name, data, tick=200):
        path = self.desktop / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, ns=(tick, tick))
        stat = path.stat()
        return {"size": stat.st_size, "mtime_ns": stat.st_mtime_ns}

    def make_manifest(self, pre=None, post=None, name="scope-v3.csv", newly_created=False):
        pre = self.pre if pre is None else pre
        post = self.post if post is None else post
        before = self.write_csv(name, pre, 100)
        after = self.write_csv(name, post, 200)
        self.document = {"pid": 46096, "exe_sha256": "a" * 64,
                         "start_utc_ns": 150, "end_utc_ns": 300, "exit_code": 0,
                         "before": {} if newly_created else {name: before}, "after": {name: after}}
        self.save()

    def save(self):
        self.path.write_text(json.dumps(self.document), encoding="utf-8")

    def run_archive(self, **kwargs):
        self.serial += 1
        return archive.archive_run(self.root, 46096, f"trial-{self.serial}",
                                   run_manifest=self.path, **kwargs)

    def test_same_pid_reuse_is_excluded_and_exact_bytes_retained(self):
        destination, result = self.run_archive()
        self.assertEqual((destination / "scope-v3.csv").read_bytes(),
                         b'pid,value\r\n46096,"new,quoted"\r\n46096,new-last\r\n')
        entry = result["files"]["scope-v3.csv"]
        self.assertEqual((entry["rows"], entry["region_rows_scanned"]), (2, 3))
        self.assertEqual((entry["start_byte"], entry["end_byte"]), (len(self.pre), len(self.post)))
        self.assertEqual((destination / "run-manifest.json").read_bytes(), self.path.read_bytes())
        self.assertEqual(result["mode"], "run-manifest-byte-bounds")
        self.assertTrue(result["complete"])

    def test_new_and_unchanged_files(self):
        self.make_manifest(post=b"pid,value\n46096,new\n", newly_created=True)
        destination, result = self.run_archive()
        self.assertEqual(result["files"]["scope-v3.csv"]["rows"], 1)
        self.assertEqual((destination / "scope-v3.csv").read_bytes(), b"pid,value\n46096,new\n")
        self.document["before"] = dict(self.document["after"])
        self.document["start_utc_ns"] = 250
        self.save()
        destination, result = self.run_archive()
        self.assertEqual((destination / "scope-v3.csv").read_bytes(), b"pid,value\n")
        self.assertEqual(result["files"]["scope-v3.csv"]["region_bytes"], 0)

    def test_unbounded_failure_log_omitted_current_log_explicit(self):
        self.write_csv("present-failures-v1.log", b"pid=46096 old failure\n")
        (self.desktop / "game.log").write_bytes(b"caller verified current run\n")
        destination, result = self.run_archive(current_log=True)
        self.assertFalse((destination / "present-failures-v1.log").exists())
        self.assertIn("No run byte bounds", result["omitted"]["present-failures-v1.log"])
        self.assertFalse(result["files"]["game.log"]["run_boundaries_verified"])

    def test_legacy_mode_remains_explicitly_pid_only(self):
        destination, result = archive.archive_run(self.root, 46096, "legacy")
        content = (destination / "scope-v3.csv").read_text()
        self.assertIn("older-reused-pid", content)
        self.assertIn("new-last", content)
        self.assertEqual(result["mode"], "pid-only")
        self.assertIn("reused", result["limitation"])

    def test_never_overwrites_existing_archive(self):
        destination, result = self.run_archive()
        initial = {p.name: p.read_bytes() for p in destination.iterdir()}
        with self.assertRaises(FileExistsError):
            archive.archive_run(self.root, 46096, "trial-1", run_manifest=self.path)
        self.assertEqual(initial, {p.name: p.read_bytes() for p in destination.iterdir()})

    def test_pid_closed_run_and_numeric_validation(self):
        for field, value in [("pid", 22), ("pid", True), ("exit_code", None),
                             ("start_utc_ns", 301), ("end_utc_ns", 0),
                             ("exe_sha256", "not-a-hash")]:
            with self.subTest(field=field, value=value):
                self.make_manifest();self.document[field] = value;self.save()
                with self.assertRaises(ValueError):self.run_archive()
        for field, value in [("size", -1), ("size", True), ("mtime_ns", 400), ("mtime_ns", "200")]:
            self.make_manifest();self.document["after"]["scope-v3.csv"][field] = value;self.save()
            with self.assertRaises(ValueError):self.run_archive()

    def test_malformed_duplicate_and_oversized_manifest(self):
        for raw in [b'{"pid":46096,"pid":46096}', b"[]", b"{", b"x"*(archive.MAX_MANIFEST_BYTES+1)]:
            with self.subTest(prefix=raw[:30]):
                self.path.write_bytes(raw)
                with self.assertRaises(ValueError):self.run_archive()

    def test_path_traversal_ads_windows_aliases_and_basename_collision(self):
        for name in ["../scope.csv", "/scope.csv", "C:/scope.csv", "scope.csv:ads",
                     "captures//scope.csv", "captures/./scope.csv", "NUL.csv", "bad /scope.csv"]:
            with self.subTest(name=name):
                self.make_manifest();self.document["after"] = {name: {"size": 1, "mtime_ns": 200}};self.save()
                with self.assertRaises(ValueError):self.run_archive()
        self.make_manifest()
        self.document["after"]["captures/scope-v3.csv"] = self.document["after"]["scope-v3.csv"]
        self.save()
        with self.assertRaises(ValueError):self.run_archive()
        self.make_manifest();self.document["after"]["SCOPE-V3.csv"] = self.document["after"]["scope-v3.csv"]
        self.save()
        with self.assertRaises(ValueError):self.run_archive()

    def test_backslash_nested_path_is_normalized(self):
        self.make_manifest(name="captures/raw-alpha-v1.csv")
        for side in ["before", "after"]:
            self.document[side]["captures\\raw-alpha-v1.csv"] = self.document[side].pop("captures/raw-alpha-v1.csv")
        self.save();destination, result = self.run_archive()
        self.assertTrue((destination / "raw-alpha-v1.csv").exists())
        self.assertIn("captures/raw-alpha-v1.csv", result["files"])

    def test_removed_truncated_and_metadata_only_changes_reject(self):
        self.document["after"] = {};self.save()
        with self.assertRaises(ValueError):self.run_archive()
        self.make_manifest(post=b"pid,value\n");
        with self.assertRaises(ValueError):self.run_archive()
        self.make_manifest(post=self.pre)
        with self.assertRaises(ValueError):self.run_archive()

    def test_current_closed_metadata_mismatch_rejects_before_archive(self):
        for action in ["append", "mtime", "delete"]:
            self.make_manifest();source = self.desktop / "scope-v3.csv"
            if action == "append":source.write_bytes(self.post+b"46096,later\n")
            elif action == "mtime":os.utime(source, ns=(1000, 1000))
            else:source.unlink()
            with self.assertRaises((ValueError, FileNotFoundError)):self.run_archive()
        self.assertFalse((self.root / "review/logs").exists())

    def test_partial_pre_boundary_post_row_and_inside_header(self):
        cases=[(b"pid,value\n46096,partial", b"pid,value\n46096,partial-finished\n"),
               (b"pid,value\n", b"pid,value\n46096,incomplete"),
               (b"pid,", b"pid,value\n46096,new\n")]
        for pre, post in cases:
            self.make_manifest(pre=pre, post=post)
            with self.assertRaises(ValueError):self.run_archive()

    def test_bad_schema_and_malformed_rows_even_other_pid_reject(self):
        cases=[b"pid,pid\n46096,46096\n", b"name,value\n46096,x\n", b"pid,value\n18,x,extra\n",
               b'pid,value\n18,"open\n', b"pid,value\n18\n", b"pid,value\nnotpid,x\n",
               b"pid,value\n18,\xff\n", b"pid,value\n18,x\r\r\n"]
        for raw in cases:
            with self.subTest(raw=raw):
                self.make_manifest(post=raw, newly_created=True)
                with self.assertRaises((ValueError, archive.csv.Error)):self.run_archive()

    def test_failed_archive_is_marked_incomplete(self):
        self.make_manifest(pre=b"pid,value\n", post=b"pid,value\n46096,ok\n46096,too,many\n")
        with self.assertRaises(ValueError):self.run_archive()
        result=json.loads((self.root / "review/logs/trial-1-46096/archive.json").read_text())
        self.assertFalse(result["complete"])
        self.assertIn("Malformed diagnostic row", result["error"])
        with self.assertRaises(FileExistsError):
            archive.archive_run(self.root, 46096, "trial-1", run_manifest=self.path)

    def test_change_during_read_or_after_earlier_file_rejects(self):
        original=archive.parse_line
        def change(raw, name):
            value=original(raw,name)
            if value and value[0]=="46096":
                source=self.desktop/name
                os.utime(source,ns=(1000,1000))
            return value
        with mock.patch.object(archive,"parse_line",side_effect=change):
            with self.assertRaisesRegex(ValueError,"Closed-run"):self.run_archive()
        self.make_manifest()
        original_region=archive.archive_region
        def late_change(*args,**kwargs):
            result=original_region(*args,**kwargs)
            os.utime(self.desktop/"scope-v3.csv",ns=(1000,1000))
            return result
        with mock.patch.object(archive,"archive_region",side_effect=late_change):
            with self.assertRaisesRegex(ValueError,"Closed-run"):self.run_archive()

    def test_identical_metadata_substitution_cannot_be_detected(self):
        # This is an explicit evidence limit, not a claimed tamper-proof check.
        self.make_manifest()
        altered=self.post.replace(b"new-last",b"replaced")
        self.assertEqual(len(altered),len(self.post))
        self.write_csv("scope-v3.csv",altered,200)
        destination,result=self.run_archive()
        self.assertIn(b"replaced",(destination/"scope-v3.csv").read_bytes())
        self.assertIn("not detectable",result["limitation"])


if __name__ == "__main__":
    unittest.main()
