#!/usr/bin/env python3
"""Linux update profile driver for DEB and RPM installs (U2-U6, U13-U15), on a desktop's host or in a VM guest.

From #386's acc386.py. Runs from SSH on the host, stdlib only plus afa.py, rfb.py and ua_app.py beside it.
put.gd is the installed package (/usr/bin/putgd). Each run lives in $UA_ROOT/runs/NAME (default
~/work/put.gd-ua/pkg) with its own runtime directory, and either
  --profile private  private XDG_CONFIG/STATE/CACHE/DATA_HOME in the run (U4, U6, U14), or
  --profile default  put.gd's default profile, as a user has it (U5); cleanup removes only the folders the run created.
--feed live reads app.put.gd as a user does; --feed lab sets PUTGD_UPDATE_FEED_URL=https://pkg.put.gd/lab/update/v1/.
The repository file is the system's real one; this driver never writes it, and every package command is run by
hand, as the user would. Full-desktop frames come from the host's loopback wall VNC server and stay on the host.

  ua_pkg.py start NAME --vnc-port P --label L [--profile private|default] [--feed live|lab] [--input M] [--notifications]
  ua_pkg.py check NAME --label L             Check now, Settings -> Updates, frame
  ua_pkg.py frame NAME --label L             a frame only (another app in front)
  ua_pkg.py click NAME X Y [--label L] / key NAME KEY... [--label L]
  ua_pkg.py open-updates NAME --label L      update.openSoftwareUpdates (the button's method); which updater started
  ua_pkg.py repo                             the repository file, the package, apt/dnf's view
  ua_pkg.py snapshot NAME --label L          config.json, history (entries, previews, links), the running build
  ua_pkg.py live-s3 NAME --prefix P          live S3 profile (public-read) for every action; key set 2 on stdin
  ua_pkg.py upload NAME [--label L]          a synthetic PNG through Edit & upload; the link and an HTTPS GET of it
  ua_pkg.py delete NAME                      Delete from destination on the uploaded entry
  ua_pkg.py watch NAME start|stop --label L  notifications on the session bus (ua_app.py watch)
  ua_pkg.py restart NAME --label L           quit and start the resident again (after a package upgrade)
  ua_pkg.py mem NAME                         idle RSS window (ua_app.py's method)
  ua_pkg.py quit NAME / cleanup NAME
"""
import argparse, hashlib, json, os, shutil, signal, subprocess, sys, time, urllib.error, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import afa  # noqa: E402  #415's guarded copy (docs/evidence/2026-10-22-rail-391-notify-415/lab/afa.py)
import ua_app  # noqa: E402  frame, notifications, window, synthetic_png, safe_handler
ROOT = Path(os.environ.get('UA_ROOT', str(Path.home() / 'work/put.gd-ua/pkg')))
LAB = 'https://pkg.put.gd/lab'
APT = Path('/etc/apt/sources.list.d/putgd.sources')
DNF = Path('/etc/yum.repos.d/putgd.repo')
DEFAULT_DIRS = {'XDG_CONFIG_HOME': '.config', 'XDG_STATE_HOME': '.local/state', 'XDG_CACHE_HOME': '.cache', 'XDG_DATA_HOME': '.local/share'}
UPDATERS = ('mintupdate', 'plasma-discover', 'gnome-software', 'update-manager')


def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)


