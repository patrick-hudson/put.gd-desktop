"""The AppImage job hands its Ubuntu sources to the mirror probe and keeps their trust and formats."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('hosted_apt_mirrors', ROOT / 'tools/ci/hosted_apt_mirrors.py')
mirrors = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirrors)

class HostedAptMirrors(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'sources.list.d').mkdir()

    def test_runner_sources_become_the_uris_the_mirror_probe_rewrites(self):
        # Run 44364918159: the runner's mirror list named one regional mirror, which stalled every attempt (#1668).
        source = self.root / 'sources.list.d/ubuntu.sources'
        source.write_text('Types: deb\nURIs: mirror+file:/etc/apt/apt-mirrors.txt\n'
                          'Suites: noble noble-updates noble-backports\nComponents: main restricted universe multiverse\n'
                          'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n'
                          'Types: deb\nURIs: https://security.ubuntu.com/ubuntu\nSuites: noble-security\n'
                          'Components: main\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')
        self.assertEqual(mirrors.configure(self.root), ['sources.list.d/ubuntu.sources'])
        self.assertEqual(source.read_text(),
                         'Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\n'
                         'Suites: noble noble-updates noble-backports\nComponents: main restricted universe multiverse\n'
                         'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n'
                         'Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\n'
                         'Components: main\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')
        self.assertEqual(mirrors.configure(self.root), [])

    def test_legacy_lines_folded_uris_and_look_alike_hosts(self):
        legacy = self.root / 'sources.list'
        legacy.write_text('deb [signed-by=/key.gpg] http://us-east-2.ec2.archive.ubuntu.com/ubuntu noble main\n'
                          'deb-src\thttps://azure.archive.ubuntu.com/ubuntu/ noble-updates universe\n'
                          '# deb http://azure.archive.ubuntu.com/ubuntu/ noble main\n'
                          'deb https://archive.ubuntu.com.example.invalid/ubuntu noble main\n')
        folded = self.root / 'sources.list.d/ubuntu.sources'
        folded.write_text('Types: deb\nURIs: https://security.ubuntu.com/ubuntu/\n'
                          ' https://azure.archive.ubuntu.com/ubuntu/\nSuites: noble\n'
                          'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')
        self.assertEqual(len(mirrors.configure(self.root)), 2)
        self.assertEqual(legacy.read_text(),
                         'deb [signed-by=/key.gpg] http://archive.ubuntu.com/ubuntu/ noble main\n'
                         'deb-src\thttp://archive.ubuntu.com/ubuntu/ noble-updates universe\n'
                         '# deb http://azure.archive.ubuntu.com/ubuntu/ noble main\n'
                         'deb https://archive.ubuntu.com.example.invalid/ubuntu noble main\n')
        self.assertEqual(folded.read_text(), 'Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\n'
                                             ' http://archive.ubuntu.com/ubuntu/\nSuites: noble\n'
                                             'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n')

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
