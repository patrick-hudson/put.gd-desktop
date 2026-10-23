#!/usr/bin/env python3
"""Linux update profile driver for AppImages (U7, U8, U10-U12, U14, U15), on a desktop's host or in a VM guest.

From #344's upd344.py (itself #343's upd293.py). Runs from SSH on the host, stdlib only plus afa.py and rfb.py
beside it. Every run lives in $UPD_ROOT/runs/NAME (default ~/work/put.gd-ua/app): put.gd gets private
XDG_CONFIG/STATE/CACHE_HOME and XDG_RUNTIME_DIR there, its AppImage sits in the run's own private folder
(apps/), and PATH starts with the run's bin/:
  --tool PATH   bin/appimageupdatetool is the real tool (zsync path)
  --crash       bin/appimageupdatetool is a shim that crashes (SIGSEGV), so the verified full download is forced
  neither       bin/ is empty and the host has no appimageupdatetool or zsync2 (full download)
--live reads the real app.put.gd feed and the pkg.put.gd mirror, as a user gets them; otherwise the lab:
PUTGD_UPDATE_FEED_URL=https://pkg.put.gd/lab/update/v1/, PUTGD_UPDATE_RELEASE_URL and PUTGD_UPDATE_MIRROR_URL
=https://pkg.put.gd/lab/releases. `tamper` points the release URLs at a loopback server serving a release with
one thing broken. Full-desktop frames come from the host's loopback wall VNC server and stay on the host.
Each subcommand prints one JSON document.
"""
import argparse, hashlib, http.server, json, os, shutil, signal, socketserver, subprocess, sys, threading, time, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import afa  # noqa: E402  #415's guarded copy: session_environment, write_png, sha256, iso, Input, RECORDER
ROOT = Path(os.environ.get('UPD_ROOT', str(Path.home() / 'work/put.gd-ua/app')))
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
        if shutil.which(tool): raise SystemExit(f'{tool} is on the host PATH ({shutil.which(tool)}); the runs need it absent')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'apps', 'bin', 'frames', 'logs', 'archive', 'cli'):
        (run.dir / sub).mkdir(mode=0o700)
    session, shell, _ = afa.session_environment()
    image = run.dir / 'apps' / Path(args.appimage).name
    shutil.copy2(args.appimage, image); image.chmod(0o755)
    subprocess.run([str(image), '--appimage-extract'], cwd=run.dir / 'cli', check=True, stdout=subprocess.DEVNULL)
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/ua-app.XXXXXX'], text=True).strip()
    if args.tool:
        tool = Path(args.tool).resolve()
        shutil.copy2(tool, run.dir / 'bin/appimageupdatetool'); (run.dir / 'bin/appimageupdatetool').chmod(0o755)
    elif args.crash:
        shim = run.dir / 'bin/appimageupdatetool'
        shim.write_text('#!/bin/sh\n# Linux update profile U7: a zsync tool that crashes, as appimageupdatetool does on stock Fedora 44 (#297).\n'
                        'echo "zsync2: Unknown error while checking whether file (null) exists: Bad address (lab shim)" >&2\nkill -SEGV $$\n')
        shim.chmod(0o755)
    autostart = Path.home() / '.config/autostart'
    run.meta = {'name': args.name, 'tool': 'real' if args.tool else 'crash' if args.crash else None, 'live': args.live, 'image': str(image), 'imageSha256': afa.sha256(image),
                'cliDir': str(run.dir / 'cli/squashfs-root'), 'runtime': runtime, 'session': session, 'shell': shell,
                'hostPath': os.environ.get('PATH', '/usr/bin:/bin'), 'host': os.uname().nodename, 'vnc': args.vnc_port,
                'urls': {} if args.live else {'PUTGD_UPDATE_FEED_URL': LAB + '/update/v1/', 'PUTGD_UPDATE_RELEASE_URL': LAB + '/releases',
                                              'PUTGD_UPDATE_MIRROR_URL': LAB + '/releases'},
                'input': args.input,
                'before': {'dataPutgd': (DATA / 'putgd').exists(),
                           'homePutgd': {str(d): d.exists() for d in REAL_PROFILE},
                           'autostart': sorted(p.name for p in autostart.glob('*')) if autostart.exists() else []},
                'pids': {}, 'created': afa.iso()}
    run.save()
    doc = {'schemaVersion': 1, 'preferences': {'launchAtLogin': False, 'notifications': args.notifications, 'appearance': {'theme': 'system'},
                                                'unlockCredentialsAtStartup': False,
                                                'updates': {'check': True, 'channel': args.channel}}}
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
    """Settings scrolled to Updates; again once a freshly opened window has its final layout."""
    run.ok('ui.show', {'view': 'updates'}); time.sleep(1.5); run.ok('ui.show', {'view': 'updates'}); time.sleep(1)


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
    reply = run.cli('update.check', timeout=90)
    show_updates(run)
    out(run.record('check', {'reply': {'ok': reply.get('ok'), 'error': reply.get('error')}, 'status': status(run), 'frame': frame(run, args.label)}))


