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

    def test_third_party_sources_are_disabled_and_ubuntu_sources_are_kept(self):
        ubuntu = ('Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\nSuites: noble\nComponents: main\n'
                  'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n'
                  'Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\nComponents: main\n')
        legacy = self.root / 'sources.list.d/microsoft-prod.list'
        legacy.write_text('# Microsoft\ndeb [arch=amd64,arm64 signed-by=/m.gpg] https://packages.microsoft.com/ubuntu/24.04/prod noble main\n')
        vendor = self.root / 'sources.list.d/vendor.sources'
        vendor.write_text('Types: deb\nURIs: https://vendor.example/apt\n https://mirror.example/apt\nSuites: stable\n')
        keep = self.root / 'sources.list.d/ubuntu.sources'
        keep.write_text(ubuntu)
        main = self.root / 'sources.list'
        main.write_text('deb http://us-east-2.ec2.archive.ubuntu.com/ubuntu noble main\n'
                        'deb https://packages.microsoft.com/repos/x noble main\n')
        self.assertEqual(sorted(mirrors.disable_third_party(self.root)),
                         ['sources.list', 'sources.list.d/microsoft-prod.list', 'sources.list.d/vendor.sources'])
        self.assertEqual(keep.read_text(), ubuntu)
        self.assertEqual(legacy.read_text().splitlines()[0:2],
                         ['# Microsoft', '# disabled for the AppImage job: deb [arch=amd64,arm64 signed-by=/m.gpg] '
                          'https://packages.microsoft.com/ubuntu/24.04/prod noble main'])
        self.assertTrue(vendor.read_text().endswith('Suites: stable\nEnabled: no\n'))
        self.assertEqual(main.read_text().splitlines(),
                         ['deb http://us-east-2.ec2.archive.ubuntu.com/ubuntu noble main',
                          '# disabled for the AppImage job: deb https://packages.microsoft.com/repos/x noble main'])
        self.assertEqual(mirrors.disable_third_party(self.root), [])

    def test_deb822_field_names_are_case_insensitive_and_enabled_yes_is_turned_off(self):
        lower = self.root / 'sources.list.d/lower.sources'
        lower.write_text('types: deb\nuris: https://vendor.example/apt\nsuites: stable\n')
        yes = self.root / 'sources.list.d/yes.sources'
        yes.write_text('Types: deb\nURIs: https://packages.microsoft.com/ubuntu/24.04/prod\nSuites: noble\nenabled: Yes\n')
        no = self.root / 'sources.list.d/no.sources'
        no.write_text('Types: deb\nURIs: https://vendor.example/apt\nEnabled: no\n')
        ubuntu = self.root / 'sources.list.d/ubuntu.sources'
        ubuntu.write_text('Types: deb\nURIS: http://security.ubuntu.com/ubuntu/\nENABLED: yes\n')
        self.assertEqual(sorted(mirrors.disable_third_party(self.root)),
                         ['sources.list.d/lower.sources', 'sources.list.d/yes.sources'])
        self.assertTrue(lower.read_text().endswith('suites: stable\nEnabled: no\n'))
        self.assertEqual(yes.read_text(), 'Types: deb\nURIs: https://packages.microsoft.com/ubuntu/24.04/prod\n'
                                          'Suites: noble\nenabled: no\n')
        self.assertEqual(no.read_text(), 'Types: deb\nURIs: https://vendor.example/apt\nEnabled: no\n')
        self.assertEqual(ubuntu.read_text(), 'Types: deb\nURIS: http://security.ubuntu.com/ubuntu/\nENABLED: yes\n')
        self.assertEqual(mirrors.disable_third_party(self.root), [])

    def test_an_ubuntu_look_alike_host_is_third_party(self):
        path = self.root / 'sources.list'
        path.write_text('deb https://archive.ubuntu.com.example.invalid/ubuntu noble main\n'
                        'deb https://notubuntu.com/ubuntu noble main\n')
        self.assertEqual(mirrors.disable_third_party(self.root), ['sources.list'])
        self.assertEqual(path.read_text().count('# disabled for the AppImage job: '), 2)


if __name__ == '__main__':
    unittest.main()
