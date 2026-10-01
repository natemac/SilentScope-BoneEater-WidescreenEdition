import copy
import csv
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import probe_framing as probe


class FakeAPI:
    def __init__(self, fail_y=None):
        self.state = {"analogs": {"Gun X": [0, False], "Gun Y": [0, False]},
                      "buttons": {"Scope Right": [0, False], "Gun Pressed": [0, False]}}
        self.calls = []
        self.fail_y = fail_y
        self.closed = False

    def request(self, module, function, values=None):
        self.calls.append((module, function, copy.deepcopy(values)))
        if function == "read":
            return [[name, *state] for name, state in self.state[module].items()]
        for pair in values:
            if function == "write_reset":
                self.state[module][pair[0]][1] = False
            else:
                self.state[module][pair[0]] = [float(pair[1]), True]
                if pair == ["Gun Y", self.fail_y]:
                    raise RuntimeError("Simulated failed response after input was applied")

    def close(self):
        self.closed = True


class FramingProbeTests(unittest.TestCase):
    def test_listener_decoder_port_order_and_truncation(self):
        rows = [(2, 0x0100007F, socket.htons(5730), 0, 0, 33328),
                (2, 0, socket.htons(9999), 0, 0, 100)]
        packet = struct.pack("<I", 2) + b"".join(struct.pack("<6I", *r) for r in rows)
        self.assertEqual(probe.listener_owners(packet, 5730), [(2, "127.0.0.1", 33328)])
        self.assertEqual(probe.listener_owners(packet, 1), [])
        for invalid in (b"", packet[:-1], struct.pack("<I", 0xFFFFFFFF)):
            with self.assertRaises(RuntimeError):
                probe.listener_owners(invalid, 5730)

    def test_pid_freshness_and_incomplete_rows(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "test.csv"
            path.write_text("pid,tick_ms,failed\n1,100,0\n2,10000,0\n1,11000", encoding="utf-8")
            rows = probe.read_rows(path, 1)
            self.assertEqual(len(rows), 1)
            with self.assertRaises(RuntimeError):
                probe.fresh_player(rows, 10000)
            self.assertEqual(probe.fresh_player(rows, 2600), rows[0])
            for now in (99, 2601):
                with self.assertRaises(RuntimeError):
                    probe.fresh_player(rows, now)
            with self.assertRaises(RuntimeError):
                probe.fresh_player([{"tick_ms": "100", "failed": "1"}], 100)

    def test_sample_window_has_both_bounds(self):
        rows = [{"tick_ms": str(x)} for x in (1999, 2000, 4000, 6000, 6001)]
        self.assertEqual(probe.settled_samples(rows, 2000, 6000), rows[1:4])

    def test_readback_requires_active_override_and_matching_value(self):
        api = FakeAPI()
        with patch.object(probe, "stamp", return_value={"tick_ms": 100}):
            with self.assertRaises(RuntimeError):
                probe.readback(api, .1, True)
            api.request("analogs", "write", [["Gun X", .5], ["Gun Y", .1]])
            api.request("buttons", "write", [["Scope Right", True]])
            self.assertEqual(probe.readback(api, .1, True)["buttons"][1], ["Gun Pressed", 0, False])
            api.state["analogs"]["Gun Y"][0] = float("nan")
            with self.assertRaises(RuntimeError):
                probe.readback(api, .1, True)

    def run_simulated_probe(self, output, api, events=None):
        events = [] if events is None else events
        now = [10000]
        original_request = api.request
        def request(module, operation, values=None):
            events.append(operation)
            return original_request(module, operation, values)
        def fresh_scope(*args):
            events.append("fresh_scope")
            return {"event": "reticle_draw"}
        def sleep(seconds):
            now[0] += round(seconds * 1000)
        def rows(path, pid, reader=None):
            return [{"pid": str(pid), "tick_ms": str(t), "failed": "0"}
                    for t in range(now[0] - 8000, now[0] + 1, 500)]
        with patch.object(sys, "argv", ["probe_framing", "--pid", "33328", "--output", str(output)]), \
             patch.object(probe, "verify_listener", side_effect=lambda *args: events.append("verify")), \
             patch.object(probe, "fresh_gameplay", side_effect=fresh_scope), \
             patch.object(probe, "read_rows", side_effect=rows), \
             patch.object(probe, "tick_ms", side_effect=lambda: now[0]), \
             patch.object(probe.time, "sleep", side_effect=sleep), \
             patch.object(probe, "Connection", return_value=api), \
             patch.object(api, "request", side_effect=request):
            probe.main()

    def test_complete_phases_timestamps_labels_and_cleanup(self):
        api = FakeAPI()
        events = []
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "report.json"
            self.run_simulated_probe(output, api, events)
            report = json.loads(output.read_text())
        # The first input write must be preceded by ownership revalidation
        # after the potentially long fresh-scope wait, not only before it.
        first_write = events.index("write")
        fresh = events.index("fresh_scope")
        self.assertIn("verify", events[fresh + 1:first_write])
        self.assertEqual(len(report["phases"]), 6)
        self.assertNotIn("fired", report)
        self.assertFalse(report["trigger_requested"])
        self.assertFalse(report["shot_occurrence_verified"])
        self.assertTrue(report["cleanup_confirmed"])
        for phase in report["phases"]:
            start = phase["inputs_written"]["tick_ms"]
            self.assertEqual(phase["settle_until_tick_ms"], start + 2000)
            self.assertEqual(phase["planned_end_tick_ms"], start + 6000)
            self.assertTrue(all(start + 2000 <= int(row["tick_ms"]) <= start + 6000
                                for row in phase["samples"]))
        self.check_released(api)

    def test_midphase_response_failure_releases_previously_acquired_inputs(self):
        api = FakeAPI(fail_y=.9)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(RuntimeError, "failed response"):
                self.run_simulated_probe(Path(folder) / "report.json", api)
        self.check_released(api)

    def test_telemetry_coverage_failure_also_releases_owned_inputs(self):
        api = FakeAPI()
        with tempfile.TemporaryDirectory() as folder, \
             patch.object(probe.CsvTailSnapshot, "require_window", side_effect=RuntimeError("coverage gap")):
            with self.assertRaisesRegex(RuntimeError, "coverage gap"):
                self.run_simulated_probe(Path(folder) / "report.json", api)
        self.check_released(api)

    def check_released(self, api):
        self.assertTrue(api.closed)
        self.assertTrue(all(not state[1] for states in api.state.values() for state in states.values()))
        writes = [pair for _, operation, values in api.calls if operation == "write" for pair in values]
        self.assertTrue(all(pair[0] != "Gun Pressed" for pair in writes))
        resets = [pair[0] for _, operation, values in api.calls if operation == "write_reset" for pair in values]
        self.assertCountEqual(resets, ["Gun X", "Gun Y", "Scope Right"])


class CsvTailTests(unittest.TestCase):
    def test_initial_read_is_bounded_and_unchanged_poll_reads_no_bytes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.csv"
            header = b"pid,tick_ms,failed\n"
            path.write_bytes(header + b"2,1,0\n" * 10000 + b"1,5000,0\n")
            reader = probe.CsvTailSnapshot(path, 1, max_bytes=64, max_rows=3)
            self.assertEqual(reader.poll()[-1]["tick_ms"], "5000")
            self.assertLessEqual(reader.bytes_read, len(header) + 64)
            before = reader.bytes_read
            self.assertEqual(reader.poll()[-1]["tick_ms"], "5000")
            self.assertEqual(reader.bytes_read, before)
            self.assertEqual(reader.gap_generation, 0)

    def test_partial_line_pid_filter_and_append_only_reads(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.csv"
            path.write_bytes(b"pid,tick_ms,failed\n1,100,0\n")
            reader = probe.CsvTailSnapshot(path, 1)
            reader.poll()
            before = reader.bytes_read
            with path.open("ab") as stream:
                stream.write(b"1,101")
            self.assertEqual(reader.poll()[-1]["tick_ms"], "100")
            self.assertEqual(reader.bytes_read - before, 5)
            appended = b",0\n2,999999,0\n1,102,0\n"
            with path.open("ab") as stream:
                stream.write(appended)
            rows = reader.poll()
            self.assertEqual([r["tick_ms"] for r in rows], ["100", "101", "102"])
            self.assertEqual(reader.bytes_read - before, 5 + len(appended))
            self.assertEqual(reader.gap_generation, 0)

    def test_rotation_and_truncation_invalidate_phase_coverage(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.csv"
            header = b"pid,tick_ms,failed\n"
            path.write_bytes(header + b"1,100000,0\n")
            reader = probe.CsvTailSnapshot(path, 1)
            reader.poll()
            path.write_bytes(header + b"1,2,0\n")
            self.assertEqual(reader.poll()[-1]["tick_ms"], "2")
            with self.assertRaisesRegex(RuntimeError, "coverage gap"):
                reader.require_window(0, 0)
            replacement = Path(folder) / "replacement.csv"
            replacement.write_bytes(header + b"1,300000,0\n")
            os.replace(replacement, path)
            self.assertEqual(reader.poll()[-1]["tick_ms"], "300000")
            self.assertEqual(reader.gap_generation, 2)

    def test_budget_skip_and_history_overflow_cannot_silently_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.csv"
            path.write_bytes(b"pid,tick_ms,failed\n1,100,0\n")
            reader = probe.CsvTailSnapshot(path, 1, max_bytes=64, max_rows=2)
            reader.poll()
            with path.open("ab") as stream:
                stream.write(b"2,200,0\n" * 100 + b"1,300,0\n")
            before = reader.bytes_read
            self.assertEqual(reader.poll()[-1]["tick_ms"], "300")
            self.assertEqual(reader.bytes_read - before, 64)
            with self.assertRaisesRegex(RuntimeError, "coverage gap"):
                reader.require_window(200, 0)
            generation = reader.gap_generation
            with path.open("ab") as stream:
                stream.write(b"1,400,0\n1,500,0\n1,600,0\n")
            self.assertEqual(len(reader.poll()), 2)
            with self.assertRaisesRegex(RuntimeError, "coverage gap"):
                reader.require_window(350, generation)
            reader.require_window(500, generation)

    def test_schema_invalid_row_and_partial_after_rejection(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "rows.csv"
            path.write_bytes(b"pid,tick_ms,failed\n1,1,0\n")
            with self.assertRaisesRegex(RuntimeError, "schema"):
                probe.CsvTailSnapshot(path, 1, probe.PLAYER_FIELDS).poll()
            path.write_bytes(b"schema,pid,tick_ms,failed\n1,1,100,0\n2,1,200,0\n1,1,300")
            reader = probe.CsvTailSnapshot(path, 1)
            self.assertEqual(reader.poll(), [])
            self.assertEqual(reader.rejected_rows, 1)
            with path.open("ab") as stream:
                stream.write(b",0\n")
            self.assertEqual(reader.poll()[-1]["tick_ms"], "300")

    def test_scope_freshness_requires_a_new_selected_pid_row(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "scope-v3.csv"
            def append(pid, elapsed):
                with path.open("a", newline="", encoding="utf-8") as stream:
                    row = {field: "0" for field in probe.SCOPE_FIELDS}
                    row.update(pid=str(pid), elapsed_ms=str(elapsed), event="reticle_draw")
                    writer = csv.DictWriter(stream, fieldnames=probe.SCOPE_FIELDS)
                    if stream.tell() == 0:
                        writer.writeheader()
                    writer.writerow(row)
            append(1, 100)
            reader = probe.CsvTailSnapshot(path, 1, probe.SCOPE_FIELDS)
            now = [0]
            def sleep(seconds):
                now[0] += seconds
                append(2 if now[0] < .2 else 1, now[0] * 1000 + 100)
            with patch.object(probe, "CsvTailSnapshot", return_value=reader), \
                 patch.object(probe.time, "monotonic", side_effect=lambda: now[0]), \
                 patch.object(probe.time, "sleep", side_effect=sleep):
                result = probe.fresh_gameplay(Path(folder), 1)
            self.assertEqual(result["pid"], "1")
            self.assertEqual(result["elapsed_ms"], "300.0")
            self.assertEqual(now[0], .2)


if __name__ == "__main__":
    unittest.main()