class Run:
    def __init__(self, name):
        self.name, self.dir = name, ROOT / 'runs' / name
        self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}

    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')

    def env(self):
        env = dict(self.meta['session'])
        env['PATH'] = self.meta['hostPath']
        env['PIPEWIRE_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        if self.meta['profile'] == 'private':
            for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_DATA_HOME', 'data')):
                env[key] = str(self.dir / sub)
            env['XDG_RUNTIME_DIR'] = self.meta['runtime']
        else:
            env['XDG_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        if self.meta['feed'] == 'lab': env['PUTGD_UPDATE_FEED_URL'] = LAB + '/update/v1/'
        return env

    @property
    def config_home(self):
        return Path(self.env().get('XDG_CONFIG_HOME') or Path.home() / '.config')

    @property
    def state_home(self):
        return Path(self.env().get('XDG_STATE_HOME') or Path.home() / '.local/state')

    def cli(self, method, params=None, timeout=90):
        r = subprocess.run(['/usr/bin/putgd-cli', method, json.dumps(params or {})], env=self.env(), timeout=timeout,
                           capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}

    def ok(self, method, params=None, timeout=90):
        r = self.cli(method, params, timeout)
        if not r.get('ok'): raise SystemExit(f'{method}: {json.dumps(r)[:800]}')
        return r['result']

    def wait(self, check, timeout, step=.5):
        end = time.time() + timeout
        while time.time() < end:
            value = check()
            if value: return value
            time.sleep(step)
        return None

    def record(self, key, value):
        self.meta.setdefault('log', []).append({'at': afa.iso(), key: value}); self.save()
        return {key: value}


def resident_pid(run):
    runtime = run.env()['XDG_RUNTIME_DIR']
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            if (proc / 'comm').read_text().strip() != 'putgd': continue
            environ = (proc / 'environ').read_bytes().split(b'\0')
            if f'XDG_RUNTIME_DIR={runtime}'.encode() not in environ: continue
            if run.meta['profile'] == 'private' or not any(e.startswith(b'XDG_CONFIG_HOME=') for e in environ): return int(proc.name)
        except OSError: continue
    return None


def repo():
    file = APT if Path('/etc/apt').exists() else DNF
    info = {'file': str(file), 'exists': file.exists()}
    if file.exists():
        data = file.read_bytes()
        info['sha256'] = hashlib.sha256(data).hexdigest()
        for channel in ('nightly', 'stable'):
            copy = Path(f'/usr/share/putgd/repo/putgd-{channel}.' + ('sources' if file == APT else 'repo'))
            if copy.exists() and copy.read_bytes() == data: info['sameAs'] = str(copy)
        info['lines'] = [l for l in data.decode().splitlines() if l.startswith(('Suites', 'URIs', 'baseurl', '[', 'gpgkey', 'Enabled', 'enabled'))]
    if file == APT:
        info['installed'] = subprocess.run(['dpkg-query', '-W', '-f=${Version}', 'putgd'], capture_output=True, text=True).stdout
        info['policy'] = subprocess.run(['apt-cache', 'policy', 'putgd'], capture_output=True, text=True).stdout.splitlines()
        info['conffiles'] = subprocess.run(['dpkg-query', '-W', '-f=${Conffiles}', 'putgd'], capture_output=True, text=True).stdout.strip()
    else:
        info['installed'] = subprocess.run(['rpm', '-q', 'putgd'], capture_output=True, text=True).stdout.strip()
        info['selinux'] = subprocess.run(['ls', '-Z', str(file)], capture_output=True, text=True).stdout.strip()
    return info


def status(run):
    s = run.ok('update.status')
    action = s.get('action') or {}
    return {'build': s['build']['version'], 'buildChannel': s['build']['channel'], 'channel': s['channel'],
            'channelSource': s.get('channelSource'), 'channelSetting': s.get('channelSetting'),
            'latest': (s.get('result') or {}).get('latest'), 'updateAvailable': (s.get('result') or {}).get('updateAvailable'),
            'notifies': s.get('notifies'), 'action': {k: action.get(k) for k in ('kind', 'manager', 'updater', 'channel', 'file', 'command', 'repository',
                                                                                  'otherChannel', 'older')},
            'installKind': s['install'].get('kind'), 'installAvailable': s['install'].get('available'), 'installReason': s['install'].get('reason'),
            'error': s.get('error')}


def show_updates(run):
    run.ok('ui.show', {'view': 'updates'}); time.sleep(1.5); run.ok('ui.show', {'view': 'updates'}); time.sleep(1)


def start(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'data', 'frames', 'logs', 'archive'):
        (run.dir / sub).mkdir(mode=0o700)
    session, shell, _ = afa.session_environment()
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/ua-pkg.XXXXXX'], text=True).strip()
    defaults = {k: str(Path.home() / v / 'putgd') for k, v in DEFAULT_DIRS.items()}
    run.meta = {'name': args.name, 'runtime': runtime, 'session': session, 'shell': shell, 'vnc': args.vnc_port, 'input': args.input,
                'profile': args.profile, 'feed': args.feed, 'hostPath': os.environ.get('PATH', '/usr/bin:/bin'),
                'host': os.uname().nodename, 'created': afa.iso(), 'pids': {}, 'log': [],
                'before': {'defaultProfile': {p: Path(p).exists() for p in defaults.values()}}}
    if args.profile == 'default' and any(run.meta['before']['defaultProfile'].values()):
        raise SystemExit(f"default profile folders exist: {run.meta['before']['defaultProfile']}; refusing")
    run.save()
    launch(run, args.label)
    template = run.ok('settings.developmentTemplate')['document']
    template['archive'] = {**template.get('archive', {}), 'root': str(run.dir / 'archive')}
    template['preferences'] = {**template.get('preferences', {}), 'launchAtLogin': False, 'notifications': args.notifications,
                               'unlockCredentialsAtStartup': False, 'updates': {'check': True, 'channel': 'build'}}
    (run.config_home / 'putgd').mkdir(parents=True, exist_ok=True)
    (run.config_home / 'putgd/config.json').write_text(json.dumps(template, indent=1))
    run.ok('settings.reload')
    check(args, run)


def launch(run, label):
    log = open(run.dir / f'logs/launch-{label}.log', 'ab')
    p = subprocess.Popen(['/usr/bin/putgd', '--background'], env=run.env(), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                         start_new_session=True)
    run.meta['pids']['launch'] = p.pid; run.save()
    sock = Path(run.env()['XDG_RUNTIME_DIR']) / 'putgd/session.sock'
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60)
    if not ready: raise SystemExit(f'resident not ready ({label})')


def check(args, run=None):
    run = run or Run(args.name)
    if not resident_pid(run): launch(run, args.label)
    reply = run.cli('update.check', timeout=90)
    show_updates(run)
    out(run.record(args.label, {'pid': resident_pid(run), 'reply': {'ok': reply.get('ok'), 'error': reply.get('error')},
                                'status': status(run), 'repo': repo(), 'frame': ua_app.frame(run, args.label)}))


def frame_only(args):
    run = Run(args.name)
    out(run.record(args.label, {'repo': repo(), 'frame': ua_app.frame(run, args.label)}))


def pointer(args):
    run = Run(args.name)
    ua_app.safe_handler(run.meta['session'])
    import rfb
    c = rfb.RFB(run.meta['vnc'], host=os.environ.get('UPD_VNC_HOST', '127.0.0.1')); size = c.frame()[:2]; c.close()
    inp = afa.Input(run, size)
    if args.cmd == 'click': inp.click(args.x, args.y, args.button)
    else: inp.key(*args.keys)
    time.sleep(args.settle)
    out(run.record(args.cmd, {'x': getattr(args, 'x', None), 'y': getattr(args, 'y', None), 'keys': getattr(args, 'keys', None),
                              'frame': ua_app.frame(run, args.label) if args.label else None}))


def updaters():
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            argv = (proc / 'cmdline').read_bytes().split(b'\0')
            name = Path(argv[0].decode(errors='replace')).name if argv and argv[0] else ''
            if (proc / 'stat').stat().st_uid != os.getuid(): continue
            if any(name == u or name.startswith(u) for u in UPDATERS) or any(u.encode() in a for a in argv[:2] for u in UPDATERS):
                found.append({'pid': int(proc.name), 'argv': [a.decode(errors='replace') for a in argv if a][:6]})
        except OSError: continue
    return found


def open_updates(args):
    """What Open software updates runs, through its method (the button calls the same one); then which updater runs."""
    run = Run(args.name)
    before = {u['pid'] for u in updaters()}
    reply = run.cli('update.openSoftwareUpdates', timeout=60)
    started = run.wait(lambda: [u for u in updaters() if u['pid'] not in before], 30)
    time.sleep(args.settle)
    out(run.record('openUpdates', {'reply': {'ok': reply.get('ok'), 'result': reply.get('result'), 'error': reply.get('error')},
                                   'started': started, 'frame': ua_app.frame(run, args.label)}))


def history(run):
    records = run.ok('history.list', {'limit': 50, 'offset': 0})
    items = []
    for r in records['records']:
        p = run.cli('history.preview', {'id': r['id']}, timeout=60).get('result') or {}
        items.append({'id': r['id'], 'name': r['name'], 'mime': r['mime'], 'uploadStatus': r.get('uploadStatus'),
                      'link': (r.get('receipt') or {}).get('directUrl'), 'local': (r.get('local') or {}).get('status'),
                      'preview': {'state': p.get('state'), 'exists': bool(p.get('previewPath')) and Path(p['previewPath']).exists()}})
    return {'total': records['total'], 'records': items}


def snapshot(args):
    run = Run(args.name)
    cfg = run.config_home / 'putgd/config.json'
    snap = run.ok('ui.snapshot')
    doc = {'build': snap.get('build'), 'pid': resident_pid(run), 'exe': os.readlink(f'/proc/{resident_pid(run)}/exe') if resident_pid(run) else None,
           'config': {'path': str(cfg), 'sha256': hashlib.sha256(cfg.read_bytes()).hexdigest() if cfg.exists() else None},
           'settingsRevision': run.ok('settings.get')['metadata'].get('acceptedRevision'),
           'preferences': run.ok('settings.get')['document'].get('preferences'),
           'history': history(run), 'repo': repo(), 'frame': ua_app.frame(run, args.label) if args.label else None}
    if cfg.exists(): shutil.copy2(cfg, run.dir / f'logs/config-{args.label}.json')
    out(run.record('snapshot:' + (args.label or ''), doc))


def socket_request(run, method, params):
    import socket
    path = str(Path(run.env()['XDG_RUNTIME_DIR']) / 'putgd/session.sock')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(20); s.connect(path)
        s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'ua-' + method, 'session': path, 'method': method, 'params': params}).encode() + b'\n')
        data = b''
        while not data.endswith(b'\n'):
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
    return json.loads(data)


