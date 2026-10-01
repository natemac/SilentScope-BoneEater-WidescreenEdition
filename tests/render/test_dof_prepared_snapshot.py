import csv
import io
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import dof_prepared_snapshot as probe

BASE = 0x180000000
ADDRESSES = dict(bloom=0x100000, processor=0x100000, combiner=0x200000, descriptor=0x280000,
                 camera=0x300000, parameter=0x400000, scope_target=0x500000, scope_owned=0x500000,
                 main_target=0x300000, manager_camera=0x300000)


def pair():
    row = {k: "0" for k in probe.FIELDS}
    row.update(schema="1", pid="42", sequence="9", tick_ms="9000", thread="7", phase="before", native_result="0",
               pair_identity_equal="1", status="resolved", role="main", exclusive_main="1", refs="1", width="1920",
               height="1080", caller=hex(BASE + 0x34E88D), caller_rva="0x34e88d")
    row.update({k: f"{value:016X}" for k, value in ADDRESSES.items()})
    after = dict(row, phase="after", native_result="1")
    return [row, after]


def text(rows, header=False):
    output = io.StringIO()
    writer = csv.DictWriter(output, fieldnames=probe.FIELDS)
    if header: writer.writeheader()
    writer.writerows(rows)
    return output.getvalue()


class FakeProcess:
    base = BASE
    def __init__(self):
        self.memory = {}
        self.now = 10000
        self.dimension_reads = 0
        self.change_second = False
        for address, size in [(0x100000, 0x2600), (0x200000, 0x600), (0x280000, 0x20), (0x300000, 0xF00), (0x400000, 0x100),
                              (0x500000, 0xF00), (0x600000, 0x100), (0x700000, 0x100), (0x800000, 0x400),
                              (0x900000, 0x100), (0xA00000, 0x100)]:
            self.put(address, b"\0" * size)
        self.q(BASE + 0x13DB588, 0x700000); self.q(BASE + 0x12E1930, 0x800000); self.q(BASE + 0x13DCF88, 0x900000)
        self.q(0x100000, BASE + 0x71B388); self.q(0x100000 + 0x690, 0x200000); self.q(0x200000 + 0x5B0, 0x100000)
        self.q(0x280010, 0x500000)  # The shared descriptor currently contains scope.
        self.q(0x700000, BASE + 0x10C4B60); self.q(0x700008, 0x300000); self.q(0x800340, 0x300000)
        self.q(0x300000, BASE + 0x6FDF48); self.q(0x300DF0, 0x400000); self.q(0x400000, BASE + 0x6FDEE8)
        self.put(0x400008, struct.pack("<I", 1)); self.put(0x300EB4, struct.pack("<ff", 1920, 1080))
        self.q(0x900000, BASE + 0x10C7350); self.q(0x900008, 0x500000); self.q(0x900060, 0x500000)
        self.q(0x500000, BASE + 0x6FDF48); self.q(0x500DF0, 0x600000); self.q(0x600000, BASE + 0x6FDEE8)
        self.put(0x102248, struct.pack("<22H", 1920, 1080, 960, 540, *([0] * 18)))
        self.q(0x1006B0, 0xA00000); self.put(0xA00018, struct.pack("<HH", 960, 540)); self.q(0xA00060, 0xB00000)
        self.put(0x102520, struct.pack("<H", 1))
        vertices = [(-1, 1, 0, 0, 0), (-1, -1, 0, 0, 1), (1, -1, 0, 1, 1), (1, 1, 0, 1, 0)]
        self.put(0x1023D0, b"".join(struct.pack("<7f", *v, 1/960, 1/540) for v in vertices))

    def put(self, address, data):
        self.memory.update({address + i: byte for i, byte in enumerate(data)})
    def q(self, address, value): self.put(address, struct.pack("<Q", value))
    def tick(self): return self.now
    def u64(self, address): return struct.unpack("<Q", self.read(address, 8))[0]
    def u32(self, address): return struct.unpack("<I", self.read(address, 4))[0]
    def read(self, address, length):
        if address == 0x102248:
            self.dimension_reads += 1
            if self.change_second and self.dimension_reads == 2:
                self.put(address, struct.pack("<H", 1900))
        try: return bytes(self.memory[address + i] for i in range(length))
        except KeyError: raise OSError("Unreadable fixture address")


