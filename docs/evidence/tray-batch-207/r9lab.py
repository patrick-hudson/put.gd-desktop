#!/usr/bin/env python3
"""Round-9 desktop checks for #206 and #207 on a physical desktop (m74 Cinnamon, a Plasma host).

Runs on the host, inside the real desktop session's display and bus, with a
private put.gd profile under ROOT (config, data, state, cache, runtime and
archive). The destination is the repository's offline S3 fixture on loopback
with its synthetic keys in session mode. Nothing is written outside ROOT except
what the desktop itself does (notifications, the tray, the clipboard).

  r9lab.py start ROOT            fixture + resident; prints {pids, port}
  r9lab.py batch ROOT            #207: six-file clipboard batch, concurrentUploads 3, SNI observations
  r9lab.py notify ROOT           #206: diagnostics.bundle with notify (a "saved" notification)
  r9lab.py stop ROOT             quit the resident, stop everything this run started
"""
import hashlib, json, os, signal, socket, subprocess, sys, time
from pathlib import Path

TREE = Path('/home/patrick/work/put.gd-r9small')
BUILD = TREE / 'build/dev'
CLI = BUILD / 'putgd-cli'
PROBE = TREE / 'build/tray-probe/putgd-tray-probe'
sys.path.insert(0, str(TREE / 'tools/lab'))
ACCESS, SECRET = 'PUTGD_SYNTHETIC_ACCESS', 'putgd-synthetic-secret-not-a-real-key'  # the fixture's own synthetic pair


def env(root):
    value = {k: v for k, v in os.environ.items() if k in ('HOME', 'PATH', 'LANG', 'DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY',
                                                         'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP',
                                                         'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'QT_QPA_PLATFORM')}
    for name, sub in (('XDG_CONFIG_HOME', 'c'), ('XDG_DATA_HOME', 'd'), ('XDG_STATE_HOME', 's'),
                      ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'r')):
        value[name] = str(root / sub)
    return value


def state(root):
    path = root / 'run.json'
    return json.loads(path.read_text()) if path.exists() else {'pids': {}}


def save(root, value):
    (root / 'run.json').write_text(json.dumps(value, indent=1))


def cli(root, method, params=None):
    out = subprocess.run([str(CLI), method, json.dumps(params or {})], env=env(root), capture_output=True, text=True, timeout=60)
    value = json.loads(out.stdout)
    if not value.get('ok'):
        raise SystemExit(f'{method}: {value.get("error")}')
    return value['result']


def socket_request(root, method, params):
    path = str(root / 'r/putgd/session.sock')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(15)
        s.connect(path)
        s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'r9lab-' + method, 'session': path,
                              'method': method, 'params': params}).encode() + b'\n')
        data = b''
        while not data.endswith(b'\n'):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data)


def fixture(port, path, body):
    import urllib.request
    request = urllib.request.Request(f'http://127.0.0.1:{port}{path}', data=json.dumps(body).encode(), method='POST')
    with urllib.request.urlopen(request, timeout=10) as reply:
        return reply.read()