def live_s3(args):
    """rerun.py's live-s3: a live S3 profile (bucket put.gd, public-read, links through https://put.gd) for every action,
    with key set 2 from stdin stored in the session. Nothing here prints or stores the keys."""
    run = Run(args.name)
    if sys.stdin.isatty(): raise SystemExit('keys come on stdin')
    if not args.prefix.startswith('acceptance/'): raise SystemExit('only acceptance/ prefixes')
    keys = json.loads(sys.stdin.read())
    doc = run.ok('settings.get')['document']
    profile = {'credentials': {'mode': 'session', 'reference': None}, 'id': 'live-s3', 'label': 'Live S3 acceptance', 'provider': 's3',
               'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'put.gd', 'connectTimeoutMs': 10000,
                            'endpoint': 'https://s3.us-east-2.amazonaws.com', 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120,
                            'objectPrefix': args.prefix, 'publicUrlBase': 'https://put.gd', 'region': 'us-east-2', 'requestTimeoutMs': 120000,
                            'storageClass': 'STANDARD'}}
    doc['profiles'] = [p for p in doc.get('profiles', []) if p['id'] != 'live-s3'] + [profile]
    for name, action in doc.get('actions', {}).items():
        if 'destination' in action: action.update({'destination': 'upload', 'profileId': 'live-s3'})
    doc.setdefault('actions', {}).setdefault('edit', {}).update({'destination': 'upload', 'profileId': 'live-s3'})
    (run.config_home / 'putgd/config.json').write_text(json.dumps(doc, indent=1)); run.ok('settings.reload')
    snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result']['metadata']['validation']['ok'] else None)(run.cli('settings.get')), 20)
    # The keys go over the session socket (afa.Run.socket_request), never on a command line.
    request = socket_request(run, 'setup.storeCredentials', {'profileId': 'live-s3', 'expectedRevision': snap['result']['metadata']['acceptedRevision'],
                                                              'credentials': {'accessKey': keys['accessKey'], 'secretKey': keys['secretKey']}})
    del keys
    rep = {'profile': profile['settings'], 'stored': request.get('ok'), 'storeError': (request.get('error') or {}).get('code')}
    if request.get('ok'):
        job = run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': request['result']['setupId']})), 40)
        rep['credentials'] = (job or {}).get('result', {}).get('state')
    out(run.record('liveS3', rep))


