#!/usr/bin/env python3
"""#293 phase 3 lab acceptance driver: the in-app AppImage update, N -> N+1, on a desktop's host.

Runs from SSH on the host (or in a VM guest), stdlib only plus afa.py and rfb.py beside it.
Every run lives in $UPD_ROOT/runs/NAME (default ~/work/put.gd-i293acc): put.gd gets private
XDG_CONFIG/STATE/CACHE_HOME and XDG_RUNTIME_DIR there, its AppImage sits in the run's own private
folder (apps/), and PATH starts with the run's bin/:
  scenario a: bin/appimageupdatetool is the real tool (zsync path)
  scenario b: bin/ is empty and the host has no appimageupdatetool or zsync2 (fallback)
  scenario c: bin/appimageupdatetool is a shim that crashes (SIGSEGV), as on stock Fedora 44 (fallback)
The resident reads the lab release: PUTGD_UPDATE_FEED_URL=https://pkg.put.gd/lab/update/v1/,
PUTGD_UPDATE_RELEASE_URL and PUTGD_UPDATE_MIRROR_URL=https://pkg.put.gd/lab/releases.
`tamper` points those at a loopback server serving the lab release with one thing broken.
Full-desktop frames come from the host's loopback wall VNC server and stay on the host.
Each subcommand prints one JSON document.
"""
import argparse, hashlib, http.server, json, os, shutil, signal, socketserver, subprocess, sys, threading, time, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import afa  # noqa: E402  session_environment, write_png, sha256, iso
ROOT = Path(os.environ.get('UPD_ROOT', str(Path.home() / 'work/put.gd-afa2/upd')))
LAB = 'https://pkg.put.gd/lab'
DATA = Path.home() / '.local/share'
# The default profile a login-started resident uses; removed at cleanup only when the run created it.
REAL_PROFILE = [Path.home() / '.config/putgd', Path.home() / '.local/state/putgd', Path.home() / '.cache/putgd']


def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)


class Run:
    def __init__(self, name):
        self.name, self.dir = name, ROOT / 'runs' / name
        self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}

    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')

    def env(self, extra=None):
        env = dict(self.meta['session'])
        env['PATH'] = f"{self.dir / 'bin'}:{self.meta['hostPath']}"
        env['PIPEWIRE_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')):
            env[key] = str(self.dir / sub)
        env['XDG_RUNTIME_DIR'] = self.meta['runtime']
        env.update(self.meta.get('urls') or {})
        env.update(extra or {})
        return env

    @property
    def image(self): return Path(self.meta['image'])

    def cli(self, method, params=None, timeout=70):
        """putgd-cli from the run's extracted copy of N (a client only; it talks to the resident's socket)."""
        cli = Path(self.meta['cliDir']) / 'usr/bin/putgd-cli'
        env = self.env()
        lib = Path(self.meta['cliDir']) / 'usr/lib'
        env['LD_LIBRARY_PATH'] = f'{lib}:{lib}/putgd'
        env['QT_PLUGIN_PATH'] = f"{self.meta['cliDir']}/usr/plugins"
        r = subprocess.run([str(cli), method, json.dumps(params or {})], env=env, timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}

    def ok(self, method, params=None, timeout=70):
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
        log = self.meta.setdefault('log', [])
        log.append({'at': afa.iso(), key: value}); self.save()
        return {key: value}


