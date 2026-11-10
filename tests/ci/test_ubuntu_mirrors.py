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
            def do_HEAD(self):
                self.send_response(200)
                self.end_headers()

            def do_GET(self):
                self.send_response(200)
                self.send_header('Content-Length', str(2 * 1024 * 1024))
                self.end_headers()
                # Indexes can be cached and fast even on a slow package mirror.
                payload_delay = 0 if self.path.startswith("/ubuntu/dists/") else delay
                try:
                    for _ in range(256):
                        self.wfile.write(b'x' * 8192)
                        self.wfile.flush()
                        if payload_delay:
                            time.sleep(payload_delay)
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
        script = snippet(WORKFLOWS[0]).replace(
            'us.archive.ubuntu.com mirrors.edge.kernel.org azure.archive.ubuntu.com archive.ubuntu.com',
            ' '.join(hosts)).replace('/dev/tcp/$host/80', '/dev/tcp/${host%:*}/${host##*:}')
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', script],
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
        # The security pocket goes through the mirrors too; its host stalls with the archive's.
        self.assertEqual(source.read_text().count(f'URIs: mirror+file:{self.apt}/putgd-mirrors.txt\n'), 2)
        self.assertIn('Signed-By: /key.gpg', source.read_text())
        self.assertNotIn('security.ubuntu.com', source.read_text())
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

    def install(self, apt_get, seconds=30):
        """Run putgd_apt_install against a fake apt-get; returns (exit status, output, apt-get calls)."""
        for name, body in (('apt-get', apt_get), ('dpkg', 'echo "dpkg $*" >> "$CALLS"')):
            tool = self.bin / name
            tool.write_text('#!/bin/bash\nfirst=$(head -n 1 "$PUTGD_APT_ROOT/putgd-mirrors.txt" 2>/dev/null | cut -f1)\n'
                            + body + '\n')
            tool.chmod(0o755)
        calls = self.root / 'calls'
        calls.touch()
        self.archives = self.root / 'archives'
        env = dict(os.environ, PUTGD_APT_ROOT=str(self.apt), PUTGD_UBUNTU_MIRROR_HOSTS='',
                   PUTGD_APT_ATTEMPT_SECONDS=str(seconds), PUTGD_APT_ARCHIVES=str(self.archives),
                   PUTGD_APT_PROGRESS_KB='1024', CALLS=str(calls), PATH=f'{self.bin}:{os.environ["PATH"]}')
        script = 'apt=(-o Test=1)\n' + snippet(WORKFLOWS[0]) + 'putgd_apt_install --no-install-recommends one two\n'
        result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script], env=env, timeout=30,
                                capture_output=True, text=True)
        return result.returncode, result.stdout, calls.read_text().splitlines()

    def write_mirrors(self, *hosts, usable=None):
        (self.apt / 'putgd-mirrors.txt').write_text(
            ''.join(f'http://{host}/ubuntu/\tpriority:{n}\n' for n, host in enumerate(hosts, 1)))
        if usable is not None:
            (self.apt / 'putgd-mirrors.txt.usable').write_text(f'{usable}\n')

    def mirror_hosts(self):
        return [line.split('/')[2] for line in self.mirrors()]

    def test_install_passes_first_time_without_touching_the_mirror_list(self):
        self.write_mirrors('a', 'b', 'c')
        status, output, calls = self.install('echo "apt-get $*" >> "$CALLS"')
        self.assertEqual(status, 0)
        archives = f'-o Dir::Cache::Archives={self.archives}/'
        self.assertEqual(calls, ['apt-get update --error-on=any -o Test=1',
                                 f'apt-get install -y -o Test=1 {archives} --no-install-recommends one two',
                                 f'apt-get clean {archives}'])
        self.assertEqual(self.mirror_hosts(), ['a', 'b', 'c'])
        self.assertNotIn('::warning::', output)

    def test_stalled_first_mirror_is_stopped_and_demoted_and_the_index_is_not_fetched_again(self):
        self.write_mirrors('stalls', 'b', 'c')
        # Like a mirror that probed fast and then hangs on package downloads.
        status, output, calls = self.install(
            'echo "apt-get $1 $first" >> "$CALLS"\n'
            '[ "$1" != install ] || [ "$first" != http://stalls/ubuntu/ ] || exec sleep 20', seconds=1)
        self.assertEqual(status, 0)
        # Docker images delete the default archives after every apt-get update.
        self.assertEqual(calls, ['apt-get update http://stalls/ubuntu/', 'apt-get install http://stalls/ubuntu/',
                                 'dpkg --configure -a', 'apt-get install http://b/ubuntu/', 'apt-get clean http://b/ubuntu/'])
        self.assertEqual((self.apt / 'putgd-mirrors.txt').read_text(),
                         'http://b/ubuntu/\tpriority:1\nhttp://c/ubuntu/\tpriority:2\nhttp://stalls/ubuntu/\tpriority:3\n')
        self.assertIn('Ubuntu mirror moved behind the other usable mirrors: http://stalls/ubuntu/', output)
        self.assertEqual(output.count('::warning::Package install: attempt'), 1)
        self.assertIn('after fetching 0 MiB', output)

    def test_a_slow_mirror_that_is_delivering_keeps_first_place_and_its_downloads(self):
        self.write_mirrors('slow', 'worse')
        # Each attempt fetches 2 MiB and is stopped; the third finds the earlier downloads and finishes.
        status, output, calls = self.install(
            'echo "apt-get $1 $first" >> "$CALLS"\n[ "$1" = install ] || exit 0\n'
            'dir=${PUTGD_APT_ARCHIVES}\n[ "$(ls "$dir" | grep -c deb)" -lt 2 ] || exit 0\n'
            'head -c 2097152 /dev/zero > "$dir/$(date +%s%N).deb"; exec sleep 20', seconds=1)
        self.assertEqual(status, 0)
        self.assertEqual([call for call in calls if 'install' in call], ['apt-get install http://slow/ubuntu/'] * 3)
        self.assertEqual(self.mirror_hosts(), ['slow', 'worse'])
        self.assertEqual(output.count('after fetching 2 MiB'), 2)
        self.assertNotIn('Ubuntu mirror moved', output)

    def test_mirrors_that_gave_the_probe_nothing_are_never_rotated_to_the_front(self):
        self.write_mirrors('a', 'b', 'dead1', 'dead2', usable=2)
        status, output, calls = self.install('echo "apt-get $1 $first" >> "$CALLS"; [ "$1" != install ] || exit 100')
        self.assertEqual(status, 1)
        self.assertEqual([call for call in calls if 'install' in call],
                         [f'apt-get install http://{host}/ubuntu/' for host in 'aba'])
        self.assertEqual(self.mirror_hosts(), ['a', 'b', 'dead1', 'dead2'])
        self.assertEqual(output.count('::warning::Package install: attempt'), 3)

    def test_a_single_usable_mirror_is_not_demoted(self):
        self.write_mirrors('only', 'dead', usable=1)
        status, output, calls = self.install('echo "apt-get $1 $first" >> "$CALLS"; exit 100')
        self.assertEqual(status, 1)
        self.assertEqual(calls.count('apt-get update http://only/ubuntu/'), 3)
        self.assertNotIn('Ubuntu mirror moved', output)

    def test_install_retries_without_a_mirror_list(self):
        # Debian containers and Ubuntu images without archive.ubuntu.com have no list to rotate.
        status, _output, calls = self.install(
            'echo "apt-get $1" >> "$CALLS"; [ "$(grep -c "apt-get update" "$CALLS")" -gt 1 ]')
        self.assertEqual(status, 0)
        self.assertEqual(calls, ['apt-get update', 'dpkg --configure -a', 'apt-get update', 'apt-get install', 'apt-get clean'])
        self.assertFalse((self.apt / 'putgd-mirrors.txt').exists())

    def test_the_probe_records_how_many_mirrors_answered(self):
        with socket.socket() as dead:
            dead.bind(('127.0.0.1', 0))
            (self.apt / 'sources.list').write_text('deb http://archive.ubuntu.com/ubuntu noble main\n')
            self.run_snippet([f'127.0.0.1:{dead.getsockname()[1]}', self.server(), self.server(0.05)])
        self.assertEqual((self.apt / 'putgd-mirrors.txt.usable').read_text().strip(), '2')

    def test_ubuntu_jobs_install_through_the_bounded_helper(self):
        ci = WORKFLOWS[0].read_text()
        self.assertEqual(ci.count('putgd_apt_install "${packages[@]}"') + ci.count('putgd_apt_install build-essential clang'), 2)
        self.assertNotIn('apt-get install', ci.split('\njobs:\n')[1])
        self.assertIn('putgd_demote_mirror\n', WORKFLOWS[1].read_text().split('\njobs:\n')[1])
        self.assertIn('putgd_apt_install "${packages[@]}"', (ROOT / 'tests/desktop/pipewire/ci_check.sh').read_text())

    def test_non_ubuntu_sources_are_left_alone(self):
        source = self.apt / 'sources.list'
        original = 'deb http://deb.debian.org/debian trixie main\n'
        source.write_text(original)
        self.run_snippet([])
        self.assertEqual(source.read_text(), original)
        self.assertFalse((self.apt / 'putgd-mirrors.txt').exists())


if __name__ == '__main__':
    unittest.main()
