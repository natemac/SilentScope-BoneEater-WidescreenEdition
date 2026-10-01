"""Byte preservation and recovery tests using disposable calibration copies."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib


SPEC = importlib.util.spec_from_file_location(
    "reset_gun_calibration", Path(__file__).resolve().parents[2] / "tools/reset_gun_calibration.py")
reset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reset)


def fixture():
    parts = [b'<?xml version="1.0" encoding="SHIFT_JIS"?>\r\n<testModeValue>',
             '<unrelated>設定</unrelated>'.encode("cp932"), b'<guncontrolleCheck>',
             b'<calibrated><factory>0</factory><current>1</current></calibrated>']
    changes = {"leftCalibrationNutX": 3378, "leftCalibrationMinX": 2706,
               "leftCalibrationMaxX": 4040, "leftCalibrationNutY": 2059,
               "leftCalibrationMinY": 546, "leftCalibrationMaxY": 3527}
    for name, value in reset.FACTORY.items():
        parts.append(f'<{name}><factory __type="s32">{value}</factory><current __type="s32"> {changes.get(name, value)} </current></{name}>\r\n'.encode())
    parts.append(b'</guncontrolleCheck><other><current>3378</current></other></testModeValue>')
    xml = b"".join(parts)
    return xml, struct.pack("<I", zlib.crc32(xml))


class ResetTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bone-calibration-test-")
        self.addCleanup(self.temporary.cleanup)
        self.workspace = Path(self.temporary.name)
        self.game = self.workspace / "runtime/game"
        (self.game / "conf/nvram").mkdir(parents=True)
        (self.game / "modules").mkdir()
        (self.game / "modules/arkndd.dll").write_bytes(b"fixture")
        self.xml, self.crc = fixture()
        self.xml_path = self.game / "conf/nvram/testmode-v.xml"
        self.crc_path = self.xml_path.with_suffix(".crc")
        self.xml_path.write_bytes(self.xml)
        self.crc_path.write_bytes(self.crc)

    def test_crc_and_exact_byte_preservation_except_current_digits(self):
        updated, crc, changes = reset.plan_reset(self.xml, self.crc)
        expected = self.xml
        for change in changes:
            name = change["field"].encode()
            old = f'<current __type="s32"> {change["before"]} </current></'.encode() + name + b'>'
            new = f'<current __type="s32"> {change["after"]} </current></'.encode() + name + b'>'
            expected = expected.replace(old, new)
        self.assertEqual(updated, expected)
        self.assertEqual(len(changes), 6)
        self.assertEqual(struct.unpack("<I", crc)[0], zlib.crc32(updated))
        self.assertIn(b'<other><current>3378</current></other>', updated)
        self.assertIn(b'<calibrated><factory>0</factory><current>1</current></calibrated>', updated)

    def test_mismatched_crc_is_refused_without_changes(self):
        self.crc_path.write_bytes(b'\0' * 4)
        with self.assertRaisesRegex(RuntimeError, 'does not match'):
            reset.apply_reset(self.game, check_stopped=lambda _game: None)
        self.assertEqual(self.xml_path.read_bytes(), self.xml)
        self.assertFalse((self.game / 'desktop').exists())

    def test_success_backs_up_pair_and_is_idempotent(self):
        backup = reset.apply_reset(self.game, check_stopped=lambda _game: None)
        self.assertEqual((backup / self.xml_path.name).read_bytes(), self.xml)
        self.assertEqual((backup / self.crc_path.name).read_bytes(), self.crc)
        self.assertEqual(zlib.crc32(self.xml_path.read_bytes()), struct.unpack('<I', self.crc_path.read_bytes())[0])
        self.assertIsNone(reset.apply_reset(self.game, check_stopped=lambda _game: None))

    def test_second_replace_failure_restores_both_original_files(self):
        original_replace = reset.os.replace
        calls = 0
        def fail_second(source, target):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError('simulated CRC replacement failure')
            return original_replace(source, target)
        with patch.object(reset.os, 'replace', side_effect=fail_second):
            with self.assertRaisesRegex(RuntimeError, 'both original files were restored'):
                reset.apply_reset(self.game, check_stopped=lambda _game: None)
        self.assertEqual(self.xml_path.read_bytes(), self.xml)
        self.assertEqual(self.crc_path.read_bytes(), self.crc)
        self.assertFalse(list(self.xml_path.parent.glob('.gun-calibration-*.tmp')))

    def test_running_game_is_refused_before_backups_or_replacements(self):
        def running(_game):
            raise RuntimeError('Game is running')
        with self.assertRaisesRegex(RuntimeError, 'Game is running'):
            reset.apply_reset(self.game, check_stopped=running)
        self.assertEqual(self.xml_path.read_bytes(), self.xml)
        self.assertFalse((self.game / 'desktop').exists())

    def test_original_folder_is_never_a_valid_cli_target(self):
        self.assertEqual(reset.validate_game_path(self.game, self.workspace), self.game)
        with self.assertRaisesRegex(RuntimeError, 'Only this workspace'):
            reset.validate_game_path(self.workspace / 'original game', self.workspace)


if __name__ == '__main__':
    unittest.main()