def setup(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    if (DATA / 'putgd').exists() and not args.allow_data_home: raise SystemExit(f'{DATA / "putgd"} exists; refusing')
    for tool in ('appimageupdatetool', 'zsync2'):
        if shutil.which(tool): raise SystemExit(f'{tool} is on the host PATH ({shutil.which(tool)}); the scenarios need it absent')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'apps', 'bin', 'frames', 'logs', 'archive', 'cli'):
        (run.dir / sub).mkdir(mode=0o700)
    session, shell, _ = afa.session_environment()
    image = run.dir / 'apps' / Path(args.appimage).name
    shutil.copy2(args.appimage, image); image.chmod(0o755)
    subprocess.run([str(image), '--appimage-extract'], cwd=run.dir / 'cli', check=True, stdout=subprocess.DEVNULL)
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/upd293.XXXXXX'], text=True).strip()
    if args.scenario == 'a':
        tool = Path(args.tool).resolve()
        shutil.copy2(tool, run.dir / 'bin/appimageupdatetool'); (run.dir / 'bin/appimageupdatetool').chmod(0o755)
    elif args.scenario == 'c':
        shim = run.dir / 'bin/appimageupdatetool'
        shim.write_text('#!/bin/sh\n# #293 lab scenario c: a zsync2 that crashes, as appimageupdatetool does on stock Fedora 44 (#297).\n'
                        'echo "zsync2: Unknown error while checking whether file (null) exists: Bad address (lab shim)" >&2\nkill -SEGV $$\n')
        shim.chmod(0o755)
    autostart = Path.home() / '.config/autostart'
    run.meta = {'name': args.name, 'scenario': args.scenario, 'image': str(image), 'imageSha256': afa.sha256(image),
                'cliDir': str(run.dir / 'cli/squashfs-root'), 'runtime': runtime, 'session': session, 'shell': shell,
                'hostPath': os.environ.get('PATH', '/usr/bin:/bin'), 'host': os.uname().nodename, 'vnc': args.vnc_port,
                # --live: the real app.put.gd feed and pkg.put.gd mirror (no overrides), as a user gets them.
                'urls': {} if args.live else {'PUTGD_UPDATE_FEED_URL': LAB + '/update/v1/', 'PUTGD_UPDATE_RELEASE_URL': LAB + '/releases',
                                              'PUTGD_UPDATE_MIRROR_URL': LAB + '/releases'},
                'before': {'dataPutgd': (DATA / 'putgd').exists(),
                           'homePutgd': {str(d): d.exists() for d in REAL_PROFILE},
                           'autostart': sorted(p.name for p in autostart.glob('*')) if autostart.exists() else []},
                'pids': {}, 'created': afa.iso()}
    run.save()
    doc = {'schemaVersion': 1, 'preferences': {'launchAtLogin': False, 'notifications': False, 'appearance': {'theme': 'system'},
                                                'unlockCredentialsAtStartup': False,
                                                'updates': {'check': True, 'channel': 'build'}}}
    launch(run, 'setup')
    template = run.ok('settings.developmentTemplate')['document']
    template.update({'archive': {**template.get('archive', {}), 'root': str(run.dir / 'archive')}})
    template['preferences'] = {**template.get('preferences', {}), **doc['preferences']}
    (run.dir / 'config/putgd').mkdir(exist_ok=True)
    (run.dir / 'config/putgd/config.json').write_text(json.dumps(template, indent=1))
    run.ok('settings.reload')
    out({'setup': True, 'run': run.meta, 'status': run.ok('update.status')})


def launch(run, label, extra=None):
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    log = open(run.dir / f'logs/launch-{label}.log', 'ab')
    p = subprocess.Popen([str(run.image), '--background'], env=run.env(extra), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                         start_new_session=True)
    run.meta['pids']['apprun'] = p.pid; run.save()
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60)
    if not ready: raise SystemExit(f'resident not ready ({label}); see logs/launch-{label}.log')
    return p


def resident_pid(run):
    """The resident's own PID: the process whose environment names this run's runtime directory."""
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            if (proc / 'comm').read_text().strip() != 'putgd': continue
            if f"XDG_RUNTIME_DIR={run.meta['runtime']}".encode() in (proc / 'environ').read_bytes().split(b'\0'): return int(proc.name)
        except OSError: continue
    return None


def frame(run, label):
    if not run.meta.get('vnc'): return None
    import rfb
    c = rfb.RFB(run.meta['vnc'], host=os.environ.get('UPD_VNC_HOST', '127.0.0.1')); f = c.frame(); c.close()
    path = run.dir / 'frames' / f'{label}.png'
    afa.write_png(f, [0, 0, f[0], f[1]], path)
    return str(path)


