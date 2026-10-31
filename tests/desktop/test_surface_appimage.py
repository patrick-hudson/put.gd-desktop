"""Exact-file cleanup regressions; synthetic tests are not KWin evidence."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.lab.surface_driver.appimage import sync_entries


class MirroredEntries(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)
        self.root = self.home / 'private'
        self.root.mkdir(mode=0o700)
        self.applications = self.home / '.local/share/applications'
        self.applications.mkdir(parents=True)
        self.entry = self.applications / 'putgd-appimage-owned.desktop'
        self.entry.write_bytes(b'exact synthetic entry')
        info = self.entry.stat()
        self.manifest = self.root / 'kwin-mirrors.json'
        self.manifest.write_text(json.dumps([dict(name=self.entry.name, inode=info.st_ino,
            device=info.st_dev, sha256=hashlib.sha256(self.entry.read_bytes()).hexdigest())]))

    def cleanup(self):
        with patch('pathlib.Path.home', return_value=self.home), \
             patch.dict(os.environ, PIPEWIRE_RUNTIME_DIR='/run/user/1234'), \
             patch('subprocess.run') as refresh:
            sync_entries(str(self.root), '/unused', True)
            return refresh

    def test_only_exact_owned_file_is_removed_and_native_cache_is_refreshed(self):
        unrelated = self.applications / 'putgd-appimage-other.desktop'
        unrelated.write_bytes(b'preserve')
        refresh = self.cleanup()
        self.assertFalse(self.entry.exists())
        self.assertEqual(unrelated.read_bytes(), b'preserve')
        self.assertFalse(self.manifest.exists())
        args, kwargs = refresh.call_args
        self.assertEqual(args[0], ['kbuildsycoca6', '--noincremental'])
        self.assertEqual(kwargs['env']['XDG_DATA_HOME'], str(self.applications.parent))

    def test_modified_file_is_preserved_and_cleanup_refuses(self):
        self.entry.write_bytes(b'changed')
        with self.assertRaises(AssertionError):
            self.cleanup()
        self.assertTrue(self.entry.exists())
        self.assertTrue(self.manifest.exists())

    def test_symlink_replacement_is_preserved_and_cleanup_refuses(self):
        other = self.home / 'foreign'
        other.write_bytes(self.entry.read_bytes())
        self.entry.unlink()
        self.entry.symlink_to(other)
        with self.assertRaises(AssertionError):
            self.cleanup()
        self.assertTrue(self.entry.is_symlink())
        self.assertTrue(other.exists())

    def test_manifest_cannot_escape_the_applications_directory(self):
        data = json.loads(self.manifest.read_text())
        data[0]['name'] = '../../../foreign'
        self.manifest.write_text(json.dumps(data))
        with self.assertRaises(AssertionError):
            self.cleanup()