def channel(args):
    """Settings' Channel choice through settings.patch (what the dropdown writes), then Check now."""
    run = Run(args.name)
    meta = run.ok('settings.get')['metadata']
    run.ok('settings.patch', {'patch': {'preferences': {'updates': {'channel': args.channel}}}, 'expectedRevision': meta['observedRevision']})
    reply = run.cli('update.check', timeout=90)
    show_updates(run)
    out(run.record('channel', {'channel': args.channel, 'reply': {'ok': reply.get('ok'), 'error': reply.get('error')},
                               'status': status(run), 'frame': frame(run, args.label)}))


def install(args):
    run = Run(args.name)
    params = {'confirm': True} | ({'method': args.method} if args.method != 'auto' else {}) | \
        ({'channel': args.channel} if args.channel else {}) | ({'allowOlder': True} if args.allow_older else {})
    started = time.time()
    reply = run.cli('update.install', params)
    if not reply.get('ok'):
        show_updates(run)
        out(run.record('install', {'params': params, 'reply': reply, 'status': status(run), 'frame': frame(run, args.label + '-refused')})); return
    run.ok('ui.show', {'view': 'updates'})
    phases, shots = [], {}
    while time.time() - started < 3600:
        s = run.ok('update.status')['install']
        step = (s.get('phase'), s.get('method'))
        if not phases or phases[-1]['phase'] != step:
            phases.append({'at': round(time.time() - started, 1), 'phase': step, 'bytes': s.get('bytes'), 'total': s.get('total')})
            if step[0] in ('zsync', 'download') and step[0] not in shots:
                time.sleep(1.5); shots[step[0]] = frame(run, f'{args.label}-{step[0]}')
        if not s.get('running') and s.get('state') in ('installed', 'failed'): break
        time.sleep(.4)
    show_updates(run)
    final = run.ok('update.status')['install']
    shots['result'] = frame(run, args.label + '-result')
    backup = (final.get('result') or {}).get('backup')
    report = {'params': params, 'seconds': round(time.time() - started, 1), 'phases': phases, 'install': final, 'frames': shots,
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
                               'channel': st.get('channel'), 'channelSetting': st.get('channelSetting'), 'build': st.get('build'),
                               'loginScript': login_script(),
                               'folder': sorted(p.name for p in run.image.parent.iterdir()), 'frame': frame(run, args.label)}))


