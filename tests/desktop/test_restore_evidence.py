"""Lossless restoration contracts using synthetic frames only."""
import gzip
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('restore_evidence', Path(__file__).resolve().parents[2] / 'tools/lab/restore_evidence.py')
restore = importlib.util.module_from_spec(spec)
spec.loader.exec_module(restore)


class RestorationContracts(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.original = self.root / 'frame.ppm'
        self.compressed = self.root / 'frame.ppm.gz'
        self.data = b'P6\n1 1\n255\n\x01\x02\x03'
        packed = gzip.compress(self.data, mtime=0)
        self.compressed.write_bytes(packed)
        self.row = {'original': str(self.original), 'compressed': str(self.compressed),
                    'originalSha256': hashlib.sha256(self.data).hexdigest(),
                    'compressedSha256': hashlib.sha256(packed).hexdigest(),
                    'originalBytes': len(self.data), 'compressedBytes': len(packed),
                    'originalMode': 0o600, 'originalMtimeNs': 1234567890000000000}

    def test_exact_restore_is_idempotent_preserves_metadata_and_archive(self):
        self.assertEqual(restore.restore(self.row, self.root), 'restored')
        self.assertEqual(self.original.read_bytes(), self.data)
        self.assertEqual(self.original.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.original.stat().st_mtime_ns, self.row['originalMtimeNs'])
        self.assertEqual(restore.restore(self.row, self.root), 'already-restored')
        self.assertTrue(self.compressed.exists())

    def test_existing_mismatched_bytes_or_metadata_are_never_overwritten(self):
        restore.restore(self.row, self.root)
        self.original.chmod(0o644)
        with self.assertRaises(ValueError): restore.restore(self.row, self.root)
        self.assertEqual(self.original.stat().st_mode & 0o777, 0o644)
        self.original.write_bytes(b'preserve this')
        with self.assertRaises(ValueError): restore.restore(self.row, self.root)
        self.assertEqual(self.original.read_bytes(), b'preserve this')

    def test_bad_digest_or_oversize_stream_leaves_no_partial_original(self):
        self.row['originalSha256'] = '0' * 64
        with self.assertRaises(ValueError): restore.restore(self.row, self.root)
        self.assertFalse(self.original.exists())
        self.assertFalse(list(self.root.glob('.restore-*')))
        self.row['originalSha256'] = hashlib.sha256(self.data).hexdigest()
        self.row['originalBytes'] -= 1
        with self.assertRaises(ValueError): restore.restore(self.row, self.root)
        self.assertFalse(self.original.exists())
        self.assertTrue(self.compressed.exists())


if __name__ == '__main__':
    unittest.main()
