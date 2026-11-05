#!/usr/bin/env python3
"""#329 driver: a headless, isolated put.gd AppImage resident on a builder.

Private XDG dirs and runtime dir, offscreen Qt, no session bus. Prints JSON only.
Synthetic keys never reach a real account (TLS is checked before auth). Live keys
are never read by this file.
"""
import argparse, json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(os.environ.get('I329_ROOT', '/home/patrick/work/put.gd-i329')) / 'runs'
FAKE = {'accessKey': 'AKIAPUTGDI329SYNTHETIC', 'secretKey': 'putgd-i329-synthetic-secret-not-a-real-key'}


def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)


class Run:
    def __init__(self, name):
        self.dir = ROOT / name
        self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}

    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')

    def env(self, extra=None):
        env = {'HOME': os.environ['HOME'], 'PATH': '/usr/local/bin:/usr/bin:/bin', 'USER': os.environ.get('USER', 'patrick'),
               'LANG': 'C.UTF-8', 'QT_QPA_PLATFORM': 'offscreen', 'XDG_RUNTIME_DIR': self.meta['runtime'],
               'DBUS_SESSION_BUS_ADDRESS': 'disabled:'}
        for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_DATA_HOME', 'data')):
            env[key] = str(self.dir / sub)
        env.update(extra or {})
        return env

    def cli_env(self):
        env = self.env()
        appdir = Path(self.meta['appdir'])
        env['LD_LIBRARY_PATH'] = f'{appdir}/usr/lib:{appdir}/usr/lib/putgd'
        env['QT_PLUGIN_PATH'] = f'{appdir}/usr/plugins:{appdir}/usr/lib/qt6/plugins'
        return env

    def cli(self, method, params=None, timeout=90):
        r = subprocess.run([str(Path(self.meta['appdir']) / 'usr/bin/putgd-cli'), method, json.dumps(params or {})],
                           env=self.cli_env(), timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-600:], 'stderr': r.stderr[-600:], 'exit': r.returncode}

    def ok(self, method, params=None, timeout=90):
        r = self.cli(method, params, timeout)
        if not r.get('ok'): raise SystemExit(f'{method}: {json.dumps(r)[:900]}')
        return r['result']

    def socket_request(self, method, params):
        path = str(Path(self.meta['runtime']) / 'putgd/session.sock')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(30); s.connect(path)
            s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'i329-' + method, 'session': path, 'method': method, 'params': params}).encode() + b'\n')
            data = b''
            while not data.endswith(b'\n'):
                chunk = s.recv(65536)
                if not chunk: break
                data += chunk
        return json.loads(data)

    def wait(self, check, timeout, step=.4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = check()
            if value: return value
            time.sleep(step)
        return None


def profiles(run):
    tag = run.meta['name']
    r2 = json.loads(Path(run.meta['r2State']).read_text()) if run.meta.get('r2State') else {}
    return [
        {'credentials': {'mode': 'session', 'reference': None}, 'id': 'aws', 'label': 'AWS S3 (i329)', 'provider': 's3',
         'settings': {'aclMode': run.meta.get('acl', 'public-read'), 'addressing': 'path', 'bucket': 'put.gd', 'connectTimeoutMs': 10000,
                      'endpoint': 'https://s3.us-east-2.amazonaws.com', 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120,
                      'objectPrefix': run.meta.get('s3Prefix', 'acceptance/i329/') + f'{tag}/', 'publicUrlBase': None, 'region': 'us-east-2',
                      'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}},
        {'credentials': {'mode': 'session', 'reference': None}, 'id': 'r2', 'label': 'Cloudflare R2 (i329)', 'provider': 's3',
         'settings': {'aclMode': 'omit', 'addressing': 'path', 'bucket': r2.get('bucket', 'putgd-i329-r2-absent'), 'connectTimeoutMs': 10000,
                      'endpoint': r2.get('endpoint', 'https://example.r2.cloudflarestorage.com'), 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120,
                      'objectPrefix': f'put.gd/acceptance/i329/{tag}/', 'publicUrlBase': r2.get('publicBase'), 'region': 'auto',
                      'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}},
        {'credentials': {'mode': 'session', 'reference': None}, 'id': 'b2', 'label': 'Backblaze B2 (i329)', 'provider': 'backblaze-b2',
         'settings': {'bucket': run.meta.get('b2Bucket') or 'putgd-i329-absent', 'objectPrefix': f'put.gd/acceptance/i329/{tag}/',
                      'linkMode': 'public', 'linkValidityDays': 7, 'publicUrlBase': None, 'apiUrl': 'https://api.backblazeb2.com',
                      'connectTimeoutMs': 10000, 'requestTimeoutMs': 600000}},
    ]


def config_document(run, template):
    doc = dict(template)
    doc.update({'schemaVersion': 1,
                'actions': {n: {'destination': 'upload', 'profileId': run.meta.get('actionProfile', 'aws'), 'shortcut': ''} for n in ('clipboard', 'desktop', 'edit', 'region', 'window')} | {'record': {'shortcut': ''}},
                'archive': {'folders': {'images': None, 'other': None, 'text': None, 'videos': None}, 'keepUploads': True, 'root': str(run.dir / 'archive')},
                'profiles': profiles(run)})
    prefs = dict(template.get('preferences') or {})
    prefs.update({'launchAtLogin': False, 'notifications': False, 'uploadClipboardBehavior': 'each_link', 'unlockCredentialsAtStartup': False,
                  'updates': {'check': True, 'channel': 'nightly'}})
    doc['preferences'] = prefs
    return doc


def setup(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'data', 'archive', 'logs', 'in'):
        (run.dir / sub).mkdir(mode=0o700)
    image = Path(args.appimage).resolve()
    appdir = run.dir / 'appdir'; appdir.mkdir(mode=0o700)
    subprocess.run([str(image), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/i329.XXXXXX'], text=True).strip()
    sock = f'{runtime}/putgd/session.sock'
    if len(sock.encode()) > 107: raise SystemExit('socket path too long')
    run.meta = {'name': args.name, 'appimage': str(image), 'appdir': str(appdir / 'squashfs-root'), 'runtime': runtime,
                'host': os.uname().nodename, 'pids': {}, 'b2Bucket': args.b2_bucket, 'actionProfile': args.action_profile, 'acl': args.acl, 'r2State': args.r2_state and str(Path(args.r2_state).resolve())}
    run.save()
    p = launch(run)
    template = run.ok('settings.developmentTemplate')['document']
    (run.dir / 'config/putgd').mkdir(exist_ok=True)
    (run.dir / 'config/putgd/config.json').write_text(json.dumps(config_document(run, template), indent=1))
    run.ok('settings.reload')
    snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result']['metadata']['validation']['ok'] else None)(run.cli('settings.get')), 20)
    out({'setup': bool(snap), 'validation': (snap or {}).get('result', {}).get('metadata', {}).get('validation'), 'run': run.meta})


def launch(run, extra_env=None):
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    log = open(run.dir / 'logs/launch.log', 'ab')
    p = subprocess.Popen([run.meta['appimage'], '--background'], env=run.env(extra_env), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                         start_new_session=True)
    run.meta['pids']['apprun'] = p.pid; run.save()
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60)
    if not ready: raise SystemExit('resident not ready; see logs/launch.log')
    return p


def start(args):
    run = Run(args.name)
    extra = dict(kv.split('=', 1) for kv in args.env or [])
    launch(run, extra)
    out({'started': True, 'pid': run.meta['pids']['apprun'], 'env': extra})


def bundle(args):
    """PUTGD_CA_BUNDLE in the running resident's and its workers' environment (AppRun's export)."""
    run = Run(args.name)
    seen = {}
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try: env = (proc / 'environ').read_bytes(); comm = (proc / 'comm').read_text().strip()
        except OSError: continue
        if run.meta['runtime'].encode() not in env or not comm.startswith('putgd'): continue
        values = dict(kv.split('=', 1) for kv in env.decode(errors='replace').split('\0') if '=' in kv)
        seen[comm] = values.get('PUTGD_CA_BUNDLE', '(unset)')
    report = {'host': os.uname().nodename, 'PUTGD_CA_BUNDLE': seen}
    (run.dir / 'bundle.json').write_text(json.dumps(report, indent=1))
    out(report)


def stop(args):
    run = Run(args.name)
    r = run.cli('ui.quit', timeout=20)
    pid = run.meta['pids'].get('apprun')
    gone = run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40) if pid else True
    out({'quit': r.get('ok'), 'apprunGone': bool(gone)})


def update_check(args):
    run = Run(args.name)
    t = time.monotonic()
    r = run.cli('update.check', {}, timeout=120)
    status = run.cli('update.status', {}, timeout=30)
    out({'check': r, 'status': status, 'seconds': round(time.monotonic() - t, 2)})


def store(run, profile, credentials):
    revision = run.ok('settings.get')['metadata']['acceptedRevision']
    stored = run.socket_request('setup.storeCredentials', {'profileId': profile, 'expectedRevision': revision, 'credentials': credentials})
    if not stored.get('ok'): raise SystemExit('storeCredentials: ' + json.dumps(stored.get('error')))
    return run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': stored['result']['setupId']})), 30)


def test_destination(run, profile):
    revision = run.ok('settings.get')['metadata']['acceptedRevision']
    started = run.socket_request('setup.testDestination', {'profileId': profile, 'expectedRevision': revision})
    if not started.get('ok'): return started
    return run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': started['result']['setupId']})), 150, 1)


def synthetic(args):
    """Fake keys: a TLS failure shows up as a transport error, a working TLS stack as an auth refusal."""
    run = Run(args.name)
    job = store(run, args.profile, FAKE)
    t = time.monotonic()
    result = test_destination(run, args.profile)
    out({'profile': args.profile, 'store': job and job['result']['state'], 'test': result, 'seconds': round(time.monotonic() - t, 2)})


def synthetic_png(path, label):
    """A 640x360 synthetic test card (gradient plus magenta bars); no real content."""
    import struct, zlib
    w, h = 640, 360
    seed = sum(label.encode()) % 97
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            if (x // 40) % 4 == 0 and y < 60: rows += bytes((255, 0, 255))
            else: rows += bytes(((x * 255) // w, (y * 255) // h, (seed * 2 + x // 5) % 256))
    def chunk(kind, data): return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


def fetch_status(url):
    import urllib.request, urllib.error
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'put.gd-i329-acceptance'}), timeout=30) as r:
            body = r.read(); return r.status, body[:8] == b'\x89PNG\r\n\x1a\n', len(body)
    except urllib.error.HTTPError as e: return e.code, False, 0


def live(args):
    """Live keys arrive on stdin as JSON and go straight to the session socket; never printed or stored here."""
    run = Run(args.name)
    raw = json.loads(sys.stdin.read())
    creds = {'accessKey': raw.get('accessKey') or raw.get('keyId'), 'secretKey': raw.get('secretKey') or raw.get('applicationKey')}
    raw.clear()
    report = {'host': os.uname().nodename, 'profile': args.profile}
    job = store(run, args.profile, creds)
    creds.clear()
    report['store'] = job and job['result']['state']
    if args.test:
        t = time.monotonic(); r = test_destination(run, args.profile)
        report['testConnection'] = {'state': (r or {}).get('result', {}).get('state'), 'error': (r or {}).get('result', {}).get('error'), 'seconds': round(time.monotonic() - t, 2)}
    src = run.dir / 'in' / f'i329-{run.meta["name"]}-{args.profile}.png'
    synthetic_png(src, src.name)
    opened = run.ok('editor.open', {'source': 'file', 'path': str(src)}); eid = opened['operationId']
    run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
    t = time.monotonic()
    sub = run.ok('editor.submit', {'id': eid}); op_id = sub.get('operationId') or eid
    op = run.wait(lambda: (lambda r: r['result'] if r.get('ok') and r['result'].get('state') in ('completed', 'partial', 'failed', 'interrupted', 'cancelled') else None)(run.cli('operation.inspect', {'id': op_id})), 180, .5)
    items = (op or {}).get('items', [])
    item = items[0] if items else {}
    link = (item.get('receipt') or {}).get('directUrl')
    report['upload'] = {'state': (op or {}).get('state', 'timeout'), 'uploadStatus': item.get('uploadStatus'), 'error': (op or {}).get('error') or item.get('error'),
                        'link': link, 'seconds': round(time.monotonic() - t, 2)}
    if link:
        status, png, size = fetch_status(link); report['linkFetch'] = {'status': status, 'png': png, 'bytes': size}
    if args.delete and link:
        records = run.ok('history.list', {'limit': 20, 'offset': 0})['records']
        target = next((r for r in records if (r.get('receipt') or {}).get('directUrl') == link), None)
        if target:
            prep = run.ok('delete.prepare', {'ids': [target['id']]})
            conf = run.ok('delete.confirm', {'ids': [target['id']], 'acknowledgement': prep['acknowledgement']})
            did = conf.get('deleteOperationId')
            fin = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'partial', 'cancelled', 'failed') else None)(run.cli('delete.inspect', {'id': did})), 120)
            report['delete'] = {'state': (fin or {}).get('state'), 'items': [i.get('status') for i in (fin or {}).get('items', [])],
                                'remoteStatus': run.ok('history.detail', {'id': target['id']}).get('remoteStatus'), 'linkAfter': fetch_status(link)[0]}
    (run.dir / 'results.jsonl').open('a').write(json.dumps(report) + '\n')
    out(report)


def logs(args):
    run = Run(args.name)
    hits = []
    for path in sorted((run.dir / 'state').rglob('*.jsonl')):
        for line in path.read_text(errors='replace').splitlines():
            if any(n in line.lower() for n in args.needles):
                hits.append(line[:700])
    out(hits[-args.limit:])


def cleanup(args):
    run = Run(args.name)
    pid = run.meta.get('pids', {}).get('apprun')
    if pid and Path(f'/proc/{pid}').exists(): raise SystemExit(f'resident {pid} still runs; stop first')
    runtime = run.meta.get('runtime', '')
    # Keep the run's own (secret-free) results: run.json and the JSON reports.
    keep = ROOT.parent / 'results' / run.meta['name']
    keep.mkdir(parents=True, exist_ok=True)
    for f in list(run.dir.glob('*.json')) + list(run.dir.glob('*.jsonl')): shutil.copy2(f, keep / f.name)
    if runtime.startswith('/tmp/i329.') and Path(runtime).is_dir(): shutil.rmtree(runtime)
    if not args.keep_dir: shutil.rmtree(run.dir)
    out({'removed': [runtime] + ([] if args.keep_dir else [str(run.dir)])})


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('setup'); s.add_argument('name'); s.add_argument('--appimage', required=True); s.add_argument('--b2-bucket'); s.add_argument('--action-profile', default='aws'); s.add_argument('--acl', default='public-read'); s.add_argument('--r2-state'); s.set_defaults(f=setup)
    s = sub.add_parser('start'); s.add_argument('name'); s.add_argument('--env', action='append'); s.set_defaults(f=start)
    s = sub.add_parser('bundle'); s.add_argument('name'); s.set_defaults(f=bundle)
    s = sub.add_parser('stop'); s.add_argument('name'); s.set_defaults(f=stop)
    s = sub.add_parser('update-check'); s.add_argument('name'); s.set_defaults(f=update_check)
    s = sub.add_parser('synthetic'); s.add_argument('name'); s.add_argument('profile'); s.set_defaults(f=synthetic)
    s = sub.add_parser('live'); s.add_argument('name'); s.add_argument('profile'); s.add_argument('--test', action='store_true'); s.add_argument('--delete', action='store_true'); s.set_defaults(f=live)
    s = sub.add_parser('logs'); s.add_argument('name'); s.add_argument('needles', nargs='+'); s.add_argument('--limit', type=int, default=20); s.set_defaults(f=logs)
    s = sub.add_parser('cleanup'); s.add_argument('name'); s.add_argument('--keep-dir', action='store_true'); s.set_defaults(f=cleanup)
    a = ap.parse_args(); a.f(a)


if __name__ == '__main__':
    main()
