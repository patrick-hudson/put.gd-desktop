#!/usr/bin/env python3
"""#560 scoped UNSCORED native functional pass on physical Cinnamon/X11 (m74), exact installed package.
Private profile under OUT; synthetic PNGs; loopback fixture with synthetic credentials only.
Editor uploads (verified via the frozen verify_uploaded), queued cancel with a fresh pre-cancel witness,
History + Settings visuals (rfb loopback 5900), normal window close, normal ui.quit."""
import json, os, socket, subprocess, sys, time, types, uuid, hashlib
from pathlib import Path
from urllib.request import Request, urlopen
DRV = Path('/home/patrick/work/i560o/driver/tests/performance'); sys.path.insert(0, str(DRV)); sys.path.insert(0, '/home/patrick/work/i560o/driver/tools/lab')
from campaign_workloads import upload_source, submit_file_upload, verify_uploaded
from campaign_fixture import png
from rfb import RFB
out = Path(sys.argv[1]); out.mkdir(mode=0o700)
events = []
def event(kind, **v): events.append({'kind': kind, 't': round(time.monotonic(), 3), **v}); print(json.dumps(events[-1])[:300], flush=True)
env = os.environ.copy()
for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state'), ('XDG_RUNTIME_DIR', 'runtime')]:
    p = out / name; p.mkdir(mode=0o700); env[key] = str(p)
sock = str(out / 'runtime/putgd/session.sock')
subprocess.run([sys.executable, '/home/patrick/work/i560o/driver/tools/diagnostics/check_socket_paths.py', '--socket', sock], check=True, stdout=(out / 'preflight.json').open('w'))
class Run:
    root = out
    def call(self, method, params=None):
        with socket.socket(socket.AF_UNIX) as c:
            c.settimeout(30); c.connect(sock)
            c.sendall(json.dumps({'schemaVersion': 1, 'requestId': str(uuid.uuid4()), 'session': sock, 'method': method, 'params': params or {}}).encode() + b'\n')
            data = bytearray()
            while not data.endswith(b'\n'):
                chunk = c.recv(65536)
                if not chunk: raise RuntimeError('transport')
                data.extend(chunk)
        r = json.loads(data)
        if not r.get('ok'):
            from bench_ui_process import EvidenceError
            raise EvidenceError('cli_' + r['error']['code'])
        return r['result']
    def event(self, kind, **v): event(kind, **v)
    def sample(self, label): pass
run = Run()
fx = subprocess.Popen([sys.executable, str(DRV.parent / 'fixtures/s3_server.py')], cwd=out, stdout=subprocess.PIPE, stderr=(out / 'fixture.err').open('w'), start_new_session=True)
port = int(fx.stdout.readline()); run.args = types.SimpleNamespace(endpoint=f'http://127.0.0.1:{port}')
def fixture(path, value):
    with urlopen(Request(run.args.endpoint + path, data=json.dumps(value).encode(), headers={'Content-Type': 'application/json'}), timeout=10) as r: return json.load(r)
log = (out / 'resident.log').open('wb')
resident = None
def shot(label):
    c = RFB(5900); c.screenshot(str(out / f'{label}.png')); c.close()
    event('visual', label=label, sha256=hashlib.sha256((out / f'{label}.png').read_bytes()).hexdigest())
def start():
    global resident
    resident = subprocess.Popen(['/usr/bin/putgd', '--background'], env=env, stdout=log, stderr=log, start_new_session=True)
    deadline = time.monotonic() + 25
    while True:
        try:
            if run.call('diagnostics')['journalReady']: return
        except Exception: pass
        if time.monotonic() > deadline: raise RuntimeError('readiness')
        time.sleep(.1)
def ui_ready(view):
    assert run.call('ui.show', {'view': view})['shown'] == view
    deadline = time.monotonic() + 20
    while True:
        s = run.call('ui.snapshot')['diagnostics']['ui']
        if s.get('state') == 'ready': return s
        if time.monotonic() > deadline: raise RuntimeError('ui_not_ready_' + view)
        time.sleep(.1)
