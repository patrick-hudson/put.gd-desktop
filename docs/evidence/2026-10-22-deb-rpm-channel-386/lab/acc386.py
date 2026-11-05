#!/usr/bin/env python3
"""#386 lab acceptance driver: a DEB/RPM install that follows its pkg.put.gd repository's channel.

Runs from SSH on the host, stdlib only plus afa.py (session environment, PNG) and rfb.py beside it.
put.gd is the installed package (/usr/bin/putgd), started with private XDG_CONFIG/STATE/CACHE_HOME and
XDG_RUNTIME_DIR under $ACC_ROOT/runs/NAME (default ~/work/put.gd-i386acc), and reads the lab feeds:
PUTGD_UPDATE_FEED_URL=https://pkg.put.gd/lab/update/v1/. The repository file it follows is the system's
real one; this driver never writes it: the moves are run by hand (`shell`), as the user would.
Full-desktop frames come from the host's loopback wall VNC server and stay on the host.

  acc386.py start NAME --vnc-port P --label L      start the resident, Check now, Settings → Updates, frame
  acc386.py check NAME --label L                   Check now, Settings → Updates, frame
  acc386.py frame NAME --label L                   a frame only (another app in front)
  acc386.py repo NAME                              the repository file, the package, apt/dnf's view
  acc386.py quit NAME / cleanup NAME
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import afa  # noqa: E402
ROOT = Path(os.environ.get('ACC_ROOT', str(Path.home() / 'work/put.gd-i386acc')))
LAB = 'https://pkg.put.gd/lab'
APT = Path('/etc/apt/sources.list.d/putgd.sources')
DNF = Path('/etc/yum.repos.d/putgd.repo')


def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)


class Run:
    def __init__(self, name):
        self.name, self.dir = name, ROOT / 'runs' / name
        self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}

    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')

    def env(self):
        env = dict(self.meta['session'])
        for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_DATA_HOME', 'data')):
            env[key] = str(self.dir / sub)
        env['XDG_RUNTIME_DIR'] = self.meta['runtime']
        env['PUTGD_UPDATE_FEED_URL'] = LAB + '/update/v1/'
        return env

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
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            if (proc / 'comm').read_text().strip() != 'putgd': continue
            if f"XDG_RUNTIME_DIR={run.meta['runtime']}".encode() in (proc / 'environ').read_bytes().split(b'\0'): return int(proc.name)
        except OSError: continue
    return None


def frame(run, label):
    import rfb
    c = rfb.RFB(run.meta['vnc']); f = c.frame(); c.close()
    path = run.dir / 'frames' / f'{label}.png'
    afa.write_png(f, [0, 0, f[0], f[1]], path)
    return str(path)


def repo():
    file = APT if APT.exists() or Path('/etc/apt').exists() else DNF
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
            'notifies': s.get('notifies'), 'action': {k: action.get(k) for k in ('kind', 'channel', 'file', 'command', 'otherChannel', 'older')},
            'installKind': s['install'].get('kind'), 'installReason': s['install'].get('reason')}


def show_updates(run):
    run.ok('ui.show', {'view': 'updates'}); time.sleep(1.5); run.ok('ui.show', {'view': 'updates'}); time.sleep(1)


def start(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'data', 'frames', 'logs', 'archive'):
        (run.dir / sub).mkdir(mode=0o700)
    session, shell, _ = afa.session_environment()
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/acc386.XXXXXX'], text=True).strip()
    run.meta = {'name': args.name, 'runtime': runtime, 'session': session, 'shell': shell, 'vnc': args.vnc_port,
                'host': os.uname().nodename, 'created': afa.iso(), 'log': []}
    run.save()
    launch(run, args.label)
    template = run.ok('settings.developmentTemplate')['document']
    template['archive'] = {**template.get('archive', {}), 'root': str(run.dir / 'archive')}
    template['preferences'] = {**template.get('preferences', {}), 'launchAtLogin': False, 'notifications': False,
                               'unlockCredentialsAtStartup': False, 'updates': {'check': True, 'channel': 'build'}}
    (run.dir / 'config/putgd').mkdir(exist_ok=True)
    (run.dir / 'config/putgd/config.json').write_text(json.dumps(template, indent=1))
    run.ok('settings.reload')
    check(args, run)


def launch(run, label):
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    log = open(run.dir / f'logs/launch-{label}.log', 'ab')
    subprocess.Popen(['/usr/bin/putgd', '--background'], env=run.env(), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                     start_new_session=True)
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60)
    if not ready: raise SystemExit(f'resident not ready ({label})')


def check(args, run=None):
    run = run or Run(args.name)
    if not resident_pid(run): launch(run, args.label)
    reply = run.cli('update.check', timeout=90)
    show_updates(run)
    out(run.record(args.label, {'pid': resident_pid(run), 'reply': {'ok': reply.get('ok'), 'error': reply.get('error')},
                                'status': status(run), 'repo': repo(), 'frame': frame(run, args.label)}))


def frame_only(args):
    run = Run(args.name)
    out(run.record(args.label, {'repo': repo(), 'frame': frame(run, args.label)}))


def quit_(args):
    run = Run(args.name)
    pid = resident_pid(run)
    r = run.cli('ui.quit', timeout=15)
    gone = run.wait(lambda: not pid or not Path(f'/proc/{pid}').exists(), 40)
    out({'quit': r.get('ok'), 'pid': pid, 'gone': bool(gone)})


def cleanup(args):
    run = Run(args.name)
    if resident_pid(run): quit_(args)
    shutil.rmtree(run.meta['runtime'], ignore_errors=True)
    keep = ROOT / 'results' / run.name
    keep.mkdir(parents=True, exist_ok=True)
    shutil.copy2(run.meta_path, keep / 'run.json')
    shutil.copytree(run.dir / 'frames', keep / 'frames', dirs_exist_ok=True)
    shutil.rmtree(run.dir)
    out({'cleaned': run.name, 'kept': str(keep), 'residentGone': resident_pid(run) is None})


parser = argparse.ArgumentParser()
sub = parser.add_subparsers(dest='command', required=True)
p = sub.add_parser('start'); p.add_argument('name'); p.add_argument('--vnc-port', type=int, required=True); p.add_argument('--label', required=True)
for name in ('check', 'frame'):
    p = sub.add_parser(name); p.add_argument('name'); p.add_argument('--label', required=True)
sub.add_parser('repo')
for name in ('quit', 'cleanup'):
    p = sub.add_parser(name); p.add_argument('name')
args = parser.parse_args()
if args.command == 'repo': out(repo())
else: {'start': start, 'check': check, 'frame': frame_only, 'quit': quit_, 'cleanup': cleanup}[args.command](args)