def show_updates(run):
    """Settings scrolled to Updates: one ui.show, which #348 (in 925 and later) keeps in view while the window settles."""
    run.ok('ui.show', {'view': 'updates'}); time.sleep(2.5)


def status(run):
    s = run.ok('update.status')
    return {'build': s['build']['version'], 'channel': s['channel'], 'latest': (s.get('result') or {}).get('latest'),
            'updateAvailable': (s.get('result') or {}).get('updateAvailable'), 'install': s['install']}


def start(args):
    run = Run(args.name)
    launch(run, args.label)
    show_updates(run)
    out(run.record('start', {'pid': resident_pid(run), 'status': status(run), 'frame': frame(run, args.label)}))


def check(args):
    run = Run(args.name)
    run.ok('update.check', timeout=90)
    show_updates(run)
    out(run.record('check', {'status': status(run), 'frame': frame(run, 'check')}))


def install(args):
    run = Run(args.name)
    params = {'confirm': True} | ({'method': args.method} if args.method != 'auto' else {})
    started = time.time()
    reply = run.cli('update.install', params)
    if not reply.get('ok'): out(run.record('install', {'reply': reply})); return
    run.ok('ui.show', {'view': 'updates'})
    phases, shots = [], {}
    while time.time() - started < 3600:
        s = run.ok('update.status')['install']
        step = (s.get('phase'), s.get('method'))
        if not phases or phases[-1]['phase'] != step:
            phases.append({'at': round(time.time() - started, 1), 'phase': step, 'bytes': s.get('bytes'), 'total': s.get('total')})
            if step[0] in ('zsync', 'download') and step[0] not in shots:
                time.sleep(1.5); shots[step[0]] = frame(run, f'install-{step[0]}')
        if not s.get('running') and s.get('state') in ('installed', 'failed'): break
        time.sleep(.4)
    show_updates(run)
    final = run.ok('update.status')['install']
    shots['result'] = frame(run, 'install-result')
    backup = (final.get('result') or {}).get('backup')
    report = {'seconds': round(time.time() - started, 1), 'phases': phases, 'install': final, 'frames': shots,
              'imageSha256': afa.sha256(run.image), 'backup': backup,
              'backupSha256': afa.sha256(backup) if backup and Path(backup).exists() else None,
              'folder': sorted(p.name for p in run.image.parent.iterdir()),
              'stateFile': json.loads((run.dir / 'state/putgd/update-install.json').read_text())
              if (run.dir / 'state/putgd/update-install.json').exists() else None}
    out(run.record('install', report))


def restart(args):
    run = Run(args.name)
    old = resident_pid(run)
    reply = run.cli('update.restart', {'confirm': True})
    gone = run.wait(lambda: not Path(f'/proc/{old}').exists(), 60) if old else True
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 90)
    new = resident_pid(run)
    snap = run.ok('ui.snapshot') if ready else {}
    if ready:
        show_updates(run)
    exe = os.readlink(f'/proc/{new}/exe') if new else None
    environ = dict(e.split('=', 1) for e in Path(f'/proc/{new}/environ').read_bytes().decode(errors='replace').split('\0') if '=' in e) if new else {}
    st = run.ok('update.status') if ready else {}
    out(run.record('restart', {'reply': reply, 'oldPid': old, 'oldGone': bool(gone), 'newPid': new, 'newExe': exe,
                               'newAppimage': environ.get('APPIMAGE'), 'version': (snap.get('build') or {}).get('version'),
                               'install': st.get('install'), 'imageSha256': afa.sha256(run.image),
                               'afterRestart': {'current': (st.get('result') or {}).get('current'), 'latest': (st.get('result') or {}).get('latest'),
                                                'updateAvailable': (st.get('result') or {}).get('updateAvailable')},
                               'folder': sorted(p.name for p in run.image.parent.iterdir()), 'frame': frame(run, 'restarted')}))