def upload(args):
    run = Run(args.name)
    png = run.dir / f'{args.label}.png'; ua_app.synthetic_png(png)
    eid = run.ok('editor.open', {'source': 'file', 'path': str(png)})['operationId']
    run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
    sub = run.ok('editor.submit', {'id': eid})
    op_id = sub.get('operationId') or eid
    op = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'failed', 'cancelled') else None)(
        run.cli('operation.inspect', {'id': op_id})), 180)
    link = next((r['link'] for r in history(run)['records'] if r['link']), None)
    try:
        with urllib.request.urlopen(link, timeout=30) as r: served = {'status': r.status, 'bytes': len(r.read()), 'type': r.headers.get('Content-Type')}
    except Exception as e: served = {'error': repr(e)[:200]}
    out(run.record('upload', {'operation': {k: (op or {}).get(k) for k in ('state', 'error')}, 'link': link, 'served': served}))


def delete(args):
    run = Run(args.name)
    records = run.ok('history.list', {'limit': 50, 'offset': 0})['records']
    target = next(r for r in records if r.get('uploadStatus') == 'uploaded')
    link = (target.get('receipt') or {}).get('directUrl')
    prep = run.ok('delete.prepare', {'ids': [target['id']]})
    conf = run.ok('delete.confirm', {'ids': [target['id']], 'acknowledgement': prep['acknowledgement']})
    fin = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'cancelled', 'failed') else None)(
        run.cli('delete.inspect', {'id': conf.get('id')})), 90)
    try: urllib.request.urlopen(link, timeout=15); remote = 'still served'
    except urllib.error.HTTPError as e: remote = f'HTTP {e.code}'
    out(run.record('delete', {'item': target['name'], 'link': link, 'final': fin, 'remoteAfter': remote}))


