#!/usr/bin/env python3
"""AppImage GPL corresponding-source archive: contents, determinism and the collector's binding.
Synthetic FFmpeg checkout and source packages; no apt or network."""
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/releases'))
import appimage_media_sources as media  # noqa: E402


class MediaSourceArchive(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='pgmedia-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.ffmpeg = self.root / 'ffmpeg'
        self.ffmpeg.mkdir()
        (self.ffmpeg / 'configure').write_text('#!/bin/sh\n# synthetic\n')
        (self.ffmpeg / 'COPYING.GPLv2').write_text('synthetic license\n')
        for command in (['git', 'init', '-q'], ['git', 'add', '.'],
                        ['git', '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'ffmpeg']):
            subprocess.run(command, cwd=self.ffmpeg, check=True)
        self.commit = subprocess.check_output(['git', '-C', str(self.ffmpeg), 'rev-parse', 'HEAD'], text=True).strip()
        components = [
            {'file': 'libx264.so.164', 'binaryPackage': 'libx264-164', 'binaryVersion': '2:0.164', 'sourcePackage': 'x264', 'sourceVersion': '2:0.164', 'publishSource': True},
            {'file': 'libx265.so.199', 'binaryPackage': 'libx265-199', 'binaryVersion': '3.5-2', 'sourcePackage': 'x265', 'sourceVersion': '3.5-2', 'publishSource': True},
            {'file': 'libgstx264.so', 'binaryPackage': 'gstreamer1.0-plugins-ugly', 'binaryVersion': '1.24.1', 'sourcePackage': 'gst-plugins-ugly1.0', 'sourceVersion': '1.24.1', 'publishSource': True}]
        self.receipt = {'build': {'version': '0.1.1-rc.1'}, 'artifact': {'sha256': 'a' * 64},
                        'recordingRuntime': {'ffmpeg': {'repository': 'https://example.invalid/ffmpeg.git', 'release': '7.1.5', 'commit': self.commit},
                                             'media': {'ffmpeg': {'configure': ['--enable-gpl', '--enable-libx264']}},
                                             'gplComponents': components}}
        self.receipt_path = self.root / 'appimage-build.json'
        self.receipt_path.write_text(json.dumps(self.receipt))

    def fake_download(self, package, version, target):
        (target / (package + '_' + version + '.dsc')).write_text('synthetic dsc ' + package)
        (target / (package + '_' + version + '.orig.tar.xz')).write_bytes(b'synthetic orig ' + package.encode())

    def build(self, output):
        return media.build(self.receipt_path, self.ffmpeg, output, 1700000000, download=self.fake_download)

    def test_archive_carries_ffmpeg_and_each_bundled_source_package(self):
        result = self.build(self.root / 'out')
        archive = self.root / 'out' / result['file']
        self.assertEqual(result['file'], 'putgd-0.1.1-rc.1-appimage-media-source.tar.gz')
        with tarfile.open(archive) as tar:
            names = tar.getnames()
            catalog = json.load(tar.extractfile('SOURCES.json'))
            self.assertTrue(all(member.uid == 0 and member.mtime == 1700000000 for member in tar.getmembers()))
        self.assertIn('ffmpeg/ffmpeg-7.1.5.tar', names)
        for package in ('x264', 'x265', 'gst-plugins-ugly1.0'):
            self.assertTrue(any(name.startswith('ubuntu/' + package + '/') and name.endswith('.dsc') for name in names), package)
        self.assertEqual(catalog['ffmpeg']['commit'], self.commit)
        self.assertEqual(catalog['ffmpeg']['configure'], ['--enable-gpl', '--enable-libx264'])
        self.assertEqual(result['appImage']['sha256'], 'a' * 64)
        # Byte-identical when rebuilt from the same inputs.
        again = self.build(self.root / 'again')
        self.assertEqual(again['sha256'], result['sha256'])

    def test_collector_binding_rejects_another_image_or_package_set(self):
        output = self.root / 'out'
        result = self.build(output)
        self.assertEqual(media.verify(output, result, self.receipt).name, result['file'])
        other_image = json.loads(json.dumps(self.receipt)); other_image['artifact']['sha256'] = 'b' * 64
        with self.assertRaisesRegex(ValueError, 'another image'):
            media.verify(output, result, other_image)
        other_packages = json.loads(json.dumps(self.receipt))
        other_packages['recordingRuntime']['gplComponents'][0]['sourceVersion'] = '2:0.165'
        with self.assertRaisesRegex(ValueError, 'packages differ'):
            media.verify(output, result, other_packages)
        (output / result['file']).write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, 'bytes differ'):
            media.verify(output, result, self.receipt)

    def test_wrong_ffmpeg_checkout_is_refused(self):
        self.receipt['recordingRuntime']['ffmpeg']['commit'] = '0' * 40
        self.receipt_path.write_text(json.dumps(self.receipt))
        with self.assertRaisesRegex(ValueError, 'pinned commit'):
            self.build(self.root / 'out')


if __name__ == '__main__':
    unittest.main()