def login_script():
    script = DATA / 'putgd/login/appimage-start.sh'
    if not script.exists(): return None
    lines = script.read_text().splitlines()
    return [l for l in lines if l.startswith(('configured=', 'update_information='))]


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
    """Serves a release from a local copy, with the run's mode applied (mode file in the run):
    image: a flipped byte in the AppImage; unsigned: no SHA256SUMS.minisig; sums: SHA256SUMS rewritten to match a
    flipped image, under the original signature; foreign: SHA256SUMS.minisig by another (lab) key;
    comment: the lab stable key's signature, but its trusted comment names the nightly channel."""
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
        if mode in ('foreign', 'comment') and name == 'SHA256SUMS.minisig':
            data = (path.parent / f'SHA256SUMS.minisig.{mode}').read_bytes()
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
    """Serve a release locally (downloaded once from the lab or the live mirror; the foreign and comment signatures come
    from --signatures), restart the resident against it, and try each mode: an update, or with --channel a switch."""
    run = Run(args.name)
    root = run.dir / 'tamper-root'
    tag = args.tag
    release = root / 'releases' / tag
    if not release.exists():
        release.mkdir(parents=True)
        for f in ('SHA256SUMS', 'SHA256SUMS.minisig', f'putgd-{tag[1:]}-x86_64.AppImage'):
            urllib.request.urlretrieve(f'{LAB if args.source == "lab" else "https://pkg.put.gd"}/releases/{tag}/{f}', release / f)
        for mode in ('foreign', 'comment'):
            if mode in args.modes.split(','):
                shutil.copy2(Path(args.signatures) / f'SHA256SUMS.minisig.{mode}', release / f'SHA256SUMS.minisig.{mode}')
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
    run.cli('update.check', timeout=90)
    for mode in args.modes.split(','):
        (run.dir / 'tamper-mode').write_text(mode + '\n')
        reply = run.cli('update.install', {'confirm': True, 'method': 'download'} | ({'channel': args.channel} if args.channel else {}))
        run.wait(lambda: not run.ok('update.status')['install'].get('running'), 600)
        time.sleep(1.5)
        final = run.ok('update.status')['install']
        results[mode] = {'reply': reply.get('ok'), 'state': final.get('state'), 'error': final.get('error'),
                         'imageSha256': afa.sha256(run.image), 'folder': sorted(p.name for p in run.image.parent.iterdir()),
                         'frame': frame(run, f'tamper-{mode}')}
    server.shutdown()
    r = run.cli('ui.quit', timeout=15)
    out(run.record('tamper', {'base': base, 'source': args.source, 'tag': tag, 'results': results, 'unchanged': all(v['imageSha256'] == run.meta['imageSha256'] for v in results.values())}))


# ---------------------------------------------------------------- frames and input

def frame_only(args):
    run = Run(args.name)
    out(run.record('frame', {'label': args.label, 'frame': frame(run, args.label)}))


def safe_handler(session):
    """Hard guard (after #415): every click or key chord runs only while links can't reach the user's own browser. The
    session's http and https default must be #326's stand-in recorder (i326.py handler-on) or the temporary Firefox entry
    (ffdefault.py on), or be Firefox whose every install defaults to a temporary putgd-afa2-* profile (ffprofile.py on).
    Exits 4 otherwise."""
    import configparser
    env = dict(session); env['PATH'] = '/usr/bin:/bin'
    found = {s: (afa.sh('gio', 'mime', f'x-scheme-handler/{s}', env=env).splitlines() or [''])[0] for s in ('http', 'https')}
    entries = (afa.RECORDER, 'putgd-afa2-firefox.desktop')
    ok = all(v.rstrip().endswith(entries) for v in found.values())
    if not ok and all('firefox' in v for v in found.values()):
        for root in (Path.home() / '.mozilla/firefox', Path.home() / 'snap/firefox/common/.mozilla/firefox'):
            ini = root / 'profiles.ini'
            if ini.exists():
                c = configparser.RawConfigParser(); c.optionxform = str; c.read(ini)
                installs = [x for x in c.sections() if x.startswith('Install')]
                ok = bool(installs) and all(c.get(x, 'Default', fallback='').startswith('putgd-afa2-') for x in installs)
    if ok: return found
    print(json.dumps({'refused': 'no click: links could reach the user\'s own browser (run i326.py handler-on, or ffdefault.py/ffprofile.py on)',
                      'gio': found}), file=sys.stderr, flush=True)
    raise SystemExit(4)


