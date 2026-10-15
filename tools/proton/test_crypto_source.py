import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('provision', Path(__file__).with_name('provision_runtime.py'))
provision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(provision)


class CryptoSourceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.package = self.source / 'packages/crypto'
        (self.package / 'src').mkdir(parents=True)
        (self.package / 'src/index.ts').write_text('export const selected = true;\n')
        (self.package / 'package.json').write_text(json.dumps({'name': '@protontech/crypto', 'version': '2.0.1', 'license': 'GPL-3.0'}))
        (self.source / 'LICENSE').write_text('Synthetic license fixture\n')
        (self.source / 'README.md').write_text('Synthetic source fixture\n')
        self.git('init', '-q')
        self.git('add', '--', 'packages', 'LICENSE', 'README.md')
        self.git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@invalid', 'commit', '-qm', 'fixture')
        self.lock = {'cryptoVersion': '2.0.1', 'cryptoRegistryBootstrapVersion': '2.0.0', 'cryptoSourceUrl': 'https://github.com/ProtonMail/WebPackages.git',
                     'cryptoSourceReference': self.git('rev-parse', 'HEAD').strip(),
                     'cryptoLicenseSha256': hashlib.sha256((self.source / 'LICENSE').read_bytes()).hexdigest()}
        self.work = self.root / 'build'
        self.target = self.work / 'node_modules/@protontech/crypto'
        (self.target / 'src').mkdir(parents=True)
        (self.target / 'src/obsolete.ts').write_text('obsolete bootstrap code')
        (self.target / 'node_modules').mkdir()
        (self.target / 'node_modules/keep').write_text('locked dependency')
        (self.work / 'package-lock.json').write_text(json.dumps({'packages': {
            '': {}, 'node_modules/@protontech/crypto': {'version': '2.0.0', 'integrity': 'sha512-synthetic'}}}))

    def git(self, *args):
        return subprocess.run(['git', '-C', str(self.source), *args], check=True,
                              capture_output=True, text=True).stdout

    def test_explicit_source_replaces_bootstrap_and_preserves_locked_dependencies(self):
        record = provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertFalse((self.target / 'src/obsolete.ts').exists())
        self.assertTrue((self.target / 'src/index.ts').is_file())
        self.assertEqual((self.target / 'node_modules/keep').read_text(), 'locked dependency')
        self.assertEqual(record['registryVersion'], '2.0.0')
        self.assertEqual(record['version'], '2.0.1')
        self.assertEqual((self.target / 'LICENSE').read_bytes(), (self.source / 'LICENSE').read_bytes())

    def test_wrong_source_commit_is_rejected_before_replacement(self):
        self.lock['cryptoSourceReference'] = '0' * 40
        with self.assertRaisesRegex(ValueError, 'commit'):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())

    def test_dirty_source_is_rejected_before_replacement(self):
        (self.package / 'src/index.ts').write_text('modified')
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())

    def test_ignored_dependency_is_rejected_before_replacement(self):
        (self.source / '.git/info/exclude').write_text('node_modules/\n')
        ignored = self.package / 'src/node_modules/openpgp'
        ignored.mkdir(parents=True)
        (ignored / 'index.js').write_text('export const shadow = true;')
        with self.assertRaisesRegex(ValueError, 'dirty'):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())

    def test_wrong_bootstrap_version_is_rejected_before_replacement(self):
        self.lock['cryptoRegistryBootstrapVersion'] = '9.9.9'
        with self.assertRaisesRegex(ValueError, 'bootstrap'):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())

    def test_symlink_source_root_is_rejected_before_replacement(self):
        original = self.package / 'src'
        original.rename(self.package / 'original-src')
        original.symlink_to('original-src', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'dirty|symlink'):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())

    def test_wrong_license_text_is_rejected_before_replacement(self):
        self.lock['cryptoLicenseSha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'license'):
            provision.apply_crypto_source(self.work, self.source, self.lock)
        self.assertTrue((self.target / 'src/obsolete.ts').exists())


if __name__ == '__main__':
    unittest.main()
