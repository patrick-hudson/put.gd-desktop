"""Private loopback APT acceptance: official mirror ordering, failover and signatures.

Only the transport URIs are mapped to loopback AFTER the production rewrite.
No installed-package or system-APT state is changed. Fixture signing keys are
temporary, synthetic, and never exported except for their public verification key.
"""
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading

source, output = map(Path, sys.argv[1:])
output.mkdir(exist_ok=True)
spec = importlib.util.spec_from_file_location('mirrors', source / 'tools/ci/hosted_apt_mirrors.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='pgapt.') as temporary:
    root = Path(temporary)
    home = root / 'gpg'
    home.mkdir(mode=0o700)
    def gpg(*args):
        return subprocess.run(['gpg', '--homedir', str(home), '--batch', '--yes', *args],
                              check=True, capture_output=True).stdout
    gpg('--pinentry-mode', 'loopback', '--passphrase', '', '--quick-generate-key',
        'putgd synthetic mirror fixture', 'ed25519', 'sign', '0')
    (root / 'public.gpg').write_bytes(gpg('--export'))
    release = ('Origin: putgd synthetic mirror fixture\nLabel: putgd fixture\nSuite: noble\n'
               'Codename: noble\nArchitectures: amd64\nComponents: main\nSHA256:\n '
               + hashlib.sha256(b'').hexdigest() + ' 0 main/binary-amd64/Packages\n')
    (root / 'Release').write_text(release)
    gpg('--output', str(root / 'InRelease'), '--clearsign', str(root / 'Release'))
    signed = (root / 'InRelease').read_bytes()
    requests = []
    corrupt = False
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith('/archive/'):
                self.send_error(503, 'Synthetic primary mirror outage')
                return
            if self.path == '/security/ubuntu/dists/noble/InRelease':
                body = signed.replace(b'Label: putgd fixture', b'Label: tampered data') if corrupt else signed
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_error(404)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        apt = root / 'apt'
        apt.mkdir()
        (apt / 'apt-mirrors.txt').write_text('http://azure.archive.ubuntu.com/ubuntu/\tpriority:1\n'
            'https://archive.ubuntu.com/ubuntu/\tpriority:2\nhttps://security.ubuntu.com/ubuntu/\tpriority:3\n')
        module.configure(apt)
        chosen = (apt / 'apt-mirrors.txt').read_text()
        assert 'azure' not in chosen
        (output / 'official-mirrors.txt').write_text(chosen)
        port = server.server_port
        (apt / 'apt-mirrors.txt').write_text(chosen.replace('https://archive.ubuntu.com/ubuntu/',
            f'http://127.0.0.1:{port}/archive/ubuntu/').replace('https://security.ubuntu.com/ubuntu/',
            f'http://127.0.0.1:{port}/security/ubuntu/'))
        (root / 'sources').write_text(f'deb [signed-by={root}/public.gpg] '
                                      f'mirror+file:{apt}/apt-mirrors.txt noble main\n')
        results = []
        for label in ('signed-fallback', 'tampered-signature'):
            lists = root / label
            (lists / 'partial').mkdir(parents=True)
            command = ['apt-get', 'update', '--error-on=any', '-o', f'Dir::Etc::sourcelist={root}/sources',
                       '-o', 'Dir::Etc::sourceparts=-', '-o', f'Dir::State::Lists={lists}',
                       '-o', f'Dir::Cache={root}/cache', '-o', 'Acquire::Languages=none',
                       '-o', 'Acquire::Retries=0', '-o', 'Acquire::http::Timeout=2',
                       '-o', 'Debug::pkgAcquire::Auth=true']
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            (output / (label + '.log')).write_text(result.stdout + result.stderr)
            results.append({'case': label, 'exitCode': result.returncode})
            if not corrupt:
                assert result.returncode == 0, result.stdout + result.stderr
                assert any(path.startswith('/archive/') for path in requests)
                assert any(path.startswith('/security/') for path in requests)
                assert 'Signature verification succeeded' in result.stdout + result.stderr
            else:
                assert result.returncode != 0
                assert any(text in result.stdout + result.stderr for text in ('BADSIG', 'Message has been manipulated'))
            corrupt = True
        (output / 'result.json').write_text(json.dumps({'cases': results, 'httpRequests': requests,
            'scope': 'Private signed loopback repository; official URI transport mapped to loopback after rewrite; no system APT changes'}, indent=2) + '\n')
        print(json.dumps(results))
    finally:
        server.shutdown()
