import importlib.util
import json
import math
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ownership_analysis", ROOT / "tools/analyze_input_ownership.py")
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)


def native_row(**changes):
    row = {key: "0" for key in a.FIELDS}
    for key in a.POINTERS:
        row[key] = "0x1000"
    row.update(schema="1", pid="77", thread="12", status="completed_same_update", tick_ms="1000", end_tick_ms="1002",
               serial="20", serial_after="20", ray_serial="20", returned="1", before_valid="1", after_valid="1",
               completed="1", completed_tick_ms="995", scope_calls="1", conversion_calls="1", ray_calls="1",
               ray_verified="1", ray_input_read="1", scope_enabled="1", scope_off_known="1", ray_scope_known="1",
               converted_x="915", converted_y="478", pre_x="915", pre_y="478", ray_x="905", ray_y="478",
               post_x="905", post_y="478", ray_input_x="905", ray_input_y="478", gain_x="0.5", gain_y="0.5",
               anchor_x="895", anchor_y="478", zoom_before="0.2", zoom_after="0.2", held="4")
    row.update({k: str(v) for k, v in changes.items()})
    return row


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "archive.csv"

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, *rows, suffix="\n"):
        contents = ",".join(a.FIELDS) + "\n"
        contents += "\n".join(",".join(row[key] for key in a.FIELDS) for row in rows) + suffix
        self.path.write_text(contents, encoding="utf-8")
        return self.path

    def report(self, *rows):
        self.write(*rows)
        selected, metadata = a.load_snapshot(self.path, 77)
        return a.analyze(selected, metadata, 77)

    def test_literal_schema_matches_native_writer(self):
        source = (ROOT / "src/input/native_input_ownership.cpp").read_text(encoding="utf-8")
        block = source.split("constexpr char kHeader[] =", 1)[1].split(";", 1)[0]
        literals = re.findall(r'"(?:[^"\\]|\\.)*"', block)
        header = "".join(json.loads(value) for value in literals).strip()
        self.assertEqual(len(a.FIELDS), 75)
        self.assertEqual(header.split(","), a.FIELDS)

    def test_half_displacement_flow_not_hardware_certificate(self):
        report = self.report(native_row(cached_selected=1, cached_runtime=1, cached_source_usable=0,
                                        cached_armed=0, cached_state=9))
        flow = report["coordinate_flow"]
        self.assertEqual(flow["single_ray_correspondence_rows"], 1)
        self.assertEqual(flow["coordinates_and_deltas_native_pixels"]["pre_to_ray_x"]["mean"], -10)
        self.assertEqual(flow["coordinates_and_deltas_native_pixels"]["ray_to_post_distance"]["max"], 0)
        self.assertEqual(report["groups"][0]["identity"]["held_scope_bits"], 4)
        self.assertIn("usable=0,armed=0", next(iter(report["cached_selected_states_not_same_packet_proof"])))
        self.assertEqual(len(report["source"]["sha256_of_read_bytes"]), 64)
        self.assertIn("physical-report", " ".join(report["limits"]))

    def test_pid_isolation_and_precision_optical_groups(self):
        report = self.report(native_row(pid=9), native_row(), native_row(serial=21, serial_after=21, ray_serial=21,
                                  tick_ms=1300, end_tick_ms=1302, completed_tick_ms=1295, state_before=3,
                                  mode_before=2, mode_after=1, zoom_before=.35, held=0, script=1))
        self.assertEqual(report["rows"], 2)
        self.assertEqual(report["source"]["rows_scanned"], 3)
        self.assertEqual(len(report["groups"]), 2)
        self.assertEqual(report["groups"][1]["identity"]["script_mode"], 1)
        self.assertEqual(report["groups"][1]["optical_modes_after"], {"1": 1})

    def test_repeated_consumption_and_multiple_rays_excluded(self):
        report = self.report(native_row(scope_calls=2, status="repeated_scope_invocation"),
                             native_row(ray_calls=2))
        self.assertEqual(report["coordinate_flow"]["single_ray_correspondence_rows"], 0)
        self.assertEqual(report["repeated_sampled_thread_serial_keys"], 1)
        self.assertEqual(report["sampled_ray_call_counts"], {"1": 1, "2": 1})
        self.assertEqual(report["correspondence_rejection_reasons_nonexclusive"]["nonunique_scope_or_conversion"], 1)

    def test_contradictory_claim_and_serial_time_regression(self):
        report = self.report(native_row(serial_after=21), native_row(serial=19, serial_after=19, ray_serial=19,
                                  tick_ms=900, end_tick_ms=901, completed_tick_ms=1100))
        self.assertEqual(report["coordinate_flow"]["single_ray_correspondence_rows"], 0)
        flags = report["review_flags_not_runtime_faults"]
        self.assertEqual(flags["same_thread_serial_regression"], 1)
        self.assertEqual(flags["same_thread_tick_regression"], 1)
        self.assertEqual(flags["completed_status_without_single_ray_correspondence"], 2)

    def test_signed_zero_and_float_roundtrip(self):
        report = self.report(native_row(pre_x="-0", converted_x="0"))
        self.assertEqual(report["correspondence_rejection_reasons_nonexclusive"]["converted_entry_not_equal"], 1)
        self.assertTrue(a.same_float32(.1, .10000000149))
        self.assertFalse(a.same_float32(1e300, 1e300))

    def test_rejected_nan_preserved_but_never_used(self):
        report = self.report(native_row(status="entry_identity_unresolved", before_valid=0, pre_x="nan"))
        self.assertEqual(report["statuses"], {"entry_identity_unresolved": 1})
        self.assertEqual(report["coordinate_flow"]["single_ray_correspondence_rows"], 0)
        self.assertEqual(report["review_flags_not_runtime_faults"]["row_with_nonfinite_native_values"], 1)
        json.dumps(report, allow_nan=False)
        self.write(native_row(pre_x="1e300"))
        with self.assertRaises(ValueError): a.load_snapshot(self.path,77)

    def test_schema_truncation_and_unknown_status_rejected(self):
        self.write(native_row(),suffix="")
        with self.assertRaisesRegex(ValueError,"Incomplete"): a.load_snapshot(self.path,77)
        self.write(native_row(status="invented"))
        with self.assertRaisesRegex(ValueError,"Unknown status"): a.load_snapshot(self.path,77)
        self.path.write_text("schema,pid\n1,77\n",encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"header"): a.load_snapshot(self.path,77)

    def test_live_directory_and_changing_file_rejected(self):
        self.write(native_row())
        with mock.patch.object(a,"ROOT",Path(self.temporary.name)):
            runtime = Path(self.temporary.name)/"runtime"
            runtime.mkdir(); live=runtime/"x.csv"; live.write_bytes(self.path.read_bytes())
            with self.assertRaisesRegex(ValueError,"live diagnostic"): a.load_snapshot(live,77)
        original = a._fingerprint
        calls = 0
        def changing(stat):
            nonlocal calls
            calls += 1
            value=original(stat)
            return value if calls<3 else (*value[:3],value[3]+1)
        with mock.patch.object(a,"_fingerprint",side_effect=changing):
            with self.assertRaisesRegex(ValueError,"changed"): a.load_snapshot(self.path,77)

    def test_bounds_no_rows_and_examples(self):
        self.write(native_row(),native_row())
        with mock.patch.object(a,"MAX_ROWS",1):
            with self.assertRaisesRegex(ValueError,"bounded row"): a.load_snapshot(self.path,77)
        with mock.patch.object(a,"MAX_SELECTED",1):
            with self.assertRaisesRegex(ValueError,"lifetime cap"): a.load_snapshot(self.path,77)
        with mock.patch.object(a,"MAX_BYTES",1):
            with self.assertRaisesRegex(ValueError,"256 MiB"): a.load_snapshot(self.path,77)
        with self.assertRaisesRegex(ValueError,"No complete rows"): a.load_snapshot(self.path,78)
        with self.assertRaises(ValueError): a.load_snapshot(self.path,0)
        rows,meta=a.load_snapshot(self.path,77)
        self.assertEqual(a.analyze(rows,meta,77,0)["examples"],[])
        with self.assertRaises(ValueError): a.analyze(rows,meta,77,21)

    def test_growth_is_bounded_before_end_of_file(self):
        self.write(native_row())
        original_size=self.path.stat().st_size
        original_fstat=a.os.fstat
        grew=False
        def grow_after_size_check(fd):
            nonlocal grew
            before=original_fstat(fd)
            if not grew:
                grew=True
                with self.path.open("ab") as stream:
                    stream.write((",".join(native_row()[k] for k in a.FIELDS)+"\n").encode()*5)
            return before
        with mock.patch.object(a,"MAX_BYTES",original_size+10), mock.patch.object(a.os,"fstat",side_effect=grow_after_size_check):
            with self.assertRaisesRegex(ValueError,"grew beyond"): a.load_snapshot(self.path,77)

    def test_group_overflow_explicit_and_output_never_overwritten(self):
        rows,metadata = a.load_snapshot(self.write(native_row(),native_row(state_before=3)),77)
        with mock.patch.object(a,"MAX_GROUPS",1):
            report=a.analyze(rows,metadata,77)
            self.assertEqual(report["group_overflow_rows"],1)
            self.assertEqual(report["coordinate_flow"]["single_ray_correspondence_rows"],2)
        output=Path(self.temporary.name)/"report.json"
        args=["analyzer","--csv",str(self.path),"--pid","77","--output",str(output)]
        with mock.patch("sys.argv",args): a.main()
        saved=output.read_bytes()
        with mock.patch("sys.argv",args):
            with self.assertRaises(FileExistsError): a.main()
        self.assertEqual(saved,output.read_bytes())


if __name__ == "__main__":
    unittest.main()
