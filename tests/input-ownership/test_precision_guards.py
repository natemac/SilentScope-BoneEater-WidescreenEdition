"""Offline guard verification against the pinned original file; no process access."""
from pathlib import Path
import hashlib
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PrecisionGuards(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = (ROOT / "original game/Silent Scope Bone Eater/modules/gamendd.dll").read_bytes()
        cls.source = (ROOT / "src/input/native_precision_bypass.cpp").read_text(encoding="utf-8")
        pe = struct.unpack_from("<I", cls.data, 0x3C)[0]
        count = struct.unpack_from("<H", cls.data, pe + 6)[0]
        start = pe + 24 + struct.unpack_from("<H", cls.data, pe + 20)[0]
        cls.sections = [struct.unpack_from("<IIII", cls.data, start + i * 40 + 8) for i in range(count)]

    def read_rva(self, rva, size):
        for _, base, raw_size, raw in self.sections:
            if base <= rva and rva + size <= base + raw_size:
                return self.data[raw + rva - base:raw + rva - base + size]
        self.fail("RVA is not wholly inside a raw section")

    def test_pinned_hash(self):
        self.assertEqual(hashlib.sha256(self.data).hexdigest(),
                         "98de62b05925224bdda5c414f5be2c7a50fa210176708f8552e95bd81fab40a6")

    def test_all_production_guard_bytes(self):
        for name, rva, size in (("x", 0x8AE20, 68), ("y", 0x8AE70, 68),
                                ("anchor", 0xCCF53, 16), ("returning", 0xCD0AE, 17),
                                ("authored", 0xCD140, 135)):
            with self.subTest(name=name):
                match = re.search(r"constexpr std::array<unsigned char,(\d+)> " + name + r"\{\{([^}]+)\}\}", self.source)
                self.assertIsNotNone(match)
                values = bytes(int(value.strip(), 16) for value in match[2].split(","))
                self.assertEqual(int(match[1]), size)
                self.assertEqual(len(values), size)
                self.assertEqual(values, self.read_rva(rva, size))

    def test_independent_call_targets_and_returns(self):
        for call, target, returned in ((0xCCF56, 0x8AE20, 0xCCF5B),
                                      (0xCCF5E, 0x8AE70, 0xCCF63),
                                      (0xCD0B1, 0x8AE20, 0xCD0B6),
                                      (0xCD0BA, 0x8AE70, 0xCD0BF),
                                      (0xCD174, 0x8AE70, 0xCD179),
                                      (0xCD1C2, 0x196B60, 0xCD1C7)):
            with self.subTest(call=hex(call)):
                instruction = self.read_rva(call, 5)
                self.assertEqual(instruction[0], 0xE8)
                self.assertEqual(call + 5, returned)
                self.assertEqual(returned + struct.unpack_from("<i", instruction, 1)[0], target)


if __name__ == "__main__":
    unittest.main()