def pointer(args):
    """A real pointer click (or a key chord) on the desktop: xdotool on X11, the uinput agent on physical
    Wayland (tools/lab/host_input.py), the VNC server in a VM. Coordinates are frame pixels. Guarded by safe_handler."""
    run = Run(args.name)
    safe_handler(run.meta['session'])
    import rfb
    c = rfb.RFB(run.meta['vnc'], host=os.environ.get('UPD_VNC_HOST', '127.0.0.1')); size = c.frame()[:2]; c.close()
    run.meta.setdefault('input', 'vnc')
    inp = afa.Input(run, size)
    if args.cmd == 'click': inp.click(args.x, args.y, args.button)
    else: inp.key(*args.keys)
    time.sleep(args.settle)
    out(run.record(args.cmd, {'x': getattr(args, 'x', None), 'y': getattr(args, 'y', None), 'keys': getattr(args, 'keys', None),
                              'frame': frame(run, args.label) if args.label else None}))


# ---------------------------------------------------------------- U12 refusals

def install_outcome(run, params, label, timeout=900):
    """update.install, then (when it started) its end state; the image and its folder afterwards."""
    reply = run.cli('update.install', params)
    if reply.get('ok'):
        run.wait(lambda: not run.ok('update.status')['install'].get('running'), timeout)
        time.sleep(1)
    show_updates(run)
    st = run.ok('update.status')
    return {'params': params, 'reply': {'ok': reply.get('ok'), 'error': reply.get('error')},
            'install': {k: st['install'].get(k) for k in ('state', 'phase', 'error', 'available', 'reason', 'restartRequired')},
            'imageSha256': afa.sha256(run.image), 'unchanged': afa.sha256(run.image) == run.meta['imageSha256'],
            'folder': sorted(p.name for p in run.image.parent.iterdir()), 'frame': frame(run, label)}


