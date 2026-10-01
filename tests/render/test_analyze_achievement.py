import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("achievement_analysis", ROOT / "tools/analyze_achievement.py")
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


def row(active=0, elapsed=192, y=-85):
    def node(local, cached, size):
        return dict(local_xy=local, cached_xy=cached, size=size, pivot=[0, 0], rotation=[0, 0],
                    local_scale=[1, 1], cached_scale=[1, 1], visible=1,
                    raw_b8_cf=(struct.pack("<2f", 1, 1) + bytes(16)).hex())
    nodes = {"nick": node([0, y], [0, y], [800, 82]),
             "title": node([80, y], [0, y], [768, 85]),
             "nick_marker": node([0, 0], [0, 0], [0, 0]),
             "nick_root": node([0, 0], [0, 0], [0, 0]),
             "title_marker": node([0, 0], [-80, 0], [0, 0]),
             "title_root": node([-80, 0], [-80, 0], [0, 0]),
             "lcd_damage_anchor": node([0, 0], [-80, 0], [0, 0])}
    return dict(accepted=True, repeated_identity_equal=True, active=active,
                elapsed_native_units=elapsed, elapsed_after_native_units=elapsed+2,
                nodes=nodes, expected_routing=True, utc="2026-09-27T10:00:00Z", tick_ms=1000, tick_end_ms=1002,
                sources={"nick": {"texture_key": "0x555345520004f780", "texture_size": [512, 1024]},
                         "title": {"texture_key": "0x555345520004f880", "texture_size": [1024, 512]}},
                front_rectangle_center_size=[960, 540, 607.2034912109375, 1080])


def document(*rows):
    return {"metadata": {"schema": 1, "module_sha256": a.GAME_HASH, "pid": 77,
                         "process_created_filetime": 123}, "rows": list(rows)}


