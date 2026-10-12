"""Release collection rejects bytes/source mismatches before exposing any output."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import collect
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
            receipt = {'manifest': dict(identity), 'package': {'format': kind, 'architecture': arch, 'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size': path.stat().st_size}}
            (directory / 'native-audit.json').write_text(json.dumps(receipt))
        directory = self.inputs / 'release-appimage'
        directory.mkdir()
        path = directory / 'synthetic.AppImage'
        path.write_bytes(b'byte-integrity fixture, not an AppImage')
        (directory / 'appimage-build.json').write_text(json.dumps({'format': 'AppImage', 'architecture': 'x86_64', 'build': identity, 'artifact': {'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes': path.stat().st_size}}))
        directory = self.inputs / 'release-native-ubuntu24.04'
        archive = directory / ('putgd-' + identity['version'] + '-source.tar.gz')
        self.source_receipt = create_archive(self.root, archive, identity)
        (directory / 'source-archive.json').write_text(json.dumps(self.source_receipt))
        compiled = {**identity, 'dirty': None, 'sourceProvenance': 'supplied',
                    'version': self.source_receipt['localBuild']['version'],
                    'packageVersion': identity['baseVersion'] + '~dev.' + str(identity['commitCount']) + '+g' + identity['gitCommit'][:12]}
        self.source_result = {'passed': True, 'compiledBuild': compiled,
                              'identityMatchedPrograms': ['putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker']}
        (directory / 'source-build-results.json').write_text(json.dumps(self.source_result))
        self.output = self.root / 'build/output'

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
        self.assertEqual(len(lines), 7)
        self.assertEqual((self.output / 'putgd-0.1.1-rc.1-source.tar.gz').read_bytes(),
                         (self.inputs / 'release-native-ubuntu24.04' / self.source_receipt['artifact']['file']).read_bytes())
        for line in lines:
            digest, name = line.split('  ', 1)
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

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


if __name__ == '__main__':
    unittest.main()
