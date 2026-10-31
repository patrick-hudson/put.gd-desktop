import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('audit_redistribution.py')
spec = importlib.util.spec_from_file_location('redistribution', SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class RedistributionTests(unittest.TestCase):
    def test_search_capability_glue_does_not_imply_wasm_engine_or_ffi(self):
        prefix = 'node_modules/@protontech/drive-sdk/src/search/'
        result = audit.search_import_graph({'outputs': {'helper': {'inputs': {
            prefix + 'sdkCapabilities.ts': {'bytesInOutput': 100},
            prefix + 'types.ts': {'bytesInOutput': 0}}}}})
        self.assertFalse(result['rustWasmSearchEngineBundled'])
        self.assertFalse(result['boltffiRuntimeBundled'])

    def test_search_engine_wasm_or_ffi_cannot_silently_enter_helper(self):
        for path in ('node_modules/@protontech/drive-sdk/src/search/core/driveEngineLoaders.ts',
                     'node_modules/@protontech/drive-sdk/src/search/vendor/engine.wasm',
                     'node_modules/@boltffi/runtime/index.js'):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'unexpected bundled search'):
                audit.search_import_graph({'outputs': {'helper': {'inputs': {path: {'bytesInOutput': 1}}}}})

    def test_remaining_evidence_uses_current_exact_helper_catalog(self):
        catalog = {'helperSha256': 'a' * 64, 'published': True,
                   'openGates': ['Current final-package acceptance']}
        self.assertEqual(audit.remaining_evidence(catalog, 'a' * 64),
                         ['Current final-package acceptance'])
        catalog['published'] = False
        self.assertIn('Publish corresponding source assets with final binary packages',
                      audit.remaining_evidence(catalog, 'a' * 64))

    def test_remaining_evidence_rejects_another_helpers_proof(self):
        with self.assertRaisesRegex(ValueError, 'does not describe'):
            audit.remaining_evidence({'helperSha256': 'a' * 64, 'openGates': []}, 'b' * 64)

    def test_native_receipt_cannot_silently_transfer_to_new_sources(self):
        pins = {'bun': '1' * 40, 'webkit': '2' * 40, 'tinycc': '3' * 40}
        catalog = {'helperSha256': 'a' * 64, 'published': True, 'openGates': ['P6'],
                   'sourcePins': pins, 'proof': {'nativeBunRelinkVerified': True},
                   'nativeRelinkProof': {'helperSha256': 'a' * 64, 'sourcePins': dict(pins)}}
        self.assertEqual(audit.remaining_evidence(catalog, 'a' * 64), ['P6'])
        for name in pins:
            with self.subTest(name=name):
                catalog['nativeRelinkProof']['sourcePins'] = {**pins, name: '0' * 40}
                with self.assertRaisesRegex(ValueError, 'native relink proof'):
                    audit.remaining_evidence(catalog, 'a' * 64)

    def test_native_receipt_requires_current_helper_even_when_runtime_sources_match(self):
        pins = {'bun': '1' * 40, 'webkit': '2' * 40, 'tinycc': '3' * 40}
        catalog = {'helperSha256': 'a' * 64, 'openGates': [], 'sourcePins': pins,
                   'proof': {'nativeCompiledHelperSourceSelectionVerified': True},
                   'nativeRelinkProof': {'helperSha256': 'b' * 64, 'sourcePins': pins}}
        with self.assertRaisesRegex(ValueError, 'native relink proof'):
            audit.remaining_evidence(catalog, 'a' * 64)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lock = {'': {}}

    def package(self, location, name, license='MIT', installed=True):
        self.lock[location] = {'version': '1.0.0', 'integrity': 'sha512-synthetic', 'license': license}
        if installed:
            folder = self.root / location
            folder.mkdir(parents=True)
            (folder / 'package.json').write_text(json.dumps({'name': name, 'version': '1.0.0', 'license': license}))

    def result(self, inputs):
        (self.root / 'package-lock.json').write_text(json.dumps({'packages': self.lock}))
        return audit.inventory(self.root, {'outputs': {'runtime': {'inputs': inputs}}})

    def test_nested_alias_is_attributed_to_its_own_locked_package(self):
        self.package('node_modules/crypto', 'crypto')
        self.package('node_modules/crypto/node_modules/hash', 'actual-hash')
        result = self.result({'node_modules/crypto/node_modules/hash/index.js': {'bytesInOutput': 12}})
        by_path = {p['location']: p for p in result}
        self.assertEqual(by_path['node_modules/crypto']['classification'], 'installed-not-bundled')
        self.assertEqual(by_path['node_modules/crypto/node_modules/hash']['name'], 'actual-hash')
        self.assertEqual(by_path['node_modules/crypto/node_modules/hash']['bundledBytes'], 12)

    def test_absent_optional_and_tree_shaken_packages_are_distinct(self):
        self.package('node_modules/optional', 'optional', installed=False)
        self.package('node_modules/shaken', 'shaken')
        self.package('node_modules/typescript', 'typescript')
        result = {p['location']: p for p in self.result({'node_modules/shaken/a.js': {'bytesInOutput': 0}})}
        self.assertEqual(result['node_modules/optional']['classification'], 'not-installed')
        self.assertEqual(result['node_modules/shaken']['classification'], 'installed-not-bundled')
        self.assertEqual(result['node_modules/typescript']['classification'], 'build-only')

    def test_approved_source_override_is_distinct_from_registry_bootstrap(self):
        location = 'node_modules/@protontech/crypto'
        self.package(location, '@protontech/crypto', 'GPL-3.0')
        self.lock[location]['version'] = '0.9.0'
        (self.root / 'package-lock.json').write_text(json.dumps({'packages': self.lock}))
        override = {location: {'registryVersion': '0.9.0', 'version': '1.0.0',
                              'sourceReference': 'a' * 40, 'sha256': {}}}
        result = audit.inventory(self.root, {'outputs': {'runtime': {'inputs': {
            location + '/index.js': {'bytesInOutput': 3}}}}}, override)
        selected = result[0]
        self.assertEqual(selected['version'], '1.0.0')
        self.assertEqual(selected['registryVersion'], '0.9.0')
        self.assertEqual(selected['sourceOverride']['sourceReference'], 'a' * 40)

    def test_changed_override_source_is_rejected(self):
        location = 'node_modules/@protontech/crypto'
        self.package(location, '@protontech/crypto')
        self.result({})
        override = {location: {'registryVersion': '1.0.0', 'version': '1.0.0',
                              'sourceReference': 'a' * 40,
                              'sha256': {'package.json': '0' * 64}}}
        with self.assertRaisesRegex(ValueError, 'source hash'):
            audit.inventory(self.root, {'outputs': {}}, override)

    def test_unaccounted_compiler_input_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unlocked'):
            self.result({'node_modules/unlocked/index.js': {'bytesInOutput': 1}})

    def test_source_archive_excludes_bun_binaries_and_unrelated_files(self):
        self.package('node_modules/lib', 'lib')
        self.package('node_modules/@oven/bun-linux-x64', 'bun-platform')
        (self.root / 'credentials.txt').write_text('synthetic-not-a-real-secret')
        (self.root / 'runtime.ts').write_text('export {}')
        self.result({})
        paths = audit.source_paths(self.root)
        self.assertIn('runtime.ts', paths)
        self.assertIn('node_modules/lib/package.json', paths)
        self.assertNotIn('credentials.txt', paths)
        self.assertNotIn('node_modules/@oven/bun-linux-x64/package.json', paths)

    def test_source_archive_rejects_top_level_helper_symlinks(self):
        (self.root / 'runtime.test.ts').symlink_to('/etc/passwd')
        self.result({})
        with self.assertRaisesRegex(ValueError, 'symlink'):
            audit.source_paths(self.root)

    def test_source_archive_rejects_dependency_symlinks(self):
        self.package('node_modules/lib', 'lib')
        (self.root / 'node_modules/lib/link.js').symlink_to('/etc/passwd')
        self.result({})
        with self.assertRaisesRegex(ValueError, 'symlink'):
            audit.source_paths(self.root)


if __name__ == '__main__':
    unittest.main()
