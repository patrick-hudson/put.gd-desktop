"""Ubuntu containers fail over to other mirrors when archive.ubuntu.com is down."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = (ROOT / '.github/workflows/ci.yml', ROOT / '.github/workflows/package.yml')


def snippet(workflow):
    """The PUTGD_UBUNTU_MIRRORS block, read without a YAML dependency."""
    lines = workflow.read_text().splitlines()
    start = lines.index('  PUTGD_UBUNTU_MIRRORS: |') + 1
    body = []
    for line in lines[start:]:
        if line and not line.startswith('    '):
            break
        body.append(line[4:])
    return '\n'.join(body).strip() + '\n'


class UbuntuMirrors(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.apt = self.root / 'apt'
        (self.apt / 'sources.list.d').mkdir(parents=True)
        self.bin = self.root / 'bin'
        self.bin.mkdir()

    def run_snippet(self, unreachable):
        """Run the snippet with hosts in `unreachable` refusing connections."""
        fake = self.bin / 'timeout'
        fake.write_text('#!/bin/bash\n'
                        f'for host in {" ".join(unreachable)}; do\n'
                        '  case "$*" in *"/dev/tcp/$host/80"*) exit 1;; esac\n'
                        'done\nexit 0\n')
        fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
        env = dict(os.environ, PUTGD_APT_ROOT=str(self.apt), PATH=f'{self.bin}:{os.environ["PATH"]}')
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', snippet(WORKFLOWS[0])], env=env, check=True,
                              capture_output=True, text=True).stdout

    def mirrors(self):
        return [line.split('\t')[0] for line in (self.apt / 'putgd-mirrors.txt').read_text().splitlines()]

    def test_workflows_share_one_snippet(self):
        self.assertEqual(snippet(WORKFLOWS[0]), snippet(WORKFLOWS[1]))

    def test_every_container_bootstrap_uses_it(self):
        for workflow in WORKFLOWS:
            text = workflow.read_text()
            self.assertEqual(text.count('apt=(-o Acquire::Retries=3'), text.count('eval "$PUTGD_UBUNTU_MIRRORS"'))
            self.assertNotIn('docker run --rm --volume', text)
        self.assertIn('eval "${PUTGD_UBUNTU_MIRRORS:-}"', (ROOT / 'tests/desktop/pipewire/ci_check.sh').read_text())

    def test_reachable_hosts_come_first_and_keep_every_mirror(self):
        source = self.apt / 'sources.list.d/ubuntu.sources'
        source.write_text('Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble\n'
                          'Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\n')
        output = self.run_snippet(['archive.ubuntu.com'])
        self.assertEqual(self.mirrors(), ['http://us.archive.ubuntu.com/ubuntu/',
                                          'http://azure.archive.ubuntu.com/ubuntu/',
                                          'http://mirrors.edge.kernel.org/ubuntu/',
                                          'http://archive.ubuntu.com/ubuntu/'])
        self.assertIn(f'URIs: mirror+file:{self.apt}/putgd-mirrors.txt\n', source.read_text())
        self.assertIn('URIs: http://security.ubuntu.com/ubuntu/\n', source.read_text())
        self.assertIn('Ubuntu mirrors reachable: us.archive.ubuntu.com', output)

    def test_mirror_priorities_are_explicit_so_apt_does_not_shuffle_them(self):
        (self.apt / 'sources.list').write_text('deb http://archive.ubuntu.com/ubuntu noble main\n')
        self.run_snippet([])
        lines = (self.apt / 'putgd-mirrors.txt').read_text().splitlines()
        self.assertEqual([line.split('\t')[1] for line in lines],
                         [f'priority:{number}' for number in range(1, 5)])
        self.assertEqual(self.mirrors()[0], 'http://archive.ubuntu.com/ubuntu/')

    def test_all_hosts_down_still_writes_a_list(self):
        (self.apt / 'sources.list').write_text('deb http://archive.ubuntu.com/ubuntu noble main\n')
        hosts = ['archive.ubuntu.com', 'us.archive.ubuntu.com', 'azure.archive.ubuntu.com', 'mirrors.edge.kernel.org']
        self.assertIn('Ubuntu mirrors reachable: none', self.run_snippet(hosts))
        self.assertEqual(len(self.mirrors()), 4)

    def test_non_ubuntu_sources_are_left_alone(self):
        source = self.apt / 'sources.list'
        original = 'deb http://deb.debian.org/debian trixie main\n'
        source.write_text(original)
        self.run_snippet([])
        self.assertEqual(source.read_text(), original)
        self.assertFalse((self.apt / 'putgd-mirrors.txt').exists())


if __name__ == '__main__':
    unittest.main()
