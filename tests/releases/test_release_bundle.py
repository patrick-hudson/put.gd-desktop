"""Release bundle contracts with synthetic bytes; no package/desktop proof."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import release_bundle

VERSION = '0.1.1-rc.1'
EPOCH = 1791830401


class Bundle(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgbundle-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.first = self.root / 'first'; self.first.mkdir()
        self.names = [
            'putgd-0.1.1-rc.1-ubuntu24.04-x86_64.deb',
            'putgd-0.1.1-rc.1-ubuntu26.04-x86_64.deb',
            'putgd-0.1.1-rc.1-fedora44-x86_64.rpm',
            'putgd-0.1.1-rc.1-x86_64.AppImage',
            'putgd-0.1.1-rc.1-appimage-media-source.tar.gz',
            'putgd-0.1.1-rc.1-source.tar.gz',
            'putgd-gnome50-0.1.1-rc.1.zip', 'release-builds.json', 'proton-source-catalog.json',
        ]
        for name in self.names:
            (self.first / name).write_bytes(('synthetic bytes: ' + name).encode())
        (self.first / 'release-builds.json').write_text(json.dumps({'release':{'version':VERSION}}))
        (self.first / 'credentials.txt').write_text('synthetic excluded fixture')
        (self.first / 'work-context.txt').write_text('synthetic excluded context')
        self.output = self.first / 'putgd-0.1.1-rc.1-release.zip'

    def build(self, names=None):
        return release_bundle.create_release_bundle(self.first, self.output, VERSION, EPOCH,
                                                     self.names if names is None else names)

    def test_explicit_inventory_inner_hashes_and_raw_public_files(self):
        receipt = self.build()
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.namelist(), sorted(self.names + ['SHA256SUMS']))
            listed = {}
            for line in archive.read('SHA256SUMS').decode().splitlines():
                digest, name = line.split('  ', 1); listed[name] = digest
            self.assertEqual(set(listed), set(self.names))
            for name in self.names:
                self.assertEqual(archive.read(name), (self.first / name).read_bytes())
                self.assertEqual(listed[name], hashlib.sha256(archive.read(name)).hexdigest())
                self.assertTrue((self.first / name).is_file())
            self.assertNotIn(self.output.name, listed)
            for m in receipt['members']:
                self.assertEqual(m['sha256'], hashlib.sha256(archive.read(m['path'])).hexdigest())
        self.assertEqual(receipt['artifact']['sha256'], hashlib.sha256(self.output.read_bytes()).hexdigest())

    def test_repeated_creation_ignores_source_mtime_mode_order(self):
        first = self.build()
        second = self.root / 'second'; second.mkdir()
        for name in self.names:
            p = second / name; p.write_bytes((self.first / name).read_bytes()); p.chmod(0o600)
            os.utime(p, (1234567890, 1234567890))
        output = second / self.output.name
        other = release_bundle.create_release_bundle(second, output, VERSION, EPOCH, list(reversed(self.names)))
        self.assertEqual(self.output.read_bytes(), output.read_bytes())
        self.assertEqual(first['artifact']['sha256'], other['artifact']['sha256'])

    def test_safe_stored_members_modes_and_timestamp(self):
        self.build()
        with zipfile.ZipFile(self.output) as archive:
            for item in archive.infolist():
                self.assertEqual(item.compress_type, zipfile.ZIP_STORED)
                self.assertEqual(item.create_system, 3)
                self.assertEqual(item.date_time, (2026, 10, 12, 18, 40, 0))
                mode = item.external_attr >> 16
                self.assertEqual(mode & 0o777, 0o755 if item.filename.endswith('.AppImage') else 0o644)
                self.assertEqual(item.extra, b'')

    def test_distinct_epoch_controls_every_member_timestamp(self):
        self.build()
        other = self.root / 'other'
        other.mkdir()
        for name in self.names:
            (other / name).write_bytes((self.first / name).read_bytes())
        output = other / self.output.name
        release_bundle.create_release_bundle(other, output, VERSION, EPOCH + 90061, self.names)
        with zipfile.ZipFile(output) as archive:
            for item in archive.infolist():
                self.assertEqual(item.date_time, (2026, 10, 13, 19, 41, 2))
        self.assertNotEqual(output.read_bytes(), self.output.read_bytes())

    def test_duplicate_unexpected_and_path_names_rejected_before_output(self):
        for bad in [self.names + [self.names[0]], self.names[:-1],
                    self.names[:-1] + ['credentials.txt'], self.names[:-1] + ['../release-builds.json']]:
            with self.assertRaises(ValueError): self.build(bad)
            self.assertFalse(self.output.exists())

    def test_symlink_and_existing_output_refused(self):
        target = self.first / self.names[0]; target.unlink(); target.symlink_to(self.first / self.names[1])
        with self.assertRaises(ValueError): self.build()
        self.assertFalse(self.output.exists())
        target.unlink(); target.write_bytes(b'synthetic replacement')
        self.output.write_bytes(b'keep existing output')
        with self.assertRaises(ValueError): self.build()
        self.assertEqual(self.output.read_bytes(), b'keep existing output')

    def test_output_collision_and_wrong_release_metadata_refused(self):
        with self.assertRaises(ValueError):
            release_bundle.create_release_bundle(self.first, self.first / self.names[5], VERSION, EPOCH, self.names)
        (self.first / 'release-builds.json').write_text(json.dumps({'release':{'version':'0.1.1'}}))
        with self.assertRaises(ValueError): self.build()
        self.assertFalse(self.output.exists())

    def test_crc_corruption_on_readback_rejected_and_new_output_removed(self):
        original_zip = zipfile.ZipFile
        class CorruptReadback(original_zip):
            def __init__(self, file, mode='r', *args, **kwargs):
                if mode == 'r':
                    target = Path(file)
                    contents = target.read_bytes()
                    needle = b'synthetic bytes: putgd-0.1.1-rc.1-fedora44-x86_64.rpm'
                    position = contents.index(needle)
                    contents = contents[:position] + bytes([contents[position] ^ 1]) + contents[position + 1:]
                    target.write_bytes(contents)
                super().__init__(file, mode, *args, **kwargs)
        with patch.object(release_bundle.zipfile, 'ZipFile', CorruptReadback):
            with self.assertRaisesRegex(ValueError, 'CRC verification failed'):
                self.build()
        self.assertFalse(self.output.exists())

    def test_nightly_positive_inventory_and_channel(self):
        version = '0.1.1-nightly.42'
        names = [name.replace(VERSION, version) for name in self.names]
        for old, new in zip(self.names, names):
            (self.first / new).write_bytes((self.first / old).read_bytes())
        (self.first / 'release-builds.json').write_text(json.dumps({'release': {'version': version}}))
        output = self.first / ('putgd-' + version + '-release.zip')
        receipt = release_bundle.create_release_bundle(self.first, output, version, EPOCH, names)
        self.assertEqual(release_bundle.expected_members(version), names)
        self.assertEqual(receipt['channel'], 'nightly')
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.namelist(), sorted(names + ['SHA256SUMS']))
            self.assertNotIn('credentials.txt', archive.namelist())

    def test_unrelated_or_malformed_versions_rejected(self):
        for version in ['0.1.1-dev.42+g0123456789ab', '0.1.1-beta.1',
                        '0.1.1-nightly.042', '0.1.1-nightly.0', '0.1.1-nightly',
                        '0.1.1-nightly.42+g0123456789ab',
                        '0.1.1-dev.42.20261012184001+g0123456789ab',
                        '../0.1.1', '0.1.1/other', '0.1.1-rc.01']:
            with self.subTest(version=version), self.assertRaises(ValueError):
                release_bundle.expected_members(version)

    def test_gnome_zip_deterministic_from_explicit_three_files(self):
        source = self.root / 'source'; extension = source / 'integrations/gnome'; extension.mkdir(parents=True)
        for p, data in [(extension/'extension.js', b'// synthetic extension'),
                        (extension/'metadata.json', b'{}'), (source/'LICENSE', b'synthetic license')]:
            p.write_bytes(data)
        a = self.root / 'gnome-a.zip'; b = self.root / 'gnome-b.zip'
        release_bundle.create_gnome_zip(source, a, EPOCH)
        for p in [extension/'extension.js', extension/'metadata.json', source/'LICENSE']:
            os.utime(p, (987654321, 987654321))
        release_bundle.create_gnome_zip(source, b, EPOCH)
        self.assertEqual(a.read_bytes(), b.read_bytes())
        with zipfile.ZipFile(a) as archive:
            self.assertEqual(archive.namelist(), ['LICENSE', 'extension.js', 'metadata.json'])
            for name, expected in {'LICENSE': b'synthetic license',
                                   'extension.js': b'// synthetic extension', 'metadata.json': b'{}'}.items():
                self.assertEqual(archive.read(name), expected)
                self.assertEqual(archive.getinfo(name).date_time, (2026, 10, 12, 18, 40, 0))


if __name__ == '__main__': unittest.main()