def watch(args):
    run = Run(args.name)
    log = run.dir / f'logs/notifications-{args.label}.log'
    if args.action == 'start':
        p = subprocess.Popen(['dbus-monitor', "interface='org.freedesktop.Notifications'"], env=run.meta['session'] | {'PATH': run.meta['hostPath']},
                             stdout=open(log, 'w'), stderr=subprocess.DEVNULL, start_new_session=True)
        run.meta['pids'][f'watch-{args.label}'] = p.pid; run.meta[f'watchStart-{args.label}'] = afa.iso(); run.save()
        out(run.record('watch', {'label': args.label, 'pid': p.pid})); return
    pid = run.meta['pids'].pop(f'watch-{args.label}')
    os.kill(pid, signal.SIGTERM); run.save()
    # ua_app.notifications reads put.gd's journal under DIR/state: the run's own, or ~/.local/state for the default profile.
    report = ua_app.notifications(type('R', (), {'dir': run.state_home.parent if run.meta['profile'] == 'default' else run.dir,
                                                 'meta': run.meta})(), log, args.label)
    out(run.record('watch', report))


def restart(args):
    run = Run(args.name)
    pid = resident_pid(run)
    exe_before = os.readlink(f'/proc/{pid}/exe') if pid else None
    if pid:
        run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
    launch(run, args.label)
    check(argparse.Namespace(name=args.name, label=args.label), run)
    run.meta.setdefault('restarts', []).append({'old': pid, 'oldExe': exe_before}); run.save()


def mem(args):
    run = Run(args.name)
    pid = resident_pid(run)
    if pid:
        run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
    launch(run, 'mem')
    pid = resident_pid(run)
    version = run.ok('ui.snapshot')['build']['version']
    time.sleep(args.settle)
    report = {'version': version, 'pid': pid, 'host': os.uname().nodename, 'session': run.meta['session'].get('XDG_SESSION_TYPE'),
              'desktop': run.meta['session'].get('XDG_CURRENT_DESKTOP'), 'A': ua_app.window(run, pid, 'A cold idle', args.seconds)}
    run.cli('update.check', timeout=90); time.sleep(args.settle)
    report['B'] = ua_app.window(run, pid, 'B idle after Check now', args.seconds)
    out(run.record('mem', report))