class SnapshotTests(unittest.TestCase):
    def selected(self, rows=None, now=10000):
        return probe.select_pair(probe.FIELDS, io.StringIO(text(rows or pair())), 42, now)

    def test_actual_native_writer_header(self):
        # Captured from schema1's native writer; deliberately independent of FIELDS.
        header = next(csv.reader((Path(__file__).parent / "fixtures/dof-probe-v1-header.csv").read_text().splitlines()))
        self.assertEqual(len(header), 80)
        self.assertIn("effective_focal", header)
        self.assertEqual(probe.select_pair(header, io.StringIO(text(pair())), 42, 10000)["sequence"], "9")

    def test_pair_freshness_and_pid(self):
        self.assertEqual(self.selected()["sequence"], "9")
        for now in (8999, 11001):
            with self.subTest(now=now), self.assertRaises(ValueError): self.selected(now=now)
        rows = pair()
        for row in rows: row["pid"] = "43"
        with self.assertRaises(ValueError): self.selected(rows)

    def test_schema_pair_identity_and_duplicate_rejection(self):
        with self.assertRaises(ValueError): probe.select_pair(probe.FIELDS[:-1], [], 42, 10000)
        for field, value in [("camera", "0000000000500000"), ("tick_ms", "9001"), ("caller", "1")]:
            rows = pair(); rows[1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError): self.selected(rows)
        with self.assertRaises(ValueError): self.selected(pair() + [pair()[0]])
        with self.assertRaises(ValueError): self.selected([pair()[0]])

    def test_bounded_tail_partial_writer_and_other_pid(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "dof.csv"
            unrelated = pair()
            for row in unrelated: row["pid"] = "43"
            path.write_text(text(unrelated, True) + text(unrelated) * 500 + text(pair()) + "1,42,partial")
            self.assertGreater(path.stat().st_size, probe.TAIL_BYTES)
            self.assertEqual(probe.read_pair(path, 42, 10000)["sequence"], "9")

    def test_valid_read_only_snapshot(self):
        reader = FakeProcess(); original = dict(reader.memory)
        result = probe.capture(reader, self.selected())
        self.assertTrue(result["repeated_identity_and_metadata_equal"])
        self.assertEqual(result["prepared"]["textures"][0]["width"], 960)
        self.assertEqual(result["prepared"]["quad_count"], 1)
        self.assertEqual(len(result["prepared"]["vertices"]), 4)
        self.assertEqual(result["shared_descriptor_camera_at_read"], "0x500000")
        self.assertFalse(result["descriptor_camera_is_ownership_guard"])
        self.assertEqual(reader.memory, original)
        # Backend address is unmapped; the successful test proves it was not dereferenced.
        self.assertNotIn(0xB00000, reader.memory)

    def test_owner_camera_scope_and_module_guard_rejections(self):
        for pointer, value in [(0x100000, BASE), (0x2005B0, 0x500000), (0x700008, 0x500000),
                               (0x800340, 0x500000), (0x500DF0, 0x400000), (0x900060, 0x300000)]:
            reader = FakeProcess(); reader.q(pointer, value)
            with self.subTest(pointer=hex(pointer)), self.assertRaises(ValueError): probe.capture(reader, self.selected())
        row = self.selected(); row["caller"] = hex(BASE + 0x10000 + 0x34E88D)
        with self.assertRaises(ValueError): probe.capture(FakeProcess(), row)

    def test_metadata_bounds_and_nonfinite_rejections(self):
        for pointer, data in [(0x102520, struct.pack("<H", 4)), (0x102520, b"\0\0"),
                              (0xA00018, struct.pack("<HH", 0, 540)), (0x102248, struct.pack("<H", 40000)),
                              (0x102274, b"\1"), (0x1023D0, struct.pack("<f", math.nan))]:
            reader = FakeProcess(); reader.put(pointer, data)
            with self.subTest(pointer=hex(pointer)), self.assertRaises(ValueError): probe.capture(reader, self.selected())

    def test_changed_metadata_and_unreadable_pointer_rejected(self):
        reader = FakeProcess(); reader.change_second = True
        with self.assertRaisesRegex(ValueError, "changed during"): probe.capture(reader, self.selected())
        reader = FakeProcess(); reader.q(0x1006B0, 0xD00000)
        with self.assertRaises(OSError): probe.capture(reader, self.selected())


if __name__ == "__main__":
    unittest.main()