class Tests(unittest.TestCase):
    def test_visible_idle_hold_slide_and_offscreen_are_distinct(self):
        cases = [(0, 192, -85, "inactive_nick_parked"), (1, 1, 0, "active_hold_pose"),
                 (1, 185, -42.5, "active_retirement_visible_nick"),
                 (1, 190, -82, "active_nick_offscreen_passthrough"),
                 (1, 191, -85, "active_nick_offscreen_passthrough")]
        for active, elapsed, y, expected in cases:
            with self.subTest(expected=expected, y=y):
                result = a.classify(row(active, elapsed, y))
                self.assertEqual(result["phase"], expected)
                self.assertEqual(result["pose"], "native_cached_baseline")
        self.assertIn("not seconds", " ".join(a.LIMITS))
        self.assertIn("do not prove every commit", " ".join(a.LIMITS))

    def test_native_initialization_parked_nick_hidden_title_different_y(self):
        value = row(0, 180, -85)
        title = value["nodes"]["title"]
        title["local_xy"] = [80, 0]; title["cached_xy"] = [0, 0]
        title["raw_b8_cf"] = (struct.pack("<2f", 0, 1) + bytes(16)).hex()
        result = a.classify(value)
        self.assertEqual(result["phase"], "inactive_nick_parked")
        self.assertEqual(result["pose"], "native_cached_baseline")
        self.assertFalse(result["active_y_subset_of_production_guards"])
        self.assertFalse(result["native_local_pair_matches"])

    def test_borrowed_pose_and_native_restoration_observation(self):
        baseline = row(1, 40, 0)
        borrowed = copy.deepcopy(baseline)
        node = borrowed["nodes"]["nick"]
        # Independent arithmetic uses the production transform's original
        # [-.5,-.5,769,1367] front rectangle and native -.1 commit convention.
        fx, fy = 607.2034912109375 / 769, 1080 / 1367
        node["cached_xy"] = [a.f32(656.3982543945312 + fx * .4 + .1), a.f32(fy * .4 + .1)]
        node["cached_scale"] = [a.f32(fx), a.f32(fy)]
        restored = copy.deepcopy(baseline)
        result = a.analyze(document(baseline, borrowed, restored), {})
        self.assertEqual(result["cached_pose_counts"]["native_cached_baseline"], 2)
        self.assertEqual(result["cached_pose_counts"]["expected_borrowed_geometry_correspondence"], 1)
        self.assertEqual(len(result["runs"]), 3)
        self.assertGreater(result["runs"][0]["first_observation"]["predicted_submitted_bounds_if_applied"][2], 1263.6)

    def test_mixed_pair_hidden_ancestor_source_and_alpha_flags(self):
        value = row(1, 186, -40)
        value["nodes"]["title"]["local_xy"][1] = -42.5
        value["nodes"]["nick_root"]["visible"] = 0
        value["nodes"]["nick"]["raw_b8_cf"] = (struct.pack("<2f", 1, 0) + bytes(16)).hex()
        value["sources"]["nick"]["texture_key"] = "0x0"
        value["expected_routing"] = False
        result = a.classify(value)
        self.assertEqual(result["phase"], "active_mixed_pose")
        for expected in ("hidden_pair_or_ancestor", "native_nick_texture_differs", "native_pair_route_differs",
                         "pair_or_ancestor_not_positive_alpha"):
            self.assertIn(expected, result["flags"])

    def test_rejections_resets_clock_regression_and_missing_fields(self):
        first, second = row(1, 185, -42.5), row(1, 0, 0)
        second["tick_ms"] = 999
        bad = {"accepted": False, "tick_ms": 1000, "tick_end_ms": 1001,
               "utc": first["utc"], "error": "native identity changed"}
        result = a.analyze(document(first, second, bad), {})
        self.assertEqual(result["phase_counts"]["rejected_observation"], 1)
        flags = result["review_flags_nonexclusive_not_fault_proof"]
        self.assertEqual(flags["sample_clock_regression"], 1)
        self.assertEqual(flags["elapsed_reset_between_observations_possible_activation"], 1)
        with self.assertRaises((KeyError, ValueError)): a.classify({"accepted": True})

    def test_schema_count_nonfinite_and_lifetime_bounds(self):
        for field, value in (("module_sha256", "bad"), ("schema", 2), ("pid", 0), ("process_created_filetime", 0)):
            data = document(row()); data["metadata"][field] = value
            with self.assertRaises(ValueError): a.analyze(data, {})
        with self.assertRaises(ValueError): a.analyze(document(*[row()] * 301), {})
        value = row(); value["nodes"]["nick"]["cached_xy"][0] = float("nan")
        with self.assertRaises(ValueError): a.classify(value)

    def test_stable_archive_loading_and_live_path_change_rejection(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            path = directory / "capture.json"; path.write_text(json.dumps(document(row())), encoding="utf-8")
            loaded, source = a.load(path)
            self.assertEqual(a.analyze(loaded, source)["rows"], 1)
            self.assertEqual(len(source["sha256"]), 64)
            original = a.fingerprint
            calls = 0
            def changed(stat):
                nonlocal calls
                calls += 1
                value = original(stat)
                return value if calls < 3 else (*value[:3], value[3]+1)
            with mock.patch.object(a, "fingerprint", side_effect=changed):
                with self.assertRaisesRegex(ValueError, "changed"): a.load(path)
            with mock.patch.object(a, "MAX_BYTES", 1):
                with self.assertRaisesRegex(ValueError, "bound"): a.load(path)
            runtime = directory / "runtime"; runtime.mkdir()
            live = runtime / "capture.json"; live.write_bytes(path.read_bytes())
            with mock.patch.object(a, "ROOT", directory):
                with self.assertRaisesRegex(ValueError, "Archive"): a.load(live)
            path.write_text('{"a": NaN}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Nonfinite"): a.load(path)

    def test_preflight_and_exclusive_output(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name); path = directory / "capture.json"; output = directory / "report.json"
            data = document(row()); data["row"] = data.pop("rows")[0]
            path.write_text(json.dumps(data), encoding="utf-8")
            argv = ["analysis", "--json", str(path), "--output", str(output)]
            with mock.patch("sys.argv", argv): a.main()
            saved = output.read_bytes()
            with mock.patch("sys.argv", argv):
                with self.assertRaises(FileExistsError): a.main()
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()
