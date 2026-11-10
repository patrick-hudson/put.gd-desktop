"""Ubuntu containers fail over to other mirrors when archive.ubuntu.com is down."""
import os
from pathlib import Path
import http.server
import socket
import threading
import time
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

    def server(self, delay=0):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', str(2 * 1024 * 1024))
                self.end_headers()
                try:
                    for _ in range(256):
                        self.wfile.write(b'x' * 8192)
                        self.wfile.flush()
                        if delay:
                            time.sleep(delay)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return f'127.0.0.1:{server.server_port}'

    def run_snippet(self, hosts):
        env = dict(os.environ, PUTGD_APT_ROOT=str(self.apt),
                   PUTGD_UBUNTU_MIRROR_HOSTS=' '.join(hosts))
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', snippet(WORKFLOWS[0])],
                              env=env, check=True, timeout=16,
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

    def test_http_200_trickle_is_demoted_below_fast_mirror(self):
        slow, fast = self.server(0.05), self.server()
        source = self.apt / 'sources.list.d/ubuntu.sources'
        source.write_text('Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\n'
                          'Suites: noble\nSigned-By: /key.gpg\n'
                          'URIs: http://security.ubuntu.com/ubuntu/\n')
        output = self.run_snippet([slow, fast])
        self.assertEqual(self.mirrors(), [f'http://{fast}/ubuntu/', f'http://{slow}/ubuntu/'])
        self.assertIn('below-floor', output)
        self.assertIn(f'Ubuntu mirror throughput: {fast}', output)
        self.assertIn('B/s (2097152 bytes', output)
        self.assertIn(f'URIs: mirror+file:{self.apt}/putgd-mirrors.txt', source.read_text())
        self.assertIn('Signed-By: /key.gpg', source.read_text())
        self.assertIn('URIs: http://security.ubuntu.com/ubuntu/', source.read_text())
        self.assertEqual([line.split('\t')[1] for line in
                          (self.apt / 'putgd-mirrors.txt').read_text().splitlines()],
                         ['priority:1', 'priority:2'])

    def test_dead_mirror_stays_last(self):
        with socket.socket() as dead:
            dead.bind(('127.0.0.1', 0))
            endpoint = f'127.0.0.1:{dead.getsockname()[1]}'
            fast = self.server()
            (self.apt / 'sources.list').write_text('deb http://archive.ubuntu.com/ubuntu noble main\n')
            output = self.run_snippet([endpoint, fast])
        self.assertEqual(self.mirrors(), [f'http://{fast}/ubuntu/', f'http://{endpoint}/ubuntu/'])
        self.assertIn(f'{endpoint} 0 B/s', output)

    def test_all_slow_still_yields_usable_speed_ordered_list(self):
        slower, slow = self.server(0.1), self.server(0.04)
        (self.apt / 'sources.list').write_text('deb http://archive.ubuntu.com/ubuntu noble main\n')
        output = self.run_snippet([slower, slow])
        self.assertEqual(self.mirrors(), [f'http://{slow}/ubuntu/', f'http://{slower}/ubuntu/'])
        self.assertEqual(output.count('below-floor'), 2)
        self.assertIn('mirror+file:', (self.apt / 'sources.list').read_text())

    def test_non_ubuntu_sources_are_left_alone(self):
        source = self.apt / 'sources.list'
        original = 'deb http://deb.debian.org/debian trixie main\n'
        source.write_text(original)
        self.run_snippet([])
        self.assertEqual(source.read_text(), original)
        self.assertFalse((self.apt / 'putgd-mirrors.txt').exists())


if __name__ == '__main__':
    unittest.main()
