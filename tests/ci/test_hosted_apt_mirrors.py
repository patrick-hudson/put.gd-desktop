"""The AppImage mirror choice keeps distro provenance and runner source formats."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('hosted_apt_mirrors', ROOT / 'tools/ci/hosted_apt_mirrors.py')
mirrors = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirrors)

RUNNER_MIRRORS = ('http://azure.archive.ubuntu.com/ubuntu/\tpriority:1\n'
                  'https://archive.ubuntu.com/ubuntu/\tpriority:2\n'
                  'https://security.ubuntu.com/ubuntu/\tpriority:3\n')


class HostedAptMirrors(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'sources.list.d').mkdir()

    def test_runner_mirror_list_keeps_archive_security_priority_and_trust(self):
        path = self.root / 'apt-mirrors.txt'
        path.write_text(RUNNER_MIRRORS)
        source = self.root / 'sources.list.d/ubuntu.sources'
        original = ('Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\n'
                    'Suites: noble noble-updates noble-backports\nComponents: main restricted universe multiverse\n'
                    'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')
        source.write_text(original)
        self.assertEqual(mirrors.configure(self.root), ['apt-mirrors.txt'])
        self.assertEqual(path.read_text(), RUNNER_MIRRORS.split('\n', 1)[1])
        self.assertEqual(source.read_text(), original)
        self.assertEqual(mirrors.configure(self.root), [])

    def test_mirror_comments_do_not_remove_the_official_fallback(self):
        path = self.root / 'apt-mirrors.txt'
        original = (RUNNER_MIRRORS + '# http://azure.archive.ubuntu.com/ubuntu/\n'
                    'https://archive.ubuntu.com/ubuntu/\tpriority:4 # http://azure.archive.ubuntu.com/ubuntu/\n')
        path.write_text(original)
        mirrors.configure(self.root)
        self.assertEqual(path.read_text(), original.split('\n', 1)[1])

    def test_direct_deb822_and_legacy_uris_only_change_exact_azure_tokens(self):
        paths = [self.root / 'sources.list', self.root / 'sources.list.d/legacy.list',
                 self.root / 'sources.list.d/ubuntu.sources']
        inputs = [
            'deb [signed-by=/key.gpg] http://azure.archive.ubuntu.com/ubuntu/ noble main\n',
            'deb-src\thttps://azure.archive.ubuntu.com/ubuntu noble-updates universe\n',
            'Types: deb deb-src\nURIs: https://azure.archive.ubuntu.com/ubuntu/ https://security.ubuntu.com/ubuntu/\n'
            'Suites: noble-security\nComponents: main\nSigned-By: /key.gpg\n',
        ]
        third_party = self.root / 'sources.list.d/vendor.sources'
        unrelated = ('# http://azure.archive.ubuntu.com/ubuntu/\nTypes: deb\n'
                     'URIs: https://azure.archive.ubuntu.com.example.invalid/ubuntu/\n'
                     'Signed-By: /vendor.gpg\n')
        third_party.write_text(unrelated)
        for path, text in zip(paths, inputs):
            path.write_text(text)
        self.assertEqual(len(mirrors.configure(self.root)), 3)
        for path, text in zip(paths, inputs):
            self.assertEqual(path.read_text(), mirrors.AZURE.sub(mirrors.ARCHIVE, text))
        self.assertEqual(third_party.read_text(), unrelated)

    def test_folded_deb822_uris_preserve_other_fields(self):
        path = self.root / 'sources.list.d/ubuntu.sources'
        path.write_text('Types: deb\nURIs: https://security.ubuntu.com/ubuntu/\n'
                        ' https://azure.archive.ubuntu.com/ubuntu/\nSuites: noble\n'
                        'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')
        mirrors.configure(self.root)
        self.assertIn(' https://archive.ubuntu.com/ubuntu/\nSuites: noble\n', path.read_text())
        self.assertIn('Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n', path.read_text())

    def test_missing_official_fallback_fails_without_partial_writes(self):
        path = self.root / 'apt-mirrors.txt'
        original = RUNNER_MIRRORS.splitlines(keepends=True)[0]
        path.write_text(original)
        source = self.root / 'sources.list'
        source.write_text('deb http://azure.archive.ubuntu.com/ubuntu/ noble main\n')
        with self.assertRaisesRegex(ValueError, 'no official Ubuntu archive fallback'):
            mirrors.configure(self.root)
        self.assertEqual(path.read_text(), original)
        self.assertIn('azure.archive.ubuntu.com', source.read_text())


if __name__ == '__main__':
    unittest.main()