def quit_(args):
    run = Run(args.name)
    pid = resident_pid(run)
    r = run.cli('ui.quit', timeout=15)
    gone = run.wait(lambda: not pid or not Path(f'/proc/{pid}').exists(), 40)
    out({'quit': r.get('ok'), 'pid': pid, 'gone': bool(gone)})


# ---------------------------------------------------------------- idle memory

def window(run, pid, label, seconds):
    """RSS once a second (statm), recorded every 10 s; Pss/Private from smaps_rollup and the TLS/network libraries mapped."""
    import re, statistics
    page = os.sysconf('SC_PAGE_SIZE'); samples = []; start = time.time()
    while time.time() - start < seconds:
        rss = int(Path(f'/proc/{pid}/statm').read_text().split()[1]) * page
        samples.append(rss); time.sleep(1)
    roll = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        k, v = line.split(':', 1); roll[k] = int(v.split()[0]) * 1024
    libs = sorted({Path(l.split()[-1]).name for l in Path(f'/proc/{pid}/maps').read_text().splitlines()
                   if len(l.split()) >= 6 and re.search(r'libQt6Network|libssl|libcrypto|libqopensslbackend|libgnutls', l.split()[-1])})
    mib = lambda b: round(b / 1048576, 2)
    return {'window': label, 'seconds': round(time.time() - start, 1), 'rssMedianMiB': mib(statistics.median(samples)),
            'rssMinMiB': mib(min(samples)), 'rssMaxMiB': mib(max(samples)), 'every10s': [mib(x) for x in samples[::10]],
            'pssMiB': mib(roll.get('Pss', 0)), 'privateMiB': mib(roll.get('Private_Clean', 0) + roll.get('Private_Dirty', 0)),
            'tlsNetworkLibraries': libs, 'load': os.getloadavg()}


def mem(args):
    run = Run(args.name)
    pid = resident_pid(run)
    if pid:
        run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
    launch(run, 'mem')  # --background: no window, as at login
    pid = resident_pid(run)
    version = run.ok('ui.snapshot')['build']['version']
    time.sleep(args.settle)
    report = {'version': version, 'imageSha256': afa.sha256(run.image), 'pid': pid, 'host': os.uname().nodename,
              'session': run.meta['session'].get('XDG_SESSION_TYPE'), 'desktop': run.meta['session'].get('XDG_CURRENT_DESKTOP')}
    report['A'] = window(run, pid, 'A cold idle', args.seconds)
    if args.action == 'install':
        run.cli('update.check', timeout=90)
        run.cli('update.install', {'confirm': True})
        run.wait(lambda: not run.ok('update.status')['install'].get('running'), 900)
        report['action'] = {'install': run.ok('update.status')['install'].get('state')}
    else:
        report['action'] = {'check': bool(run.cli('update.check', timeout=90).get('ok'))}
    time.sleep(args.settle)
    report['B'] = window(run, pid, 'B idle after ' + args.action, args.seconds)
    out(run.record('mem', report))


# ---------------------------------------------------------------- tampering (loopback)

class Tamper(http.server.BaseHTTPRequestHandler):
    """Serves the lab release from a local copy, with the run's mode applied (mode file in the run)."""
    def log_message(self, *a): pass

    def do_HEAD(self): self.serve(head=True)
    def do_GET(self): self.serve()

    def serve(self, head=False):
        root, mode = self.server.root, (self.server.run_dir / 'tamper-mode').read_text().strip()
        path = root / self.path.lstrip('/').split('?')[0]
        name = path.name
        data = path.read_bytes() if path.is_file() else None
        if data is not None and mode == 'image' and name.endswith('.AppImage'):
            data = data[:-1] + bytes([data[-1] ^ 1])
        if mode == 'unsigned' and name == 'SHA256SUMS.minisig': data = None
        if mode == 'sums' and name == 'SHA256SUMS' and data is not None:
            # SHA256SUMS rewritten to match a tampered image; its signature is the original one.
            image = next(path.parent.glob('*.AppImage')).read_bytes()
            tampered = hashlib.sha256(image[:-1] + bytes([image[-1] ^ 1])).hexdigest()
            data = b''.join((tampered.encode() + line[64:] if line.endswith(b'.AppImage') else line) + b'\n' for line in data.splitlines())
        if mode == 'sums' and name.endswith('.AppImage') and data is not None:
            data = data[:-1] + bytes([data[-1] ^ 1])
        if data is None:
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers()
        if not head: self.wfile.write(data)


