import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('package', Path(__file__).resolve().parents[2] / 'tools/build_beta_package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class PackageTests(unittest.TestCase):
    def test_player_allowlist_excludes_game_data_and_user_state(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ('Play Bone Eater.exe', 'candidate.exe', 'runtime/game/desktop/title.png',
                         'vendor/spice2x/src/spice2x/LICENSE', 'vendor/spice2x/src/spice2x/licenses.txt',
                         'release/README.md', 'release/KNOWN_ISSUES.md', 'release/BUG_REPORT.md',
                         'runtime/game/modules/gamendd.dll', 'runtime/game/desktop/bone-eater-controls.xml',
                         'runtime/game/conf/nvram/testmode-v.xml', 'runtime/game/minidump.dmp',
                         'launch-settings.json'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'fixture')
            payload = package.player_payload(root, 'candidate.exe', package.digest(b'fixture'))
            self.assertNotIn('runtime/game/modules/gamendd.dll', payload)
            self.assertNotIn('runtime/game/desktop/bone-eater-controls.xml', payload)
            self.assertNotIn('runtime/game/conf/nvram/testmode-v.xml', payload)
            self.assertNotIn('runtime/game/minidump.dmp', payload)
            self.assertIn(b'"input_profile": null', payload['launch-settings.json'])
            with self.assertRaises(ValueError):
                package.player_payload(root, 'candidate.exe', '0' * 64)

    def test_archive_is_verified_and_existing_artifact_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'test.zip'
            package.write_archive(path, {'nested/test.txt': b'checked bytes'})
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                package.write_archive(path, {'different.txt': b'wrong'})
            self.assertEqual(before, path.read_bytes())
            with zipfile.ZipFile(path) as archive:
                self.assertEqual(archive.read('nested/test.txt'), b'checked bytes')

if __name__ == '__main__':
    unittest.main()