result = {'exit': None}
try:
    start()
    event('build', build=run.call('diagnostics').get('build') or json.loads(subprocess.check_output(['/usr/bin/putgd', '--version-json'])))
    doc = run.call('settings.developmentTemplate')['document']; profile = doc['profiles'][0]
    profile.update(id='isolated-fixture', label='Synthetic loopback', credentials={'mode': 'session', 'reference': None})
    profile['settings'].update(endpoint=run.args.endpoint, bucket='fixture', region='us-east-1', publicUrlBase=run.args.endpoint + '/fixture', objectPrefix='i560f/', aclMode='public-read', requestTimeoutMs=120000, lowSpeedTimeSeconds=120)
    doc['profiles'] = [profile]; doc['archive'].update(root=str(out / 'archive'), keepUploads=True)
    doc['preferences'].update(notifications=False, launchAtLogin=False, updates={'check': False, 'channel': 'build'})
    doc['limits'].update(pendingOperations=64, concurrentUploads=1); doc['naming']['mode'] = 'original'
    for action in doc['actions'].values():
        if 'destination' in action: action.update(destination='upload', profileId='isolated-fixture', shortcut='')
    run.call('settings.patch', {'patch': doc, 'expectedRevision': run.call('settings.get')['metadata']['observedRevision']})
    run.call('setup.storeCredentials', {'profileId': 'isolated-fixture', 'expectedRevision': run.call('settings.get')['metadata']['acceptedRevision'], 'credentials': {'accessKey': 'PUTGD_SYNTHETIC_ACCESS', 'secretKey': 'putgd-synthetic-secret-not-a-real-key'}})
    fixture('/__rules', {'rules': [{'method': 'PUT', 'delay': 6, 'remaining': 10}]})
    media = out / 'burst-media'; media.mkdir(mode=0o700); ops = []; sources = []
    for i in range(3):
        p = media / f'queue-{i:02d}-synthetic.png'; png(p, 500 + i, 320, 180); p.chmod(0o400)
        s = upload_source(p, out); sources.append(s); ops.append(submit_file_upload(run, s))
    # Concurrency 1: the last submission is queued behind the first. Fresh witness, then cancel.
    target = ops[-1]; witness = run.call('operation.inspect', {'id': target})
    event('pre-cancel-witness', id=target, state=witness['state'], phase=witness.get('phase'), items=len(witness['items']))
    assert witness.get('phase') == 'queued' and not any(i.get('uploadStatus') == 'uploading' for i in witness['items']), 'target_not_queued_at_cancel'
    run.call('operation.cancel', {'id': target})
    for key, s in zip(ops, sources):
        deadline = time.monotonic() + 90
        while True:
            op = run.call('operation.inspect', {'id': key})
            if op['state'] in ('completed', 'failed', 'partial', 'cancelled'): break
            if time.monotonic() > deadline: raise RuntimeError('upload_timeout')
            time.sleep(.2)
        event('terminal', id=key, state=op['state'])
        if key == target: assert op['state'] == 'cancelled', op['state']
        else: verify_uploaded(run, op, s)
    s = ui_ready('history'); time.sleep(2); shot('history'); ui_pid = s['pid']
    s = ui_ready('settings'); time.sleep(2); shot('settings')
    subprocess.run(['wmctrl', '-l', '-p'], stdout=(out / 'wmctrl-before-close.txt').open('w'))
    wins = [l.split()[0] for l in (out / 'wmctrl-before-close.txt').read_text().splitlines() if len(l.split()) > 2 and l.split()[2] == str(ui_pid)]
    event('ui-windows', pid=ui_pid, windows=wins)
    for w in wins: subprocess.run(['wmctrl', '-i', '-c', w], check=True)
    deadline = time.monotonic() + 20
    while run.call('ui.snapshot')['diagnostics']['ui'].get('state') != 'stopped':
        if time.monotonic() > deadline: raise RuntimeError('ui_close_not_normal')
        time.sleep(.1)
    event('ui-closed-normally'); time.sleep(1); shot('after-close')
    run.call('ui.quit'); resident.wait(timeout=60); result['exit'] = resident.returncode
    event('resident-exit', code=resident.returncode, socketAbsent=not Path(sock).exists())
    assert resident.returncode == 0
    result['status'] = 'pass'
except BaseException as e:
    result['status'] = 'fail'; result['error'] = f'{type(e).__name__}: {e}'; raise
finally:
    if resident and resident.poll() is None:
        try: run.call('ui.quit'); resident.wait(timeout=30)
        except Exception: resident.terminate(); resident.wait(timeout=30); event('cleanup-fallback')
    fx.terminate(); fx.wait(timeout=10)
    left = subprocess.run(['pgrep', '-u', str(os.getuid()), '-f', str(out)], capture_output=True, text=True).stdout.split()
    left = [x for x in left if x != str(os.getpid())]
    result.update(events=events, remainingOwned=left)
    (out / 'functional-results.json').write_text(json.dumps(result, indent=1))