def tamper(args):
    """Serve the lab release locally (download from the lab mirror once), restart the resident against it, and try each mode."""
    run = Run(args.name)
    root = run.dir / 'tamper-root'
    tag = args.tag
    release = root / 'releases' / tag
    if not release.exists():
        release.mkdir(parents=True)
        for f in ('SHA256SUMS', 'SHA256SUMS.minisig', f'putgd-{tag[1:]}-x86_64.AppImage'):
            urllib.request.urlretrieve(f'{LAB}/releases/{tag}/{f}', release / f)
    (run.dir / 'tamper-mode').write_text('none\n')
    server = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Tamper)
    server.daemon_threads = True; server.root = root; server.run_dir = run.dir
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_address[1]}/releases'
    pid = resident_pid(run)
    if pid:
        run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
    launch(run, 'tamper', {'PUTGD_UPDATE_RELEASE_URL': base, 'PUTGD_UPDATE_MIRROR_URL': base})
    run.ok('ui.show', {'view': 'updates'})
    results = {}
    for mode in ('image', 'unsigned', 'sums'):
        (run.dir / 'tamper-mode').write_text(mode + '\n')
        reply = run.cli('update.install', {'confirm': True, 'method': 'download'})
        run.wait(lambda: not run.ok('update.status')['install'].get('running'), 600)
        time.sleep(1.5)
        final = run.ok('update.status')['install']
        results[mode] = {'reply': reply.get('ok'), 'state': final.get('state'), 'error': final.get('error'),
                         'imageSha256': afa.sha256(run.image), 'folder': sorted(p.name for p in run.image.parent.iterdir()),
                         'frame': frame(run, f'tamper-{mode}')}
    server.shutdown()
    r = run.cli('ui.quit', timeout=15)
    out(run.record('tamper', {'base': base, 'results': results, 'unchanged': all(v['imageSha256'] == run.meta['imageSha256'] for v in results.values())}))


# ---------------------------------------------------------------- start at login

def login(args):
    """enable: Launch at login in the run's profile, published to the session's own autostart (the real config home,
    as afa.py does). check: after the real logout/login, which image and version started. disable: undo."""
    run = Run(args.name)
    real = Path.home() / '.config/autostart/putgd.desktop'
    if args.action == 'enable':
        meta = run.ok('settings.get')['metadata']
        run.ok('settings.patch', {'patch': {'preferences': {'launchAtLogin': True}}, 'expectedRevision': meta['observedRevision']})
        entry = run.dir / 'config/autostart/putgd.desktop'
        run.wait(lambda: entry.exists(), 20)
        if real.exists(): raise SystemExit(f'{real} exists; refusing')
        real.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(entry, real)
        run.meta['realAutostart'] = str(real); run.save()
        script = DATA / 'putgd/login/appimage-start.sh'
        out(run.record('loginEnable', {'entry': entry.read_text(), 'script': script.read_text() if script.exists() else None}))
    elif args.action == 'check':
        found = []
        for proc in Path('/proc').iterdir():
            if not proc.name.isdigit(): continue
            try:
                if (proc / 'comm').read_text().strip() != 'putgd': continue
                env = dict(e.split('=', 1) for e in (proc / 'environ').read_bytes().decode(errors='replace').split('\0') if '=' in e)
                parent = (Path('/proc') / (proc / 'stat').read_text().split()[3] / 'comm').read_text().strip()
                found.append({'pid': int(proc.name), 'appimage': env.get('APPIMAGE'), 'parent': parent,
                              'started': time.strftime('%H:%M:%S', time.localtime((proc / 'stat').stat().st_mtime))})
            except OSError: continue
        version = None
        for item in found:
            if item['appimage']:
                version = subprocess.run([item['appimage'], '--version'], capture_output=True, text=True, timeout=30).stdout.strip()
        out(run.record('loginCheck', {'residents': found, 'version': version, 'imageSha256': afa.sha256(run.image),
                                      'journal': afa.sh('journalctl', '--user', '-b', '--no-pager', '-o', 'cat', '-g', 'put.gd login start', timeout=30)[-600:]}))
    else:
        if real.exists() and real.read_text().count('X-putgd-Managed=true'): real.unlink()
        out(run.record('loginDisable', {'removed': not real.exists()}))


