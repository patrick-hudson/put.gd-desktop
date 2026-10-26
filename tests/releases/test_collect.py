"""Release collection rejects bytes/source mismatches before exposing any output."""
import hashlib
import io
import shutil
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
import quiet_git  # noqa: F401  (disables git background maintenance, #99)
import appimage_update
import collect
import debug_symbols
import proton_sources
from appimage_fixture import make_appimage, make_zsync
from debug_fixture import build_tree
from stage_artifacts import stage
from source_archive import create_archive


# The fixture's one commit, 23:30 at -05:00: October 22 in UTC, so v26.1022.1 (#373).
COMMITTED = '2026-10-21T23:30:00-0500'
TAG = 'v26.1022.1'


class ReleaseCollection(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgrel-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / 'CMakeLists.txt').write_text('project(putgd LANGUAGES C CXX)\n')
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
                    ['git', '-C', str(self.root), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture',
                     '--date', COMMITTED],
                    ['git', '-C', str(self.root), 'tag', TAG]]
        for command in commands:
            subprocess.run(command, check=True, env=dict(os.environ, GIT_COMMITTER_DATE=COMMITTED))
        self.release = collect.verify(self.root, TAG)
        identity = {field: self.release[field] for field in ('version', 'baseVersion', 'packageVersion', 'gitCommit', 'gitTree', 'commitCount')}
        identity.update(schemaVersion=1, dirty=False, sourceProvenance='git')
        self.inputs = self.root / 'build/inputs'
        for distro, kind, suffix, arch in [('ubuntu24.04', 'DEB', '.deb', 'amd64'), ('ubuntu26.04', 'DEB', '.deb', 'amd64'), ('debian13', 'DEB', '.deb', 'amd64'), ('fedora44', 'RPM', '.rpm', 'x86_64')]:
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
        from complete_source_fixture import add_inventory, write_source
        image_path = directory / 'appimage-build.json'
        image = add_inventory(json.loads(image_path.read_text()))
        image_path.write_text(json.dumps(image))
        write_source(directory, image)
        self.write_appimage(TAG)
        directory = self.inputs / 'release-native-ubuntu24.04'
        archive = directory / ('putgd-' + identity['version'] + '-source.tar.gz')
        self.source_receipt = create_archive(self.root, archive, identity)
        (directory / 'source-archive.json').write_text(json.dumps(self.source_receipt))
        compiled = {**identity, 'dirty': None, 'sourceProvenance': 'supplied',
                    'version': self.source_receipt['localBuild']['version'],
                    'packageVersion': identity['baseVersion'] + '~dev+g' + identity['gitCommit'][:12]}
        self.source_result = {'passed': True, 'compiledBuild': compiled,
                              'rebuiltProton': {'sha256': catalog['helperSha256'], 'bundledSha256': catalog['helperSha256']},
                              'identityMatchedPrograms': ['putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker', 'putgd-b2-worker']}
        (directory / 'source-build-results.json').write_text(json.dumps(self.source_result))
        proton_sources.prepare(self.proton_catalog, self.inputs / 'proton-sources', bundle)
        self.stage_debug_symbols(identity)
        self.output = self.root / 'build/output'

    def write_appimage(self, tag, information=None, url=None):
        """The image embeds tag's update information; its .zsync names the signer's mirror copy (#293)."""
        directory = self.inputs / 'release-appimage'
        path = directory / 'synthetic.AppImage'
        make_appimage(path, appimage_update.update_information(tag) if information is None else information,
                      b'byte-integrity fixture, not an AppImage' * 100)
        name = 'putgd-' + tag[1:] + '-x86_64.AppImage'
        url = url or appimage_update.mirror_url(tag, name)
        zsync = directory / 'synthetic.AppImage.zsync'
        make_zsync(zsync, path, name, url)
        receipt_path = directory / 'appimage-build.json'
        receipt = json.loads(receipt_path.read_text())
        receipt['artifact'] = {'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                               'bytes': path.stat().st_size}
        receipt['update'] = {'information': appimage_update.update_information(tag),
            'zsync': {'file': zsync.name, 'url': url, 'sha256': hashlib.sha256(zsync.read_bytes()).hexdigest(),
                      'bytes': zsync.stat().st_size}}
        receipt_path.write_text(json.dumps(receipt))
        from complete_source_fixture import write_source
        write_source(directory, receipt)
        self.write_sboms()
        return receipt_path, zsync

    def write_sboms(self):
        """Each package's SBOM names that package's bytes (sbom.py, #420)."""
        directory = self.inputs / 'release-sbom'
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir()
        receipts = {}
        for label in ('appimage', 'ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44'):
            package = (self.inputs / 'release-appimage/synthetic.AppImage' if label == 'appimage' else
                       self.inputs / ('release-native-' + label) / ('synthetic.rpm' if label == 'fedora44' else 'synthetic.deb'))
            digest = hashlib.sha256(package.read_bytes()).hexdigest()
            path = directory / ('putgd-' + self.release['version'] + '-' + label + '.cdx.json')
            path.write_text(json.dumps({'bomFormat': 'CycloneDX', 'metadata': {'component': {
                'name': package.name, 'hashes': [{'alg': 'SHA-256', 'content': digest}]}}, 'components': []}))
            receipts[label] = {'label': label, 'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                               'packageSha256': digest}
        (directory / 'sbom-receipts.json').write_text(json.dumps({'schemaVersion': 1, 'sboms': receipts}))

    def stage_debug_symbols(self, identity):
        # Each package job stages its split debug files beside its package (#124).
        for index, label in enumerate(debug_symbols.LABELS):
            directory = self.inputs / debug_symbols._artifact(label)
            for name in ('debug-symbols-' + label + '.tar', 'debug-symbols-' + label + '.json'):
                (directory / name).unlink(missing_ok=True)
            tree = self.root / 'build/debug-trees' / label
            shutil.rmtree(tree, ignore_errors=True)
            build_tree(tree / 'build', tree / 'installed', identity, format(index + 10, 'x'))
            debug_symbols.stage(tree / 'build', tree / 'installed', directory, label)

    def git_show_commit_time(self):
        return subprocess.check_output(['git', '-C', str(self.root), 'show', '-s', '--format=%ct',
                                        self.release['gitCommit']], text=True).strip()

    def run_collection(self):
        return collect.collect(self.root, self.inputs, self.output, TAG)

    def test_installer_layout_links_sources_and_keeps_debug_out_of_public_assets(self):
        import support_assets
        # Tagged builds do not recompile the source export or carry unchanged
        # Proton archives/debug payloads through the collection job.
        (self.inputs / 'release-native-ubuntu24.04/source-build-results.json').unlink()
        shutil.rmtree(self.inputs / 'proton-sources')
        for path in self.inputs.glob('release-*/debug-symbols-*'):
            path.unlink()
        collect.collect(self.root, self.inputs, self.output, TAG, installers_only=True)
        files = {p.name for p in self.output.iterdir() if p.is_file()}
        self.assertEqual(files, support_assets.inventory(self.release['version']))
        self.assertEqual(len(files), 15)
        self.assertFalse(any(name.endswith(('.tar.gz', '.zip', '.part1', '.part2', '.tar.zst')) for name in files))
        delivery = json.loads((self.output / 'source-delivery.json').read_text())
        support_assets.validate(delivery, self.release, appimage_update.REPOSITORY)
        for name, item in delivery['assets'].items():
            path = self.output / 'sources' / name
            self.assertEqual(collect.sha256(path), item['sha256'])
            self.assertEqual(path.stat().st_size, item['bytes'])
        manifest = json.loads((self.output / 'release-builds.json').read_text())
        self.assertEqual(manifest['assetLayout'], support_assets.LAYOUT)
        self.assertEqual(manifest['sources'], delivery)

    def test_installer_layout_still_requires_exact_appimage_source_bytes(self):
        path = next((self.inputs / 'release-appimage').glob('*-appimage-complete-source.tar.gz'))
        path.write_bytes(b'corrupt sources')
        with self.assertRaisesRegex(ValueError, 'source bytes'):
            collect.collect(self.root, self.inputs, self.output, TAG, installers_only=True)
        self.assertFalse(self.output.exists())

    def test_collects_unique_distro_assets_and_checksums(self):
        self.assertEqual(self.run_collection(), self.release)
        files = {path.name for path in self.output.iterdir()}
        self.assertIn('putgd-26.1022.1-ubuntu24.04-x86_64.deb', files)
        self.assertIn('putgd-26.1022.1-ubuntu26.04-x86_64.deb', files)
        self.assertIn('putgd-26.1022.1-debian13-x86_64.deb', files)
        lines = (self.output / 'SHA256SUMS').read_text().splitlines()
        self.assertIn('putgd-26.1022.1-source.tar.gz', files)
        self.assertIn('putgd-26.1022.1-fedora44-x86_64.rpm', files)
        self.assertIn('putgd-26.1022.1-x86_64.AppImage', files)
        self.assertIn('putgd-26.1022.1-appimage-complete-source.tar.gz', files)
        self.assertIn('putgd-26.1022.1-x86_64.AppImage.zsync', files)
        self.assertEqual(len(lines), 20)
        for label in ('appimage', 'ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44'):
            self.assertIn('putgd-26.1022.1-' + label + '.cdx.json', files)
        self.assertIn('putgd-26.1022.1-debug-x86_64.tar.zst', files)
        self.assertEqual((self.output / 'putgd-26.1022.1-source.tar.gz').read_bytes(),
                         (self.inputs / 'release-native-ubuntu24.04' / self.source_receipt['artifact']['file']).read_bytes())
        combined = self.output / 'putgd-26.1022.1-release.zip'
        self.assertTrue(combined.is_file())
        commit_epoch = int(self.git_show_commit_time())
        utc = datetime.fromtimestamp(commit_epoch, timezone.utc)
        expected_time = (utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second - utc.second % 2)
        with zipfile.ZipFile(combined) as archive:
            self.assertTrue(all(item.date_time == expected_time for item in archive.infolist()))
            underlying = files - {combined.name, 'SHA256SUMS', *proton_sources.PART_NAMES, 'putgd-26.1022.1-debug-x86_64.tar.zst'}
            self.assertEqual(set(archive.namelist()), underlying | {'SHA256SUMS'})
            for line in archive.read('SHA256SUMS').decode().splitlines():
                digest, name = line.split('  ', 1)
                self.assertIn(name, underlying)
                self.assertEqual(archive.read(name), (self.output / name).read_bytes())
                self.assertEqual(digest, hashlib.sha256(archive.read(name)).hexdigest())
        with zipfile.ZipFile(self.output / 'putgd-gnome50-26.1022.1.zip') as archive:
            self.assertTrue(all(item.date_time == expected_time for item in archive.infolist()))
        for line in lines:
            digest, name = line.split('  ', 1)
            self.assertEqual(digest, hashlib.sha256((self.output / name).read_bytes()).hexdigest())

    def test_repeat_collection_is_identical_despite_checkout_mtimes(self):
        self.run_collection()
        for name in ('LICENSE', 'integrations/gnome/extension.js', 'integrations/gnome/metadata.json'):
            os.utime(self.root / name, (1234567890, 1234567890))
        second = self.root / 'build/second'
        collect.collect(self.root, self.inputs, second, TAG)
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
        for name in ('source-archive.json', 'putgd-26.1022.1-source.tar.gz', 'private-context.txt'):
            (export / name).write_bytes(name.encode())
        compile_dir = build / 'source-build'
        compile_dir.mkdir()
        for name in ('results.json', 'parameters.json', 'build.log', 'private-context.txt'):
            (compile_dir / name).write_bytes(name.encode())
        output = build / 'artifact'
        stage(build, output, 'native')
        self.assertEqual({p.name for p in output.iterdir()}, {'source-archive.json',
            'putgd-26.1022.1-source.tar.gz', 'source-build-results.json', 'source-build-parameters.json', 'source-build.log'})
        self.assertEqual((output / 'source-build-results.json').read_bytes(), b'results.json')

    def test_corresponding_source_parts_and_exact_catalog_are_public(self):
        self.run_collection()
        for name in (*proton_sources.PART_NAMES, proton_sources.CATALOG_NAME):
            self.assertEqual((self.output / name).read_bytes(), (self.inputs / 'proton-sources' / name).read_bytes())
        with zipfile.ZipFile(self.output / 'putgd-26.1022.1-release.zip') as archive:
            self.assertIn(proton_sources.CATALOG_NAME, archive.namelist())
            self.assertTrue(set(proton_sources.PART_NAMES).isdisjoint(archive.namelist()))

    def test_missing_or_mismatched_debug_symbols_rejected_before_output(self):
        directory = self.inputs / 'release-native-fedora44'
        (directory / 'debug-symbols-fedora44.json').rename(directory / 'kept.json')
        with self.assertRaisesRegex(ValueError, 'not staged for this build: fedora44'):
            self.run_collection()
        self.assertFalse(self.output.exists())
        (directory / 'kept.json').rename(directory / 'debug-symbols-fedora44.json')
        manifest = json.loads((directory / 'debug-symbols-fedora44.json').read_text())
        manifest['build']['gitCommit'] = 'f' * 40
        (directory / 'debug-symbols-fedora44.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'different source identity: fedora44'):
            self.run_collection()
        self.assertFalse(self.output.exists())

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

    def test_source_build_with_optional_helpers_accepted(self):
        path = self.inputs / 'release-native-ubuntu24.04/source-build-results.json'
        programs = self.source_result['identityMatchedPrograms'] + ['putgd-kde-region', 'putgd-record']
        path.write_text(json.dumps({**self.source_result, 'identityMatchedPrograms': programs}))
        self.assertEqual(self.run_collection(), self.release)

    def test_source_build_with_unknown_program_rejected(self):
        path = self.inputs / 'release-native-ubuntu24.04/source-build-results.json'
        programs = self.source_result['identityMatchedPrograms'] + ['putgd-unknown']
        path.write_text(json.dumps({**self.source_result, 'identityMatchedPrograms': programs}))
        with self.assertRaisesRegex(ValueError, 'successful production build'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_appimage_staging_preserves_flat_consumer_paths(self):
        build = self.root / 'build/appimage-staging'
        packages = build / 'appimage-output'
        packages.mkdir(parents=True)
        (packages / 'appimage-build.json').write_bytes(b'receipt')
        (packages / 'sample.AppImage').write_bytes(b'image')
        (packages / 'sample.AppImage.zsync').write_bytes(b'zsync')
        (packages / 'AppDir').mkdir()
        (packages / 'AppDir/internal.txt').write_bytes(b'excluded')
        output = build / 'artifact'
        stage(build, output, 'appimage')
        self.assertEqual({p.name for p in output.iterdir()}, {'appimage-build.json', 'sample.AppImage', 'sample.AppImage.zsync'})
        self.assertEqual((output / 'sample.AppImage').read_bytes(), b'image')

    def test_sbom_of_other_bytes_rejected_before_output(self):
        directory = self.inputs / 'release-sbom'
        receipts = json.loads((directory / 'sbom-receipts.json').read_text())
        receipts['sboms']['fedora44']['packageSha256'] = '0' * 64
        (directory / 'sbom-receipts.json').write_text(json.dumps(receipts))
        with self.assertRaisesRegex(ValueError, 'SBOM differs from its receipt or package: fedora44'):
            self.run_collection()
        self.assertFalse(self.output.exists())
        self.write_sboms()
        path = directory / receipts['sboms']['appimage']['file']
        bom = json.loads(path.read_text())
        bom['metadata']['component']['hashes'][0]['content'] = '0' * 64
        path.write_text(json.dumps(bom))
        receipts = json.loads((directory / 'sbom-receipts.json').read_text())
        receipts['sboms']['appimage']['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        (directory / 'sbom-receipts.json').write_text(json.dumps(receipts))
        with self.assertRaisesRegex(ValueError, 'SBOM names another package: appimage'):
            self.run_collection()
        self.assertFalse(self.output.exists())
        del receipts['sboms']['debian13']
        (directory / 'sbom-receipts.json').write_text(json.dumps(receipts))
        with self.assertRaisesRegex(ValueError, 'SBOMs differ from the release packages'):
            self.run_collection()

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

    def test_appimage_update_information_and_zsync_bind_release_before_output(self):
        # A lab or development image, another channel, another repository or a
        # .zsync for other bytes never reaches a release (#293).
        cases = [('update information', dict(information='zsync|http://127.0.0.1:8765/appimage.zsync')),
                 ('update information', dict(information='')),
                 ('update information', dict(information='zsync|https://app.put.gd/dl/nightly/appimage.zsync')),
                 # The GitHub asset redirects, which zsync2 can't follow on Fedora 44 (#297).
                 ('release mirror', dict(url=appimage_update.asset_url(appimage_update.REPOSITORY, TAG,
                                                                       'putgd-26.1022.1-x86_64.AppImage')))]
        for message, change in cases:
            self.write_appimage(TAG, **change)
            with self.assertRaisesRegex(ValueError, message):
                self.run_collection()
            self.assertFalse(self.output.exists())
        receipt_path, zsync = self.write_appimage(TAG)
        image = zsync.with_name('synthetic.AppImage')
        make_zsync(zsync, image, 'putgd-26.1022.1-x86_64.AppImage', json.loads(receipt_path.read_text())['update']['zsync']['url'],
                   length=image.stat().st_size // 2048 * 2048)
        receipt = json.loads(receipt_path.read_text())
        receipt['update']['zsync'].update(sha256=hashlib.sha256(zsync.read_bytes()).hexdigest(), bytes=zsync.stat().st_size)
        receipt_path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'other image bytes'):
            self.run_collection()
        zsync.write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError, '.zsync bytes'):
            self.run_collection()
        zsync.unlink()
        with self.assertRaisesRegex(ValueError, 'missing'):
            self.run_collection()
        receipt['update'] = None
        receipt_path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'update information'):
            self.run_collection()
        self.assertFalse(self.output.exists())
        self.write_appimage(TAG)
        self.run_collection()
        self.assertEqual((self.output / 'putgd-26.1022.1-x86_64.AppImage.zsync').read_bytes(), zsync.read_bytes())

    def test_missing_or_foreign_debian_package_rejected_before_output(self):
        directory = self.inputs / 'release-native-debian13'
        path = directory / 'native-audit.json'
        receipt = json.loads(path.read_text())
        receipt['package']['format'] = 'RPM'
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'debian13'):
            self.run_collection()
        self.assertFalse(self.output.exists())
        shutil.rmtree(directory)
        with self.assertRaises(FileNotFoundError):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def test_changed_package_version_rejected(self):
        path = self.inputs / 'release-native-fedora44/native-audit.json'
        receipt = json.loads(path.read_text())
        receipt['manifest']['packageVersion'] = '26.1022.1~nightly'
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'identity'):
            self.run_collection()
        self.assertFalse(self.output.exists())

    def nightly_inputs(self):
        # U1 covers exact Git-tag verification; this fixture starts at its verified
        # identity boundary and tests the real collector/archive byte pipeline.
        tag = TAG + '-nightly'
        version = tag[1:]
        self.release.update(tag=tag, version=version, packageVersion=version.replace('-nightly', '~nightly'), channel='nightly')
        for receipt_path in self.inputs.glob('*/native-audit.json'):
            receipt = json.loads(receipt_path.read_text())
            receipt['manifest'].update(version=version, packageVersion=self.release['packageVersion'])
            receipt_path.write_text(json.dumps(receipt))
        receipt_path = self.inputs / 'release-appimage/appimage-build.json'
        receipt = json.loads(receipt_path.read_text())
        receipt['build'].update(version=version, packageVersion=self.release['packageVersion'])
        receipt_path.write_text(json.dumps(receipt))
        self.write_appimage(tag)
        self.stage_debug_symbols(receipt['build'])
        directory = self.inputs / 'release-native-ubuntu24.04'
        archive = directory / ('putgd-' + version + '-source.tar.gz')
        self.source_receipt = create_archive(self.root, archive, receipt['build'])
        (directory / 'source-archive.json').write_text(json.dumps(self.source_receipt))
        return tag, archive

    def test_nightly_cli_preserves_all_twenty_one_raw_assets_and_exact_compiled_source(self):
        tag, source = self.nightly_inputs()
        (self.inputs / 'credentials.txt').write_text('synthetic excluded fixture')
        argv = ['collect.py', '--tag', tag, '--root', str(self.root),
                '--inputs', str(self.inputs), '--output', str(self.output)]
        with patch.object(sys, 'argv', argv), patch('builtins.print'), \
             patch.object(collect, 'verify', return_value=self.release):
            collect.main()
        files = {path.name for path in self.output.iterdir()}
        from release_bundle import expected_members
        debug_name = debug_symbols.asset_name(self.release['version'])
        expected = set(expected_members(self.release['version']) +
                       ['putgd-' + self.release['version'] + '-release.zip', 'SHA256SUMS', *proton_sources.PART_NAMES, debug_name])
        self.assertEqual(files, expected)
        self.assertEqual(len(files), 21)
        self.assertEqual(appimage_update.read_update_information(self.output / ('putgd-' + self.release['version'] + '-x86_64.AppImage')),
                         'zsync|https://app.put.gd/dl/nightly/appimage.zsync')
        self.assertEqual((self.output / source.name).read_bytes(), source.read_bytes())
        builds = json.loads((self.output / 'release-builds.json').read_text())
        self.assertEqual(builds['release']['channel'], 'nightly')
        self.assertEqual(builds['receipts']['sourceBuild'], self.source_result)
        self.assertEqual(builds['receipts']['sourceBuild']['compiledBuild']['sourceProvenance'], 'supplied')
        self.assertNotEqual(builds['receipts']['sourceBuild']['compiledBuild']['version'], self.release['version'])
        with zipfile.ZipFile(self.output / ('putgd-' + self.release['version'] + '-release.zip')) as archive:
            for name in expected - {'SHA256SUMS', 'putgd-' + self.release['version'] + '-release.zip', *proton_sources.PART_NAMES, debug_name}:
                self.assertEqual(archive.read(name), (self.output / name).read_bytes())
            self.assertNotIn(debug_name, archive.namelist())
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
