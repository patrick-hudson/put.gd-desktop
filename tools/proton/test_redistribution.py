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
