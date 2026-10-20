"""Kept debug symbols (#124): each build's split .debug files are checked against
its packaged binaries and merged by GNU build ID into one release asset."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/releases'))
import debug_symbols
from debug_fixture import build_tree, make_elf

IDENTITY = {'schemaVersion': 1, 'version': '0.1.1-rc.1', 'baseVersion': '0.1.1', 'packageVersion': '0.1.1~rc.1',
            'gitCommit': 'c' * 40, 'gitTree': 'd' * 40, 'commitCount': 7, 'dirty': False, 'sourceProvenance': 'git'}
SEEDS = {'ubuntu24.04': 'a1', 'ubuntu26.04': 'b2', 'debian13': 'c3', 'fedora44': 'd4', 'appimage': 'e5'}


@unittest.skipUnless(shutil.which('zstd'), 'zstd is required for the debug asset')
class DebugSymbols(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='pgdbg-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def staged(self, label, identity=IDENTITY, products=None):
        build, installed = self.root / label / 'build', self.root / label / 'installed'
        build_tree(build, installed, identity, SEEDS[label], products)
        output = self.root / 'staged' / label
        return build, installed, output, debug_symbols.stage(build, installed, output, label, 1760000000)

    def test_reads_build_id_and_debuglink(self):
        path = self.root / 'binary'
        make_elf(path, '0123456789abcdef0123456789abcdef01234567', debuglink='binary.debug')
        self.assertEqual(debug_symbols.build_id(path), '0123456789abcdef0123456789abcdef01234567')
        self.assertEqual(debug_symbols.debuglink(path), 'binary.debug')
        self.assertFalse(debug_symbols.has_debug_info(path))

    def test_stage_lays_out_debug_files_by_build_id(self):
        _, _, output, manifest = self.staged('debian13', products=[*debug_symbols.REQUIRED, 'putgd-record'])
        self.assertEqual({entry['program'] for entry in manifest['files']}, {*debug_symbols.REQUIRED, 'putgd-record'})
        for entry in manifest['files']:
            self.assertEqual(entry['path'], '.build-id/' + entry['buildId'][:2] + '/' + entry['buildId'][2:] + '.debug')
        self.assertTrue((output / 'debug-symbols-debian13.tar').is_file())
        self.assertEqual(json.loads((output / 'debug-symbols-debian13.json').read_text())['build'], IDENTITY)

    def test_stage_rejects_unsplit_mismatched_or_missing_files(self):
        cases = {
            'missing': lambda build, installed: (build / 'putgd-ui.debug').unlink(),
            'build ID differs from its binary': lambda build, installed: make_elf(build / 'putgd.debug', 'f' * 40, debug_info=True),
            'debuglink': lambda build, installed: make_elf(build / 'putgd', debug_symbols.build_id(build / 'putgd')),
            'not split': lambda build, installed: make_elf(build / 'putgd', debug_symbols.build_id(build / 'putgd'),
                                                          debuglink='putgd.debug', debug_info=True),
            'Packaged binary has a different': lambda build, installed: make_elf(installed / 'usr/bin/putgd-cli', 'e' * 40),
            'Packaged binary missing': lambda build, installed: (installed / 'usr/bin/putgd-capture').unlink(),
            'Product binary missing': lambda build, installed: (build / 'putgd-s3-worker').unlink(),
        }
        for message, damage in cases.items():
            with self.subTest(message):
                build, installed = self.root / message / 'build', self.root / message / 'installed'
                build_tree(build, installed, IDENTITY, 'ab')
                damage(build, installed)
                with self.assertRaisesRegex(ValueError, message):
                    debug_symbols.stage(build, installed, self.root / message / 'out', 'fedora44')

    def test_asset_merges_every_build(self):
        inputs = {label: self.staged(label)[2] for label in debug_symbols.LABELS}
        prepared = debug_symbols.prepare(inputs, lambda build: build == IDENTITY)
        output = self.root / 'publish'
        output.mkdir()
        result = debug_symbols.write(prepared, output, IDENTITY['version'], 1760000000)
        self.assertEqual(result['file'], 'putgd-0.1.1-rc.1-debug-x86_64.tar.zst')
        self.assertEqual(result['buildIds'], len(debug_symbols.LABELS) * len(debug_symbols.REQUIRED))
        manifest, files = debug_symbols.read_asset(output / result['file'])
        self.assertEqual([build['label'] for build in manifest['builds']], list(debug_symbols.LABELS))
        for build in manifest['builds']:
            for program in build['programs']:
                identifier = program['buildId']
                self.assertIn('.build-id/' + identifier[:2] + '/' + identifier[2:] + '.debug', files)
        # The same inputs give the same bytes.
        again = self.root / 'again'
        again.mkdir()
        debug_symbols.write(prepared, again, IDENTITY['version'], 1760000000)
        self.assertEqual((again / result['file']).read_bytes(), (output / result['file']).read_bytes())

    def test_prepare_rejects_tampering_identity_and_missing_builds(self):
        inputs = {label: self.staged(label)[2] for label in debug_symbols.LABELS}
        with self.assertRaisesRegex(ValueError, 'different source identity'):
            debug_symbols.prepare(inputs, lambda build: False)
        with self.assertRaisesRegex(ValueError, 'exactly these builds'):
            debug_symbols.prepare({label: path for label, path in inputs.items() if label != 'appimage'}, lambda build: True)
        archive = inputs['fedora44'] / 'debug-symbols-fedora44.tar'
        archive.write_bytes(archive.read_bytes()[:-1] + b'x')
        with self.assertRaisesRegex(ValueError, 'archive bytes differ'):
            debug_symbols.prepare(inputs, lambda build: True)

    def test_dry_run_uses_downloaded_package_artifacts(self):
        downloaded = self.root / 'downloaded'
        for label in debug_symbols.LABELS:
            staged = self.staged(label, identity={**IDENTITY, 'gitCommit': ''})[2]
            shutil.copytree(staged, downloaded / debug_symbols._artifact(label))
        result = debug_symbols.dry_run(downloaded, self.root / 'dry')
        self.assertTrue((self.root / 'dry' / result['file']).is_file())


if __name__ == '__main__':
    unittest.main()
