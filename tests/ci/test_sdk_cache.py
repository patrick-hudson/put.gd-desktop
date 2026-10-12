"""Installed cache corruption and environment mismatch must fail before compilation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('sdk_cache', Path(__file__).resolve().parents[2] / 'tools/ci/sdk_cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class CacheContract(unittest.TestCase):
    def test_installed_prefix_roundtrip_rejects_corruption_and_abi_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'sdk'
            prefix.mkdir()
            library = prefix / 'library.so'
            library.write_bytes(b'installed official SDK fixture')
            manifest = root / 'manifest.json'
            cache.seal(prefix, manifest, 'environment-a')
            cache.verify(prefix, manifest, 'environment-a')
            with self.assertRaisesRegex(ValueError, 'provenance'):
                cache.verify(prefix, manifest, 'environment-b')
            library.write_bytes(b'corrupted cache')
            with self.assertRaisesRegex(ValueError, 'integrity'):
                cache.verify(prefix, manifest, 'environment-a')

    def test_added_files_cannot_bypass_saved_inventory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            prefix = root / 'sdk'
            prefix.mkdir()
            (prefix / 'one.h').write_text('official header fixture')
            manifest = root / 'manifest.json'
            cache.seal(prefix, manifest, 'same-environment')
            (prefix / 'unexpected.h').write_text('unexpected')
            with self.assertRaisesRegex(ValueError, 'integrity'):
                cache.verify(prefix, manifest, 'same-environment')


if __name__ == '__main__':
    unittest.main()