def synthetic_png(path, w=320, h=200):
    import struct, zlib
    raw = b''.join(b'\0' + b''.join(bytes((x * 255 // w, y * 255 // h, 160)) for x in range(w)) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def socket_request(run, method, params):
    import socket
    path = str(Path(run.meta['runtime']) / 'putgd/session.sock')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(20); s.connect(path)
        s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'ua-' + method, 'session': path, 'method': method, 'params': params}).encode() + b'\n')
        data = b''
        while not data.endswith(b'\n'):
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
    return json.loads(data)


def refuse(args):
    """One U12 case. readonly: the AppImage's folder is read-only. foreign: the AppImage belongs to root (sudo chown, then
    back). diskfull: the resident runs from a copy on a tmpfs with no room for a second copy (sudo mount, then umount).
    busy: an upload to a loopback S3 fixture that holds the PUT for 90 s is running. double: a second install while the
    first runs. restart: an installed update still waits for its restart (run it right after `install`)."""
    run = Run(args.name)
    label = f'refuse-{args.case}'
    run.cli('update.check', timeout=90)
    params = {'confirm': True} | ({'method': args.method} if args.method != 'auto' else {})
    report = {'case': args.case}
    if args.case == 'readonly':
        folder = run.image.parent
        folder.chmod(0o555)
        try: report.update(install_outcome(run, params, label))
        finally: folder.chmod(0o700)
    elif args.case == 'foreign':
        subprocess.run(['sudo', '-n', 'chown', 'root:root', str(run.image)], check=True)
        try: report.update(install_outcome(run, params, label)); report['owner'] = 'root'
        finally: subprocess.run(['sudo', '-n', 'chown', f'{os.getuid()}:{os.getgid()}', str(run.image)], check=True)
    elif args.case == 'diskfull':
        small = run.dir / 'small'; small.mkdir(mode=0o700)
        size = run.image.stat().st_size + 24 * 1048576
        subprocess.run(['sudo', '-n', 'mount', '-t', 'tmpfs', '-o', f'size={size},uid={os.getuid()},gid={os.getgid()},mode=0700', 'putgd-ua-small', str(small)], check=True)
        original = run.meta['image']
        try:
            pid = resident_pid(run)
            if pid: run.cli('ui.quit', timeout=15); run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40)
            shutil.copy2(original, small / run.image.name)
            run.meta['image'] = str(small / run.image.name); run.save()
            launch(run, 'diskfull'); run.cli('update.check', timeout=90)
            report.update(install_outcome(run, params, label)); report['tmpfsBytes'] = size
            pid = resident_pid(run)
            run.cli('ui.quit', timeout=15); run.wait(lambda: not pid or not Path(f'/proc/{pid}').exists(), 40)
        finally:
            run.meta['image'] = original; run.save()
            subprocess.run(['sudo', '-n', 'umount', str(small)], check=True); small.rmdir()
        launch(run, 'after-diskfull')
    elif args.case == 'busy':
        fixture = subprocess.Popen([sys.executable, '-I', str(HERE / 's3_server.py')], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                   text=True, start_new_session=True)
        port = int(fixture.stdout.readline())
        try:
            rules = json.dumps({'rules': [{'method': 'PUT', 'delay': 90, 'remaining': 50}]}).encode()
            urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}/__rules', data=rules, method='POST'), timeout=10)
            doc = run.ok('settings.get')['document']
            doc['profiles'] = [p for p in doc.get('profiles', []) if p['id'] != 'ua-fixture'] + [{
                'credentials': {'mode': 'session', 'reference': None}, 'id': 'ua-fixture', 'label': 'Loopback fixture', 'provider': 's3',
                'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'fixture', 'connectTimeoutMs': 5000,
                             'endpoint': f'http://127.0.0.1:{port}', 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120, 'objectPrefix': 'ua/',
                             'publicUrlBase': f'http://127.0.0.1:{port}/fixture', 'region': 'us-east-1', 'requestTimeoutMs': 180000, 'storageClass': 'STANDARD'}}]
            doc.setdefault('actions', {}).setdefault('edit', {}).update({'destination': 'upload', 'profileId': 'ua-fixture'})
            (run.dir / 'config/putgd/config.json').write_text(json.dumps(doc, indent=1)); run.ok('settings.reload')
            snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result']['metadata']['validation']['ok'] else None)(run.cli('settings.get')), 20)
            # The CLI refuses setup.storeCredentials by design (docs/cli.md); the session socket takes it, as Settings does.
            stored = socket_request(run, 'setup.storeCredentials', {'profileId': 'ua-fixture', 'expectedRevision': snap['result']['metadata']['acceptedRevision'],
                                                                    'credentials': {'accessKey': 'PUTGD_SYNTHETIC_ACCESS', 'secretKey': 'putgd-synthetic-secret-not-a-real-key'}})
            if stored.get('ok'):
                run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': stored['result']['setupId']})), 40)
            time.sleep(2)
            png = run.dir / 'busy.png'; synthetic_png(png)
            eid = run.ok('editor.open', {'source': 'file', 'path': str(png)})['operationId']
            run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
            sub = run.ok('editor.submit', {'id': eid})
            time.sleep(4)
            report['upload'] = {'stored': stored.get('ok'), 'submitted': sub, 'putsSeen': sum(1 for e in json.loads(urllib.request.urlopen(
                f'http://127.0.0.1:{port}/__state', timeout=10).read())['events'] if e['method'] == 'PUT')}
            report.update(install_outcome(run, params, label))
        finally:
            fixture.terminate(); fixture.wait(10)
    elif args.case == 'double':
        first = run.cli('update.install', params)
        second = run.cli('update.install', params)
        report['first'] = {'ok': first.get('ok'), 'error': first.get('error')}
        report['second'] = {'ok': second.get('ok'), 'error': second.get('error')}
        run.wait(lambda: not run.ok('update.status')['install'].get('running'), 900)
        report['frame'] = frame(run, label)
        report['install'] = {k: run.ok('update.status')['install'].get(k) for k in ('state', 'error', 'restartRequired')}
    elif args.case == 'restart':
        report.update(install_outcome(run, params, label))
    out(run.record('refuse', report))


# ---------------------------------------------------------------- U8 appimageupdatetool

