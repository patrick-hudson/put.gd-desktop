"""Release collection rejects bytes/source mismatches before exposing any output."""
import hashlib
import io
import tarfile
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import collect
import proton_sources
from stage_artifacts import stage
from source_archive import create_archive


class ReleaseCollection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgrel-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'CMakeLists.txt').write_text('project(putgd VERSION 0.1.1 LANGUAGES C CXX)\n')
        (self.root / '.gitignore').write_text('/build/\n')
        (self.root / 'LICENSE').write_text('synthetic license fixture\n')
        (self.root / 'CMakePresets.json').write_text('{}\n')
        (self.root / 'THIRD_PARTY_NOTICES.md').write_text('synthetic notices fixture\n')
        for name in ('src/main.cpp', 'cmake/Dependencies.cmake', 'packaging/linux/CMakeLists.txt'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('// synthetic compilation-closure fixture\n')
        extension = self.root / 'integrations/gnome'
        extension.mkdir(parents=True)
        (extension / 'extension.js').write_text('// synthetic extension fixture\n')
        (extension / 'metadata.json').write_text('{}\n')
        bundle = self.root / 'build/source-transport.tar.gz'
        bundle.parent.mkdir()
        source_data = b'preferred source byte fixture'
        with tarfile.open(bundle, 'w:gz') as archive:
            info = tarfile.TarInfo('helper-source.tar.gz')
            info.size = len(source_data)
            archive.addfile(info, io.BytesIO(source_data))
        data = bundle.read_bytes()
        halves = (data[:len(data) // 2], data[len(data) // 2:])
        self.proton_catalog = self.root / 'tools/proton/notices/SOURCES.json'
        self.proton_catalog.parent.mkdir(parents=True)
        catalog = {'schemaVersion': 1, 'materialsReady': True, 'helperSha256': 'a' * 64,
            'delivery': {'file': proton_sources.BUNDLE_NAME, 'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
                'parts': [{'file': name, 'bytes': len(part), 'sha256': hashlib.sha256(part).hexdigest()}
                          for name, part in zip(proton_sources.PART_NAMES, halves)]},
            'artifacts': [{'filename': 'helper-source.tar.gz', 'bytes': len(source_data),
                           'sha256': hashlib.sha256(source_data).hexdigest()}]}
        self.proton_catalog.write_text(json.dumps(catalog))
        self.proton_inventory = {'runtimeSha256': catalog['helperSha256'],
            'sourceCatalogSha256': hashlib.sha256(self.proton_catalog.read_bytes()).hexdigest()}
        commands = [['git', 'init', '-q', str(self.root)],
                    ['git', '-C', str(self.root), 'add', '.'],
                    ['git', '-C', str(self.root), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'],
                    ['git', '-C', str(self.root), 'tag', 'v0.1.1-rc.1']]
        for command in commands:
            subprocess.run(command, check=True)
        self.release = collect.verify(self.root, 'v0.1.1-rc.1')
        identity = {field: self.release[field] for field in ('version', 'baseVersion', 'packageVersion', 'gitCommit', 'gitTree', 'commitCount')}
        identity.update(schemaVersion=1, dirty=False, sourceProvenance='git')
        self.inputs = self.root / 'build/inputs'
        for distro, kind, suffix, arch in [('ubuntu24.04', 'DEB', '.deb', 'amd64'), ('ubuntu26.04', 'DEB', '.deb', 'amd64'), ('fedora44', 'RPM', '.rpm', 'x86_64')]:
            directory = self.inputs / ('release-native-' + distro)
            directory.mkdir(parents=True)
            path = directory / ('synthetic' + suffix)
            path.write_bytes(b'byte-integrity fixture, not a native package')
            receipt = {'manifest': dict(identity), 'proton': self.proton_inventory, 'package': {'format': kind, 'architecture': arch, 'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size': path.stat().st_size}}
            (directory / 'native-audit.json').write_text(json.dumps(receipt))
        directory = self.inputs / 'release-appimage'
        directory.mkdir()
        path = directory / 'synthetic.AppImage'
        path.write_bytes(b'byte-integrity fixture, not an AppImage')
        image_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        runtime = {'ffmpeg': {'commit': 'f' * 40}, 'gplComponents': [
            {'file': 'libx264.so.164', 'binaryPackage': 'libx264-164', 'binaryVersion': '2:0.164', 'sourcePackage': 'x264', 'sourceVersion': '2:0.164', 'publishSource': True}]}
        (directory / 'appimage-build.json').write_text(json.dumps({'format': 'AppImage', 'architecture': 'x86_64', 'build': identity, 'postDeploymentAudit': {'proton': self.proton_inventory}, 'recordingRuntime': runtime, 'artifact': {'file': path.name, 'sha256': image_sha, 'bytes': path.stat().st_size}}))
        media = directory / 'synthetic-appimage-media-source.tar.gz'
        media.write_bytes(b'byte-integrity fixture, not a source archive')
        (directory / 'appimage-media-source.json').write_text(json.dumps({'schemaVersion': 1, 'format': 'appimage-media-source',
            'appImage': {'version': identity['version'], 'sha256': image_sha}, 'file': media.name,
            'sha256': hashlib.sha256(media.read_bytes()).hexdigest(), 'bytes': media.stat().st_size, 'ffmpegCommit': 'f' * 40,
            'ubuntuSources': [{'sourcePackage': 'x264', 'sourceVersion': '2:0.164'}]}))
        directory = self.inputs / 'release-native-ubuntu24.04'
        archive = directory / ('putgd-' + identity['version'] + '-source.tar.gz')
        self.source_receipt = create_archive(self.root, archive, identity)
        (directory / 'source-archive.json').write_text(json.dumps(self.source_receipt))
        compiled = {**identity, 'dirty': None, 'sourceProvenance': 'supplied',
                    'version': self.source_receipt['localBuild']['version'],
                    'packageVersion': identity['baseVersion'] + '~dev.' + str(identity['commitCount']) + '+g' + identity['gitCommit'][:12]}
        self.source_result = {'passed': True, 'compiledBuild': compiled,
                              'rebuiltProton': {'sha256': catalog['helperSha256'], 'bundledSha256': catalog['helperSha256']},
                              'identityMatchedPrograms': ['putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker']}
        (directory / 'source-build-results.json').write_text(json.dumps(self.source_result))
        proton_sources.prepare(self.proton_catalog, self.inputs / 'proton-sources', bundle)
        self.output = self.root / 'build/output'

    def git_show_commit_time(self):
        return subprocess.check_output(['git', '-C', str(self.root), 'show', '-s', '--format=%ct',
                                        self.release['gitCommit']], text=True).strip()

    def run_collection(self):
        return collect.collect(self.root, self.inputs, self.output, 'v0.1.1-rc.1')

    def test_collects_unique_distro_assets_and_checksums(self):
        self.assertEqual(self.run_collection(), self.release)
        files = {path.name for path in self.output.iterdir()}
        self.assertIn('putgd-0.1.1-rc.1-ubuntu24.04-x86_64.deb', files)
        self.assertIn('putgd-0.1.1-rc.1-ubuntu26.04-x86_64.deb', files)
        lines = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertIn('putgd-0.1.1-rc.1-source.tar.gz', files)
        self.assertIn('putgd-0.1.1-rc.1-fedora44-x86_64.rpm', files)
        self.assertIn('putgd-0.1.1-rc.1-x86_64.AppImage', files)
        self.assertIn('putgd-0.1.1-rc.1-appimage-media-source.tar.gz', files)
        self.assertEqual(len(lines), 12)
        self.assertEqual((self.output / 'putgd-0.1.1-rc.1-source.tar.gz').read_bytes(),
                         (self.inputs / 'release-native-ubuntu24.04' / self.source_receipt['artifact']['file']).read_bytes())
        combined = self.output / 'putgd-0.1.1-rc.1-release.zip'
        self.assertTrue(combined.is_file())
        commit_epoch = int(self.git_show_commit_time())
        utc = datetime.fromtimestamp(commit_epoch, timezone.utc)
        expected_time = (utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second - utc.second % 2)
        with zipfile.ZipFile(combined) as archive:
            self.assertTrue(all(item.date_time == expected_time for item in archive.infolist()))
            underlying = files - {combined.name, 'SHA256SUMS', *proton_sources.PART_NAMES}
            self.assertEqual(set(archive.namelist()), underlying | {'SHA256SUMS'})
            for line in archive.read('SHA256SUMS').decode().splitlines():
                digest, name = line.split('  ', 1)
                self.assertIn(name, underlying)
                self.assertEqual(archive.read(name), (self.output / name).read_bytes())
                self.assertEqual(digest, hashlib.sha256(archive.read(name)).hexdigest())
        with zipfile.ZipFile(self.output / 'putgd-gnome50-0.1.1-rc.1.zip') as archive:
            self.assertTrue(all(item.date_time == expected_time for item in archive.infolist()))
        for line in lines:
            digest, name = line.split('  ', 1)
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_repeat_collection_is_identical_despite_checkout_mtimes(self):
        self.run_collection()
        for name in ('LICENSE', 'integrations/gnome/extension.js', 'integrations/gnome/metadata.json'):
            os.utime(self.root / name, (1234567890, 1234567890))
        second = self.root / 'build/second'
        collect.collect(self.root, self.inputs, second, 'v0.1.1-rc.1')
        for path in self.output.iterdir():
            self.assertEqual(path.read_bytes(), (second / path.name).read_bytes(), path.name)

    def test_native_artifact_staging_is_flat_and_allowlisted(self):
        build = self.root / 'build/native-staging'
        packages = build / 'release-packages'
        packages.mkdir(parents=True)
        for name in ('native-audit.json', 'build-info.json', 'cpack.log', 'sample.deb'):
            (packages / name).write_bytes(name.encode())
        (build / 'release-tag.json').write_bytes(b'tag')
        (packages / 'unrelated.txt').write_bytes(b'excluded')
        output = build / 'artifact'
        stage(build, output, 'native')
        self.assertEqual({p.name for p in output.iterdir()},
                         {'native-audit.json', 'build-info.json', 'cpack.log', 'sample.deb', 'release-tag.json'})
        self.assertEqual((output / 'sample.deb').read_bytes(), b'sample.deb')
        self.assertFalse((output / 'release-packages').exists())

    def test_source_staging_retains_exact_archive_and_build_evidence_only(self):
        build = self.root / 'build/source-staging'
        export = build / 'source-export'
        export.mkdir(parents=True)
        for name in ('source-archive.json', 'putgd-0.1.1-rc.1-source.tar.gz', 'private-context.txt'):
            (export / name).write_bytes(name.encode())
        compile_dir = build / 'source-build'
        compile_dir.mkdir()
        for name in ('results.json', 'parameters.json', 'build.log', 'private-context.txt'):
            (compile_dir / name).write_bytes(name.encode())
        output = build / 'artifact'
        stage(build, output, 'native')
        self.assertEqual({p.name for p in output.iterdir()}, {'source-archive.json',
            'putgd-0.1.1-rc.1-source.tar.gz', 'source-build-results.json', 'source-build-parameters.json', 'source-build.log'})
        self.assertEqual((output / 'source-build-results.json').read_bytes(), b'results.json')

    def test_corresponding_source_parts_and_exact_catalog_are_public(self):
        self.run_collection()
        for name in (*proton_sources.PART_NAMES, proton_sources.CATALOG_NAME):
            self.assertEqual((self.output / name).read_bytes(), (self.inputs / 'proton-sources' / name).read_bytes())
        with zipfile.ZipFile(self.output / 'putgd-0.1.1-rc.1-release.zip') as archive:
            self.assertIn(proton_sources.CATALOG_NAME, archive.namelist())
            self.assertTrue(set(proton_sources.PART_NAMES).isdisjoint(archive.namelist()))

    def test_corrupt_source_transport_rejected_before_output(self):
        path = self.inputs / 'proton-sources' / proton_sources.PART_NAMES[0]
        path.write_bytes(path.read_bytes() + b'bad')
        with self.assertRaisesRegex(ValueError, 'bundle bytes'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_other_source_catalog_rejected_before_output(self):
        path = self.inputs / 'proton-sources' / proton_sources.CATALOG_NAME
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'catalog differs'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_appimage_missing_proton_inventory_rejected_before_output(self):
        path = self.inputs / 'release-appimage/appimage-build.json'
        receipt = json.loads(path.read_text())
        del receipt['postDeploymentAudit']['proton']
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'Proton package'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_other_helper_or_catalog_package_inventory_rejected_before_output(self):
        path = self.inputs / 'release-native-fedora44/native-audit.json'
        original = json.loads(path.read_text())
        for field in ('runtimeSha256', 'sourceCatalogSha256'):
            receipt = json.loads(json.dumps(original))
            receipt['proton'][field] = '0' * 64
            path.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, 'Proton package'):
                self.run_collection()
            self.assertFalse(self.output.exists())

    def test_missing_or_mismatched_rebuilt_proton_rejected_before_output(self):
        path = self.inputs / 'release-native-ubuntu24.04/source-build-results.json'
        for index, rebuilt in enumerate((None, {'sha256': '0' * 64, 'bundledSha256': 'a' * 64},
                        {'sha256': 'a' * 64, 'bundledSha256': '0' * 64})):
            with self.subTest(rebuilt=rebuilt):
                self.output = self.root / ('output-rebuild-' + str(index))
                result = {**self.source_result}
                result.pop('rebuiltProton')
                if rebuilt is not None:
                    result['rebuiltProton'] = rebuilt
                path.write_text(json.dumps(result))
                with self.assertRaisesRegex(ValueError, 'rebuilt Proton'):
                    self.run_collection()
                self.assertFalse(self.output.exists())

    def test_corrupt_source_archive_rejected_before_output(self):
        archive = self.inputs / 'release-native-ubuntu24.04' / self.source_receipt['artifact']['file']
        original = archive.read_bytes()
        archive.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.assertEqual(archive.stat().st_size, len(original))
        with self.assertRaisesRegex(ValueError, 'Source archive bytes'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_missing_source_build_receipt_rejected_before_output(self):
        (self.inputs / 'release-native-ubuntu24.04/source-build-results.json').unlink()
        with self.assertRaises(FileNotFoundError):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_failed_other_commit_or_missing_helper_source_build_rejected(self):
        path = self.inputs / 'release-native-ubuntu24.04/source-build-results.json'
        for changed in ({'passed': False}, {'compiledBuild': {**self.source_result['compiledBuild'], 'gitCommit': '0' * 40}},
                        {'identityMatchedPrograms': ['putgd']}):
            with self.subTest(changed=changed):
                path.write_text(json.dumps({**self.source_result, **changed}))
                with self.assertRaisesRegex(ValueError, 'successful production build'):
                    self.run_collection()
                self.assertFalse(self.output.exists())

    def test_appimage_staging_preserves_flat_consumer_paths(self):
        build = self.root / 'build/appimage-staging'
        packages = build / 'appimage-output'
        packages.mkdir(parents=True)
        (packages / 'appimage-build.json').write_bytes(b'receipt')
        (packages / 'sample.AppImage').write_bytes(b'image')
        (packages / 'AppDir').mkdir()
        (packages / 'AppDir/internal.txt').write_bytes(b'excluded')
        output = build / 'artifact'
        stage(build, output, 'appimage')
        self.assertEqual({p.name for p in output.iterdir()}, {'appimage-build.json', 'sample.AppImage'})
        self.assertEqual((output / 'sample.AppImage').read_bytes(), b'image')

    def test_corrupt_bytes_rejected_before_output(self):
        path = self.inputs / 'release-native-ubuntu24.04/synthetic.deb'
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.assertEqual(path.stat().st_size, len(original))
        with self.assertRaisesRegex(ValueError, 'bytes'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_same_version_other_source_rejected_before_output(self):
        path = self.inputs / 'release-appimage/appimage-build.json'
        receipt = json.loads(path.read_text())
        receipt['build']['gitCommit'] = '0' * 40
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'identity'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_changed_package_version_rejected(self):
        path = self.inputs / 'release-native-fedora44/native-audit.json'
        receipt = json.loads(path.read_text())
        receipt['manifest']['packageVersion'] = '0.1.1'
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'identity'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def nightly_inputs(self):
        # U1 covers exact Git-tag verification; this fixture starts at its verified
        # identity boundary and tests the real collector/archive byte pipeline.
        tag = f"v0.1.1-nightly.{self.release['commitCount']}"
        version = tag[1:]
        self.release.update(tag=tag, version=version, packageVersion=version.replace('-nightly.', '~nightly.'), channel='nightly')
        for receipt_path in self.inputs.glob('*/native-audit.json'):
            receipt = json.loads(receipt_path.read_text())
            receipt['manifest'].update(version=version, packageVersion=self.release['packageVersion'])
            receipt_path.write_text(json.dumps(receipt))
        receipt_path = self.inputs / 'release-appimage/appimage-build.json'
        receipt = json.loads(receipt_path.read_text())
        receipt['build'].update(version=version, packageVersion=self.release['packageVersion'])
        receipt_path.write_text(json.dumps(receipt))
        directory = self.inputs / 'release-native-ubuntu24.04'
        archive = directory / ('putgd-' + version + '-source.tar.gz')
        self.source_receipt = create_archive(self.root, archive, receipt['build'])
        (directory / 'source-archive.json').write_text(json.dumps(self.source_receipt))
        return tag, archive

    def test_nightly_cli_preserves_all_thirteen_raw_assets_and_exact_compiled_source(self):
        tag, source = self.nightly_inputs()
        (self.inputs / 'credentials.txt').write_text('synthetic excluded fixture')
        argv = ['collect.py', '--tag', tag, '--root', str(self.root),
                '--inputs', str(self.inputs), '--output', str(self.output)]
        with patch.object(sys, 'argv', argv), patch('builtins.print'), \
             patch.object(collect, 'verify', return_value=self.release):
            collect.main()
        files = {path.name for path in self.output.iterdir()}
        from release_bundle import expected_members
        expected = set(expected_members(self.release['version']) +
                       ['putgd-' + self.release['version'] + '-release.zip', 'SHA256SUMS', *proton_sources.PART_NAMES])
        self.assertEqual(files, expected)
        self.assertEqual(len(files), 13)
        self.assertEqual((self.output / source.name).read_bytes(), source.read_bytes())
        builds = json.loads((self.output / 'release-builds.json').read_text())
        self.assertEqual(builds['release']['channel'], 'nightly')
        self.assertEqual(builds['receipts']['sourceBuild'], self.source_result)
        self.assertEqual(builds['receipts']['sourceBuild']['compiledBuild']['sourceProvenance'], 'supplied')
        self.assertNotEqual(builds['receipts']['sourceBuild']['compiledBuild']['version'], self.release['version'])
        with zipfile.ZipFile(self.output / ('putgd-' + self.release['version'] + '-release.zip')) as archive:
            for name in expected - {'SHA256SUMS', 'putgd-' + self.release['version'] + '-release.zip', *proton_sources.PART_NAMES}:
                self.assertEqual(archive.read(name), (self.output / name).read_bytes())
            self.assertNotIn('credentials.txt', archive.namelist())
        for line in (self.output / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ', 1)
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())
        second = self.root / 'build/nightly-repeat'
        with patch.object(collect, 'verify', return_value=self.release):
            collect.collect(self.root, self.inputs, second, tag)
        for path in self.output.iterdir():
            self.assertEqual(path.read_bytes(), (second / path.name).read_bytes(), path.name)

    def test_nightly_failed_source_compile_and_mixed_package_source_reject_before_output(self):
        tag, _ = self.nightly_inputs()
        receipt_path = self.inputs / 'release-native-ubuntu24.04/source-build-results.json'
        receipt_path.write_text(json.dumps({**self.source_result, 'passed': False}))
        with patch.object(collect, 'verify', return_value=self.release):
            with self.assertRaisesRegex(ValueError, 'successful production build'):
                collect.collect(self.root, self.inputs, self.output, tag)
            self.assertFalse(self.output.exists())
            receipt_path.write_text(json.dumps(self.source_result))
            receipt_path = self.inputs / 'release-native-fedora44/native-audit.json'
            receipt = json.loads(receipt_path.read_text())
            receipt['manifest']['gitCommit'] = '0' * 40
            receipt_path.write_text(json.dumps(receipt))
            with self.assertRaisesRegex(ValueError, 'identity'):
                collect.collect(self.root, self.inputs, self.output, tag)
            self.assertFalse(self.output.exists())



if __name__ == '__main__':
    unittest.main()