def quit_(args):
    run = Run(args.name)
    pid = resident_pid(run)
    r = run.cli('ui.quit', timeout=15)
    gone = run.wait(lambda: not pid or not Path(f'/proc/{pid}').exists(), 40)
    out({'quit': r.get('ok'), 'pid': pid, 'gone': bool(gone)})


def cleanup(args):
    run = Run(args.name)
    if resident_pid(run): quit_(args)
    for key, pid in list(run.meta.get('pids', {}).items()):
        if key.startswith('watch-') and Path(f'/proc/{pid}').exists(): os.kill(pid, signal.SIGTERM)
    removed = []
    if run.meta['profile'] == 'default':
        for path, existed in run.meta['before']['defaultProfile'].items():
            if not existed and Path(path).exists(): shutil.rmtree(path); removed.append(path)
    shutil.rmtree(run.meta['runtime'], ignore_errors=True)
    keep = ROOT / 'results' / run.name
    keep.mkdir(parents=True, exist_ok=True)
    shutil.copy2(run.meta_path, keep / 'run.json')
    shutil.copytree(run.dir / 'frames', keep / 'frames', dirs_exist_ok=True)
    shutil.copytree(run.dir / 'logs', keep / 'logs', dirs_exist_ok=True)
    shutil.rmtree(run.dir)
    out({'cleaned': run.name, 'kept': str(keep), 'removedDefaultProfile': removed, 'residentGone': resident_pid(run) is None})


parser = argparse.ArgumentParser()
sub = parser.add_subparsers(dest='cmd', required=True)
p = sub.add_parser('start'); p.add_argument('name'); p.add_argument('--vnc-port', type=int, required=True); p.add_argument('--label', required=True)
p.add_argument('--profile', choices=('private', 'default'), default='private'); p.add_argument('--feed', choices=('live', 'lab'), default='live')
p.add_argument('--input', choices=('xdotool', 'uinput', 'vnc'), default='vnc'); p.add_argument('--notifications', action='store_true')
for name in ('check', 'frame', 'restart'):
    p = sub.add_parser(name); p.add_argument('name'); p.add_argument('--label', required=True)
p = sub.add_parser('open-updates'); p.add_argument('name'); p.add_argument('--label', required=True); p.add_argument('--settle', type=float, default=8)
p = sub.add_parser('snapshot'); p.add_argument('name'); p.add_argument('--label')
p = sub.add_parser('click'); p.add_argument('name'); p.add_argument('x', type=int); p.add_argument('y', type=int)
p.add_argument('--button', default='left'); p.add_argument('--label'); p.add_argument('--settle', type=float, default=2)
p = sub.add_parser('key'); p.add_argument('name'); p.add_argument('keys', nargs='+'); p.add_argument('--label'); p.add_argument('--settle', type=float, default=1)
p = sub.add_parser('live-s3'); p.add_argument('name'); p.add_argument('--prefix', required=True)
p = sub.add_parser('upload'); p.add_argument('name'); p.add_argument('--label', default='upload')
p = sub.add_parser('watch'); p.add_argument('name'); p.add_argument('action', choices=('start', 'stop')); p.add_argument('--label', required=True)
p = sub.add_parser('mem'); p.add_argument('name'); p.add_argument('--seconds', type=int, default=180); p.add_argument('--settle', type=int, default=30)
sub.add_parser('repo')
for name in ('quit', 'cleanup', 'delete'):
    p = sub.add_parser(name); p.add_argument('name')
args = parser.parse_args()
if args.cmd == 'repo': out(repo())
else: {'start': start, 'check': check, 'frame': frame_only, 'click': pointer, 'key': pointer, 'open-updates': open_updates, 'snapshot': snapshot,
       'live-s3': live_s3, 'upload': upload, 'delete': delete, 'watch': watch, 'restart': restart, 'mem': mem, 'quit': quit_,
       'cleanup': cleanup}[args.cmd](args)
