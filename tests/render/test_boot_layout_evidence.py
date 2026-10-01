"""Offline evidence checks; no module is executed or changed."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("boot_evidence", ROOT / "tools/inspect_boot_layout.py")
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class BootEvidenceTests(unittest.TestCase):
    def test_unknown_binary_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "arkndd.dll"
            path.write_bytes(b"not a trusted native module")
            with self.assertRaisesRegex(ValueError, "Unverified module"):
                boot.Image(path)

    def test_pinned_ownership_and_coordinates(self):
        modules = ROOT / "runtime/game/modules"
        if not all((modules / name).exists() for name in boot.PINS):
            self.skipTest("Native modules are not distributed with the tests")
        result = boot.inspect(modules)
        self.assertEqual([x["rtti"] for x in result["owners"]],
                         [".?AVnddDiagnosisMode@@", ".?AVArkDiagnosisMode@@"])
        self.assertTrue(all(x["draw_rva"] == "0x87010" for x in result["owners"]))
        self.assertEqual(result["callbacks"][0]["game_function_rva"], "0x5590")
        self.assertEqual(result["native_coordinate_conversion"],
                         {"x_multiplier": 800., "y_multiplier": 1280., "divisor": 100.})
        self.assertEqual(result["rows"][3], {"method_rva": "0x86720", "label": "BOOKKEEPING",
                                            "label_x": 40., "status_x": 60., "y": 42.})
        image = boot.Image(modules / "arkndd.dll")
        with self.assertRaises(ValueError):
            image.offset(0xffffffffffffffff, 8)


if __name__ == "__main__":
    unittest.main()
