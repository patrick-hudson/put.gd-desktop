"""Kept debug symbols (#124): each build's split .debug files are checked against
its packaged binaries and merged by GNU build ID into one release asset."""
import json
from pathlib import Path
import shutil
import subprocess
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

    @unittest.skipUnless(all(shutil.which(tool) for tool in ('cc', 'objcopy', 'dwz', 'addr2line', 'nm')),
                         'needs cc, objcopy, dwz, addr2line and nm')
    def test_asset_shares_each_builds_dwarf_and_still_symbolizes(self):
        # Two programs of one build with zlib-compressed split debug files, as
        # the package jobs stage them; the asset decompresses them, moves the
        # DWARF they share into .dwz/<label>.debug and links it relatively (#162).
        header = self.root / 'shared.h'
        header.write_text('\n'.join('struct shape%d { int a%d; long b%d; const char* name%d; double c%d[4]; };' % ((i,) * 5)
                                    for i in range(40)) + '\n')
        files, programs = {}, []
        for index, name in enumerate(('putgd', 'putgd-ui')):
            source = self.root / (name + '.c')
            source.write_text('#include "shared.h"\n' + ''.join('struct shape%d s%d;\n' % (i, i) for i in range(40))
                              + 'int entry_%d(int value) { return value * %d + s3.a3; }\n' % (index, index + 2)
                              + 'int main(void) { return entry_%d(3); }\n' % index)
            binary = self.root / name
            subprocess.run(['cc', '-g', '-O1', '-Wl,--build-id', '-I', str(self.root), '-o', str(binary), str(source)], check=True)
            debug = self.root / (name + '.debug')
            subprocess.run(['objcopy', '--only-keep-debug', '--compress-debug-sections=zlib', str(binary), str(debug)], check=True)
            identifier = debug_symbols.build_id(binary)
            files['.build-id/' + identifier[:2] + '/' + identifier[2:] + '.debug'] = debug.read_bytes()
            programs.append({'program': name, 'buildId': identifier, 'binary': binary, 'entry': 'entry_%d' % index})
        builds = [{'label': 'appimage', 'build': IDENTITY,
                   'programs': [{'program': entry['program'], 'buildId': entry['buildId']} for entry in programs]}]
        output = self.root / 'publish'
        output.mkdir()
        result = debug_symbols.write({'files': files, 'builds': builds}, output, IDENTITY['version'], 1760000000)
        manifest, members = debug_symbols.read_asset(output / result['file'])
        self.assertEqual(manifest['builds'][0]['dwz'], '.dwz/appimage.debug')
        self.assertIn('.dwz/appimage.debug', members)
        # The documented maintainer step: extract with plain tar, then look up by build ID.
        extracted = self.root / 'debug'
        extracted.mkdir()
        subprocess.run(['tar', '--zstd', '-xf', str(output / result['file']), '-C', str(extracted)], check=True)
        for entry in programs:
            debug = extracted / ('.build-id/' + entry['buildId'][:2] + '/' + entry['buildId'][2:] + '.debug')
            data = debug.read_bytes()
            self.assertEqual(debug_symbols.compressed_sections(data), [])
            _, offset, size = debug_symbols.elf_sections(data)['.gnu_debugaltlink']
            self.assertEqual(data[offset:data.index(b'\0', offset)], b'../../.dwz/appimage.debug')
            symbols = subprocess.run(['nm', str(entry['binary'])], check=True, capture_output=True, text=True).stdout
            address = next(line.split()[0] for line in symbols.splitlines() if line.endswith(' ' + entry['entry']))
            found = subprocess.run(['addr2line', '-f', '-e', str(debug), '0x' + address], check=True,
                                   capture_output=True, text=True).stdout.split('\n')
            self.assertEqual(found[0], entry['entry'])
            self.assertTrue(found[1].endswith(entry['program'] + '.c:%d' % 42), found)
        # The same inputs give the same bytes.
        again = self.root / 'again'
        again.mkdir()
        debug_symbols.write({'files': files, 'builds': builds}, again, IDENTITY['version'], 1760000000)
        self.assertEqual((again / result['file']).read_bytes(), (output / result['file']).read_bytes())

    @unittest.skipUnless(shutil.which('cc') and shutil.which('objcopy'), 'needs a C compiler and objcopy')
    def test_asset_holds_uncompressed_debug_sections(self):
        # The builds split with --compress-debug-sections=zlib; the asset
        # decompresses them so the outer zstd can work across builds (#162).
        source = self.root / 'program.c'
        source.write_text('int square(int value) { return value * value; }\nint main(void) { return square(3); }\n')
        binary, debug = self.root / 'program', self.root / 'program.debug'
        subprocess.run(['cc', '-g', '-O1', '-Wl,--build-id', '-o', str(binary), str(source)], check=True)
        subprocess.run(['objcopy', '--only-keep-debug', '--compress-debug-sections=zlib', str(binary), str(debug)], check=True)
        compressed = debug.read_bytes()
        self.assertIn('.debug_info', debug_symbols.compressed_sections(compressed))
        plain = debug_symbols.decompressed(compressed, self.root)
        self.assertEqual(debug_symbols.compressed_sections(plain), [])
        self.assertGreater(len(plain), len(compressed))
        (self.root / 'plain.debug').write_bytes(plain)
        self.assertEqual(debug_symbols.build_id(self.root / 'plain.debug'), debug_symbols.build_id(binary))
        # Files without compressed sections pass through unchanged.
        self.assertIs(debug_symbols.decompressed(plain, self.root), plain)

    def test_dry_run_uses_downloaded_package_artifacts(self):
        downloaded = self.root / 'downloaded'
        for label in debug_symbols.LABELS:
            staged = self.staged(label, identity={**IDENTITY, 'gitCommit': ''})[2]
            shutil.copytree(staged, downloaded / debug_symbols._artifact(label))
        result = debug_symbols.dry_run(downloaded, self.root / 'dry')
        self.assertTrue((self.root / 'dry' / result['file']).is_file())


if __name__ == '__main__':
    unittest.main()