def start(root):
    for sub in ('c/putgd', 'd', 's', 'cache', 'r', 'archive', 'out', 'files'):
        (root / sub).mkdir(parents=True, exist_ok=True)
    os.chmod(root / 'r', 0o700)
    run = state(root)
    server = subprocess.Popen([sys.executable, str(TREE / 'tests/fixtures/s3_server.py')], stdout=subprocess.PIPE,
                              stderr=open(root / 'fixture.log', 'w'), text=True, start_new_session=True)
    port = int(server.stdout.readline())
    run['pids']['fixture'] = server.pid
    run['port'] = port
    resident = subprocess.Popen([str(BUILD / 'putgd')], env=env(root), stdout=open(root / 'resident.log', 'w'),
                                stderr=subprocess.STDOUT, start_new_session=True)
    run['pids']['resident'] = resident.pid
    save(root, run)
    sock = root / 'r/putgd/session.sock'
    for _ in range(100):
        if sock.exists():
            break
        time.sleep(.2)
    time.sleep(1)
    template = cli(root, 'settings.developmentTemplate')['document']
    config = {
        'schemaVersion': 1, 'retention': template['retention'],
        'actions': {name: {'destination': 'upload', 'profileId': 'r9-fixture', 'shortcut': ''}
                    for name in ('clipboard', 'desktop', 'edit', 'region', 'window')} | {'record': {'shortcut': ''}},
        'archive': {'folders': {'images': None, 'other': None, 'text': None, 'videos': None}, 'keepUploads': False,
                    'root': str(root / 'archive')},
        'limits': {'clipboardTextBytes': 16777216, 'concurrentUploads': 3, 'imageBytes': 536870912, 'imagePixels': 100000000,
                   'maxBatchBytes': 'auto', 'maxFileBytes': 'auto', 'maxFiles': 100, 'pendingArtifactBytes': 'auto',
                   'pendingImageBytes': 67108864, 'pendingOperations': 8, 'recordingAnimationEdge': 1280,
                   'recordingAnimationSeconds': 60},
        'naming': {'captureTemplate': 'Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}', 'collisionPolicy': 'rename',
                   'fileTemplate': '{name}.{ext}', 'imageTemplate': 'Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}',
                   'mode': 'original', 'recordingTemplate': 'Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}',
                   'textTemplate': 'Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}'},
        'preferences': {'launchAtLogin': False, 'notifications': os.environ.get('R9_NOTIFY', '1') == '1', 'uploadClipboardBehavior': 'list',
                        'unlockCredentialsAtStartup': False},
        'profiles': [{'credentials': {'mode': 'session', 'reference': None}, 'id': 'r9-fixture',
                      'label': 'Synthetic fixture', 'provider': 's3',
                      'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'fixture',
                                   'connectTimeoutMs': 5000, 'endpoint': f'http://127.0.0.1:{port}',
                                   'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120, 'objectPrefix': 'r9/',
                                   'publicUrlBase': f'http://127.0.0.1:{port}/fixture', 'region': 'us-east-1',
                                   'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}}],
    }
    (root / 'c/putgd/config.json').write_text(json.dumps(config, indent=1))
    cli(root, 'settings.reload')
    for _ in range(100):
        try:
            snap = cli(root, 'settings.get')
            if snap['metadata']['validation']['ok']:
                break
        except (SystemExit, ValueError):
            pass
        time.sleep(.2)
    revision = cli(root, 'settings.get')['metadata']['acceptedRevision']
    stored = socket_request(root, 'setup.storeCredentials', {'profileId': 'r9-fixture', 'expectedRevision': revision,
                            'credentials': {'accessKey': ACCESS, 'secretKey': SECRET}})
    if not stored.get('ok'):
        raise SystemExit('storeCredentials: ' + json.dumps(stored.get('error')))
    setup = stored['result']['setupId']
    for _ in range(100):
        job = cli(root, 'setup.inspect', {'id': setup})
        if job['state'] != 'running':
            break
        time.sleep(.2)
    print(json.dumps({'pids': run['pids'], 'port': port, 'credentials': job['state']}))


def observe(root, pid):
    """One bracketed observation: CLI, SNI probe, CLI (tray_progress.read_observation)."""
    before = cli(root, 'operation.recent')['operations']
    if os.environ.get('R9_DRY'):
        snapshot = {}
    else:
        probe = subprocess.run([str(PROBE), '--pid', str(pid)], env=env(root), capture_output=True, text=True, timeout=10)
        snapshot = json.loads(probe.stdout or '{}')
    after = cli(root, 'operation.recent')['operations']
    return before, snapshot, after


# Theme::trayTints(lightPanel=true) and its dim remainder, mapped onto the dark
# (default) family tray_progress.check_tooltip expects (#207 follow-up).
LIGHT_TO_DARK = {'#2759b8': '#9bbcff', '#146c42': '#62d69b', '#092f1d': '#2b5d43'}


def light_adapter(snapshot):
    """R9_LIGHT_PANEL=1: rename the light-panel state colours to their dark-panel twins.

    Only those three exact colours are renamed; anything else is dropped, as the
    check only counts the family colours. Tooltip text, counters and SNI identity
    checks are unchanged.
    """
    if not os.environ.get('R9_LIGHT_PANEL'):
        return snapshot
    icons = [{**icon, 'opaqueColors': {LIGHT_TO_DARK[c]: n for c, n in icon.get('opaqueColors', {}).items() if c in LIGHT_TO_DARK}}
             for icon in snapshot.get('icons', [])]
    return {**snapshot, 'icons': icons}


def batch(root, files=6, mib=4):
    import tray_progress as tp
    run = state(root)
    port, pid = run['port'], run['pids']['resident']
    paths = []
    for index in range(1, files + 1):
        path = root / 'files' / f'r9-batch-{index}.bin'
        block = hashlib.sha256(f'r9 synthetic {index}'.encode()).digest() * (1024 * 1024 // 32)
        path.write_bytes(block * mib)
        paths.append(path)
    # Hold PUTs so files finish one at a time: three start, the first finishes at
    # 12 s, the next at 24 s... (the fixture applies rules in order).
    fixture(port, '/__reset', {})
    fixture(port, '/__rules', {'rules': [{'method': 'PUT', 'delay': 12 * (i + 1), 'remaining': 1} for i in range(files)]})
    uris = ''.join('file://' + str(p) + '\r\n' for p in paths)
    clip = subprocess.Popen(['xclip', '-selection', 'clipboard', '-t', 'text/uri-list'] if os.environ.get('XDG_SESSION_TYPE') != 'wayland'
                            else ['wl-copy', '--type', 'text/uri-list', '--foreground'],
                            stdin=subprocess.PIPE, env=env(root), start_new_session=True)
    clip.stdin.write(uris.encode()); clip.stdin.close()
    run['pids']['clipboard'] = clip.pid
    save(root, run)
    time.sleep(.5)
    operation_id = cli(root, 'action.invoke', {'action': 'clipboard'})['operationId']
    log = (root / 'observations.jsonl').open('a')
    history, rows_seen, matched, mismatched = [], {}, 0, 0
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        before, snapshot, after = observe(root, pid)
        now = time.monotonic()
        before_rows, after_rows = tp.records_from(before), tp.records_from(after)
        history.append((now, before_rows + after_rows))
        history = [(t, r) for t, r in history if now - t <= 2]
        recent = tp.correlated_records([row for _, rows in history for row in rows], after_rows)
        entry = {'t': round(now, 2), 'title': snapshot.get('title'), 'tooltip': snapshot.get('description'),
                 'registered': snapshot.get('registered'), 'hostRegistered': snapshot.get('hostRegistered'),
                 'probePid': snapshot.get('pid'),
                 'iconColors': [sorted(icon.get('opaqueColors', {}).items(), key=lambda kv: -kv[1])[:4] for icon in snapshot.get('icons', [])],
                 'cli': [{k: r[k] for k in ('phase', 'running', 'waiting', 'done', 'failed', 'batchTransferred', 'batchTotal')}
                         for r in after_rows]}
        try:
            if os.environ.get('R9_DRY'):
                raise tp.ObservationMismatch('dry run: no tray host')
            checked = tp.check_tooltip(light_adapter(snapshot), recent, 's3', pid)
            entry['check'] = {'matched': True, **checked}
            matched += 1
            text = snapshot.get('description', '')
            if ' uploading' in text:
                rows_seen[text.split(' · ', 1)[-1]] = rows_seen.get(text.split(' · ', 1)[-1], 0) + 1
        except tp.ObservationMismatch as error:
            entry['check'] = {'matched': False, 'mismatch': str(error)}
            mismatched += 1
        except tp.TrayProgressError as error:
            entry['check'] = {'matched': False, 'fatal': str(error)}
            log.write(json.dumps(entry) + '\n'); log.flush()
            raise SystemExit('fatal: ' + str(error))
        log.write(json.dumps(entry) + '\n'); log.flush()
        mine = next((op for op in after if op['id'] == operation_id), None)
        if mine and mine['state'] in tp.TERMINAL and not after_rows and (
                os.environ.get('R9_DRY') or 'Working' not in (snapshot.get('title') or 'Working')):
            break
        time.sleep(.4)
    final = cli(root, 'operation.inspect', {'id': operation_id})
    summary = {'operationId': operation_id, 'finalState': final.get('state'), 'error': final.get('error'), 'matched': matched, 'mismatched': mismatched,
               'batchRows': rows_seen,
               'items': [{k: item.get(k) for k in ('uploadStatus', 'phase')} for item in final.get('items', [])]}
    (root / 'batch-summary.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))


def notify(root):
    out = root / 'out'
    result = cli(root, 'diagnostics.bundle', {'notify': True, 'output': str(out / time.strftime('putgd-diagnostics-%Y%m%d-%H%M%SZ.zip', time.gmtime())), 'since': '1h'})
    print(json.dumps({k: result.get(k) for k in ('path', 'bytes', 'notified')} | {'keys': sorted(result)}))


def stop(root):
    run = state(root)
    try:
        cli(root, 'ui.quit')
    except (SystemExit, ValueError, subprocess.SubprocessError, OSError):
        pass
    time.sleep(2)
    for name, pid in run['pids'].items():
        try:
            os.kill(pid, 0)
        except OSError:
            continue
        os.kill(pid, signal.SIGTERM)
        print('stopped', name, pid)


if __name__ == '__main__':
    command, root = sys.argv[1], Path(sys.argv[2]).resolve()
    {'start': start, 'batch': batch, 'notify': notify, 'stop': stop}[command](root)
