"""Installed cache corruption and environment mismatch must fail before compilation."""
import importlib.util
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

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


    def test_package_inventory_order_does_not_change_the_key(self):
        def key(root, listing):
            def run(command, **kwargs):
                output = listing if command[0] == 'rpm' else command[0] + ' fixture\n'
                return subprocess.CompletedProcess(command, 0, output, '')
            with patch.object(cache.platform, 'system', return_value='Linux'), \
                 patch.object(cache.shutil, 'which', return_value=None), \
                 patch.object(cache.subprocess, 'run', side_effect=run):
                return cache.fingerprint(root)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('cmake/dependency-lock.json', 'tools/ci/sdk_cache.py', 'tools/ci/sdk-source.sh',
                         'tools/releases/provision-sdk.sh'):
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(name)
            installed = key(root, 'a-1.fc44.x86_64\nb-2.fc44.x86_64\nc-3.fc44.x86_64\n')
            self.assertEqual(key(root, 'c-3.fc44.x86_64\na-1.fc44.x86_64\nb-2.fc44.x86_64\n'), installed)
            self.assertNotEqual(key(root, 'a-1.fc44.x86_64\nb-2.fc44.x86_64\nc-4.fc44.x86_64\n'), installed)


if __name__ == '__main__':
    unittest.main()
