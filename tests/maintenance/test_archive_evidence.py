import importlib.util
from pathlib import Path
import tempfile
from unittest.mock import patch
import unittest

spec = importlib.util.spec_from_file_location('archive', Path(__file__).resolve().parents[2] / 'tools/maintenance/archive_evidence.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ArchiveTests(unittest.TestCase):
    def test_round_trip_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'sample.log'
            content = b'synthetic evidence\n' * 10000
            source.write_bytes(content)
            source.chmod(0o600)
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            self.assertTrue(module.archive(root, 'sample.log', root / 'index.json', manifest))
            self.assertFalse(source.exists())
            module.restore(root, 'sample.log', manifest)
            self.assertEqual(source.read_bytes(), content)
            self.assertEqual(source.stat().st_mode & 0o777, 0o600)
            with self.assertRaises(ValueError):
                module.restore(root, 'sample.log', manifest)

    def test_protected_and_symlink_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'link').symlink_to('/tmp', target_is_directory=True)
            for path in ('../escape', '/tmp/escape', 'credentials.txt', 'lab-runs/disk', 'link/file'):
                with self.assertRaises(ValueError):
                    module.checked(root, path)

    def test_corruption_does_not_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'sample.log').write_bytes(b'x' * 10000)
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            module.archive(root, 'sample.log', root / 'index.json', manifest)
            (root / 'sample.log.gz').write_bytes(b'corrupt')
            with self.assertRaises(ValueError):
                module.restore(root, 'sample.log', manifest)
            self.assertFalse((root / 'sample.log').exists())

    def test_changed_source_preserves_original(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'sample.log'
            source.write_bytes(b'x' * 10000)
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            original_digest = module.digest
            def mutate_after_check(path, compressed=False):
                result = original_digest(path, compressed)
                if compressed:
                    source.write_bytes(b'new evidence')
                return result
            with patch.object(module, 'digest', side_effect=mutate_after_check):
                with self.assertRaises(ValueError):
                    module.archive(root, 'sample.log', root / 'index.json', manifest)
            self.assertEqual(source.read_bytes(), b'new evidence')
            self.assertFalse((root / 'sample.log.gz').exists())
            self.assertFalse(manifest['files'])

    def test_no_savings_preserves_original(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'small.log').write_bytes(b'tiny')
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            self.assertFalse(module.archive(root, 'small.log', root / 'index.json', manifest))
            self.assertEqual((root / 'small.log').read_bytes(), b'tiny')
            self.assertFalse((root / 'small.log.gz').exists())

    def test_manifest_failure_keeps_original(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'sample.log'
            source.write_bytes(b'x' * 10000)
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            with patch.object(module, 'save_manifest', side_effect=OSError('disk full')):
                with self.assertRaises(OSError):
                    module.archive(root, 'sample.log', root / 'index.json', manifest)
            self.assertEqual(source.read_bytes(), b'x' * 10000)
            self.assertEqual(module.digest(root / 'sample.log.gz', True), module.digest(source))

    def test_manifest_cannot_replace_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'sample.log'
            source.write_bytes(b'x' * 10000)
            manifest = {'schema': 1, 'root': str(root), 'files': {}}
            with self.assertRaises(ValueError):
                module.archive(root, 'sample.log', source, manifest)
            self.assertEqual(source.read_bytes(), b'x' * 10000)


if __name__ == '__main__':
    unittest.main()