def aut(args):
    """appimageupdatetool on the run's image, as a user runs it: --check-for-update, then the update (written beside the
    old file). On Fedora pass --ssl-cert-file (#297). The new file's SHA-256 is compared with the release's SHA256SUMS."""
    run = Run(args.name)
    tool = Path(args.tool).resolve()
    env = dict(run.meta['session']); env['PATH'] = run.meta['hostPath']
    if args.ssl_cert_file: env['SSL_CERT_FILE'] = args.ssl_cert_file
    before = sorted(p.name for p in run.image.parent.iterdir())
    check_ = subprocess.run([str(tool), '--check-for-update', str(run.image)], capture_output=True, text=True, timeout=180, env=env)
    started = time.time()
    update = subprocess.run([str(tool), str(run.image)], capture_output=True, text=True, timeout=1800, env=env, cwd=run.image.parent)
    after = sorted(p.name for p in run.image.parent.iterdir())
    new = [n for n in after if n not in before]
    sums = {}
    if args.tag:
        with urllib.request.urlopen(f'https://pkg.put.gd/releases/{args.tag}/SHA256SUMS', timeout=30) as r:
            sums = {l.split()[1]: l.split()[0] for l in r.read().decode().splitlines() if l.strip()}
    files = {n: afa.sha256(run.image.parent / n) for n in new if (run.image.parent / n).is_file()}
    report = {'tool': tool.name, 'toolSha256': afa.sha256(tool), 'sslCertFile': args.ssl_cert_file,
              'check': {'exit': check_.returncode, 'output': (check_.stdout + check_.stderr)[-1200:]},
              'update': {'exit': update.returncode, 'seconds': round(time.time() - started, 1), 'output': (update.stdout + update.stderr)[-2500:]},
              'before': before, 'after': after, 'new': files,
              'matchesSums': {n: sums.get(n) == h for n, h in files.items()} if sums else None, 'oldUnchanged': afa.sha256(run.image) == run.meta['imageSha256']}
    out(run.record('appimageupdatetool', report))


# ---------------------------------------------------------------- U14 notifications

def watch(args):
    """start: dbus-monitor on the session bus's org.freedesktop.Notifications, to logs/notifications-LABEL.log (its PID in
    run.json). stop: stop that exact PID; report the Notify calls (app, summary, body) and put.gd's own journal lines."""
    run = Run(args.name)
    log = run.dir / f'logs/notifications-{args.label}.log'
    if args.action == 'start':
        p = subprocess.Popen(['dbus-monitor', "interface='org.freedesktop.Notifications'"], env=run.meta['session'] | {'PATH': run.meta['hostPath']},
                             stdout=open(log, 'w'), stderr=subprocess.DEVNULL, start_new_session=True)
        run.meta['pids'][f'watch-{args.label}'] = p.pid; run.meta[f'watchStart-{args.label}'] = afa.iso(); run.save()
        out(run.record('watch', {'label': args.label, 'pid': p.pid, 'started': run.meta[f'watchStart-{args.label}']})); return
    pid = run.meta['pids'].pop(f'watch-{args.label}')
    os.kill(pid, signal.SIGTERM); run.save()
    out(run.record('watch', notifications(run, log, args.label)))