def cleanup(args):
    run = Run(args.name)
    pid = resident_pid(run)
    if pid:
        run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
    # A resident the run's login entry started (default profile, the run's image).
    for proc in Path('/proc').iterdir():
        try:
            if proc.name.isdigit() and (proc / 'comm').read_text().strip() == 'putgd' and \
               f"APPIMAGE={run.meta['image']}".encode() in (proc / 'environ').read_bytes().split(b'\0'):
                os.kill(int(proc.name), signal.SIGTERM); run.wait(lambda: not proc.exists(), 30)
        except OSError: continue
    real = run.meta.get('realAutostart')
    if real and Path(real).exists(): Path(real).unlink()
    if not run.meta['before']['dataPutgd'] and (DATA / 'putgd').exists(): shutil.rmtree(DATA / 'putgd')
    for path, existed in run.meta['before'].get('homePutgd', {}).items():
        if not existed and Path(path).exists(): shutil.rmtree(path)
    shutil.rmtree(run.meta['runtime'], ignore_errors=True)
    keep = ROOT / 'results' / run.name
    keep.mkdir(parents=True, exist_ok=True)
    shutil.copy2(run.meta_path, keep / 'run.json')
    if (run.dir / 'frames').exists(): shutil.copytree(run.dir / 'frames', keep / 'frames', dirs_exist_ok=True)
    shutil.rmtree(run.dir)
    out({'cleaned': run.name, 'kept': str(keep), 'residentGone': resident_pid(run) is None})


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('setup'); a.add_argument('name'); a.add_argument('--appimage', required=True); a.add_argument('--scenario', choices='abc', required=True)
    a.add_argument('--tool'); a.add_argument('--vnc-port', type=int, default=5900); a.add_argument('--allow-data-home', action='store_true')
    a.add_argument('--live', action='store_true')
    for name in ('check', 'restart', 'quit', 'cleanup'):
        sub.add_parser(name).add_argument('name')
    a = sub.add_parser('start'); a.add_argument('name'); a.add_argument('--label', default='start')
    a = sub.add_parser('install'); a.add_argument('name'); a.add_argument('--method', default='auto', choices=('auto', 'zsync', 'download'))
    a = sub.add_parser('mem'); a.add_argument('name'); a.add_argument('--action', choices=('install', 'check'), required=True)
    a.add_argument('--seconds', type=int, default=180); a.add_argument('--settle', type=int, default=30)
    a = sub.add_parser('tamper'); a.add_argument('name'); a.add_argument('--tag', required=True)
    a = sub.add_parser('login'); a.add_argument('name'); a.add_argument('action', choices=('enable', 'check', 'disable'))
    args = p.parse_args()
    {'setup': setup, 'start': start, 'check': check, 'install': install, 'restart': restart, 'quit': quit_, 'tamper': tamper, 'mem': mem,
     'login': login, 'cleanup': cleanup}[args.cmd](args)


if __name__ == '__main__':
    main()