def notifications(run, log, label):
    import re
    text = log.read_text(errors='replace') if log.exists() else ''
    calls = []
    for block in text.split('method call')[1:]:
        if 'member=Notify' not in block.split('\n', 1)[0]: continue
        strings = re.findall(r'^\s+string "(.*)"$', block, re.M)
        calls.append({'app': strings[0] if strings else None, 'summary': strings[2] if len(strings) > 2 else None,
                      'body': (strings[3] if len(strings) > 3 else '')[:300]})
    journal = []
    for path in sorted((run.dir / 'state').rglob('putgd-*.jsonl')):
        for line in path.read_text(errors='replace').splitlines():
            if 'Notification shown' in line or 'Update check finished' in line:
                try: d = json.loads(line); journal.append({k: d.get(k) for k in ('time', 'message', 'fields') if k in d})
                except ValueError: pass
    return {'label': label, 'from': run.meta.get(f'watchStart-{label}'), 'to': afa.iso(), 'notifyCalls': calls,
            'putgdNotifyCalls': [c for c in calls if (c['app'] or '').lower().startswith('put')], 'journal': journal[-12:]}


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
    for key, pid in list(run.meta.get('pids', {}).items()):
        if key.startswith('watch-') and Path(f'/proc/{pid}').exists(): os.kill(pid, signal.SIGTERM)
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
    a = sub.add_parser('setup'); a.add_argument('name'); a.add_argument('--appimage', required=True); a.add_argument('--channel', default='build')
    a.add_argument('--tool'); a.add_argument('--crash', action='store_true'); a.add_argument('--live', action='store_true')
    a.add_argument('--notifications', action='store_true'); a.add_argument('--input', choices=('xdotool', 'uinput', 'vnc'), default='vnc')
    a.add_argument('--vnc-port', type=int, default=5900); a.add_argument('--allow-data-home', action='store_true')
    for name in ('quit', 'cleanup'):
        sub.add_parser(name).add_argument('name')
    for name in ('check', 'restart'):
        a = sub.add_parser(name); a.add_argument('name'); a.add_argument('--label', default=name)
    a = sub.add_parser('channel'); a.add_argument('name'); a.add_argument('channel', choices=('build', 'stable', 'nightly')); a.add_argument('--label', default='channel')
    a = sub.add_parser('start'); a.add_argument('name'); a.add_argument('--label', default='start')
    a = sub.add_parser('install'); a.add_argument('name'); a.add_argument('--method', default='auto', choices=('auto', 'zsync', 'download'))
    a.add_argument('--channel'); a.add_argument('--allow-older', action='store_true'); a.add_argument('--label', default='install')
    a = sub.add_parser('mem'); a.add_argument('name'); a.add_argument('--action', choices=('install', 'check'), required=True)
    a.add_argument('--seconds', type=int, default=180); a.add_argument('--settle', type=int, default=30)
    a = sub.add_parser('tamper'); a.add_argument('name'); a.add_argument('--tag', required=True); a.add_argument('--signatures')
    a.add_argument('--source', choices=('lab', 'live'), default='lab'); a.add_argument('--modes', default='image,unsigned,sums'); a.add_argument('--channel')
    a = sub.add_parser('login'); a.add_argument('name'); a.add_argument('action', choices=('enable', 'check', 'disable'))
    a = sub.add_parser('frame'); a.add_argument('name'); a.add_argument('--label', required=True)
    a = sub.add_parser('click'); a.add_argument('name'); a.add_argument('x', type=int); a.add_argument('y', type=int)
    a.add_argument('--button', default='left'); a.add_argument('--label'); a.add_argument('--settle', type=float, default=2)
    a = sub.add_parser('key'); a.add_argument('name'); a.add_argument('keys', nargs='+'); a.add_argument('--label'); a.add_argument('--settle', type=float, default=1)
    a = sub.add_parser('refuse'); a.add_argument('name'); a.add_argument('case', choices=('readonly', 'foreign', 'diskfull', 'busy', 'double', 'restart'))
    a.add_argument('--method', default='download', choices=('auto', 'zsync', 'download'))
    a = sub.add_parser('aut'); a.add_argument('name'); a.add_argument('--tool', required=True); a.add_argument('--tag'); a.add_argument('--ssl-cert-file')
    a = sub.add_parser('watch'); a.add_argument('name'); a.add_argument('action', choices=('start', 'stop')); a.add_argument('--label', required=True)
    args = p.parse_args()
    {'setup': setup, 'start': start, 'check': check, 'channel': channel, 'install': install, 'restart': restart, 'quit': quit_, 'tamper': tamper, 'mem': mem,
     'login': login, 'cleanup': cleanup, 'frame': frame_only, 'click': pointer, 'key': pointer, 'refuse': refuse, 'aut': aut, 'watch': watch}[args.cmd](args)


if __name__ == '__main__':
    main()
