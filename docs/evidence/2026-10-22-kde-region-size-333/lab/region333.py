#!/usr/bin/env python3
"""#333 region-size driver, run on a Plasma/GNOME/Cinnamon host from SSH (stdlib + rfb.py).

  region333.py setup NAME (--appimage PATH | --installed)
  region333.py start NAME
  region333.py region NAME --drag x0,y0,x1,y1 --map lab|afa [--size W,H] [--frame LABEL] [--input SOCK|xdotool]
  region333.py memory NAME [--seconds N]
  region333.py quit NAME
  region333.py cleanup NAME

Runs live under $R333_ROOT/runs/NAME with private XDG config/state/cache/runtime.
Output is local only (archive.keepUploads, no upload profile). `--map lab` is
tools/lab/lab.py's pointer() (first tick at or above each pixel edge); `--map afa`
is the 2026-10-21 A-K driver's int(x*32767/(extent-1)) mapping that found #333.
"""
import argparse, json, math, os, socket, struct, subprocess, sys, time, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
ROOT = Path(os.environ.get('R333_ROOT', str(Path.home() / 'work/put.gd-333')))
SHELLS = ('plasmashell', 'gnome-shell', 'cinnamon')
KEEP = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP', 'WAYLAND_DISPLAY',
        'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION', 'XDG_DATA_DIRS', 'LANG')


def out(o): print(json.dumps(o, indent=1, default=str), flush=True)


def session_env():
    for shell in SHELLS:
        pids = subprocess.run(['pgrep', '-u', str(os.getuid()), '-x', shell], capture_output=True, text=True).stdout.split()
        if pids: break
    else: raise SystemExit('no desktop session process')
    found = dict(p.split('=', 1) for p in Path(f'/proc/{pids[0]}/environ').read_bytes().decode(errors='replace').split('\0') if '=' in p)
    env = {'HOME': os.environ['HOME'], 'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'USER': os.environ.get('USER', ''), 'LANG': found.get('LANG', 'C.UTF-8')}
    env.update({k: found[k] for k in KEEP if found.get(k)})
    if shell == 'gnome-shell' or (env.get('XDG_SESSION_TYPE') == 'wayland' and not env.get('WAYLAND_DISPLAY')):
        shown = subprocess.run(['systemctl', '--user', 'show-environment'], capture_output=True, text=True).stdout
        exported = dict(l.split('=', 1) for l in shown.splitlines() if '=' in l)
        for k in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY'):
            if k in exported: env.setdefault(k, exported[k])
    if env.get('WAYLAND_DISPLAY') and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = f'/run/user/{os.getuid()}/' + env['WAYLAND_DISPLAY']
    return env, shell


class Run:
    def __init__(self, name):
        self.dir = ROOT / 'runs' / name; self.path = self.dir / 'run.json'
        self.meta = json.loads(self.path.read_text()) if self.path.exists() else {}
    def save(self): self.path.write_text(json.dumps(self.meta, indent=1) + '\n')
    def env(self):
        env = dict(self.meta['session'])
        for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')): env[key] = str(self.dir / sub)
        env['XDG_RUNTIME_DIR'] = self.meta['runtime']; env['PIPEWIRE_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        return env
    def cli(self, method, params=None, timeout=70):
        env = self.env()
        if self.meta.get('appdir'):
            lib = Path(self.meta['appdir']) / 'usr/lib'; exe = str(Path(self.meta['appdir']) / 'usr/bin/putgd-cli')
            env['LD_LIBRARY_PATH'] = f'{lib}:{lib}/putgd'; env['QT_PLUGIN_PATH'] = f"{self.meta['appdir']}/usr/plugins:{lib}/qt6/plugins"
        else: exe = '/usr/bin/putgd-cli'
        r = subprocess.run([exe, method, json.dumps(params or {})], env=env, timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}
    def ok(self, method, params=None, timeout=70):
        r = self.cli(method, params, timeout)
        if not r.get('ok'): raise SystemExit(f'{method}: {json.dumps(r)[:600]}')
        return r['result']
    def wait(self, check, timeout, step=.3):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            v = check()
            if v: return v
            time.sleep(step)


def launch(run, label):
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    log = open(run.dir / f'logs/launch-{label}.log', 'ab')
    cmd = [run.meta['appimage'], '--background'] if run.meta.get('appimage') else ['/usr/bin/putgd', '--background']
    p = subprocess.Popen(cmd, env=run.env(), stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    run.meta.setdefault('pids', {})['launch'] = p.pid; run.save()
    if not run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60):
        raise SystemExit(f'resident not ready; see logs/launch-{label}.log')
    return p


def quit_resident(run):
    r = run.cli('ui.quit', timeout=15); pid = run.meta.get('pids', {}).get('launch')
    gone = run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40) if pid else True
    return {'quit': r.get('ok'), 'gone': bool(gone)}


def residents(run):
    found = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try: env = (p / 'environ').read_bytes(); comm = (p / 'comm').read_text().strip(); exe = os.readlink(p / 'exe')
        except OSError: continue
        if run.meta['runtime'].encode() in env and comm.startswith('putgd'): found.append({'pid': int(p.name), 'comm': comm, 'exe': exe})
    return found


def setup(a):
    run = Run(a.name)
    if run.meta: raise SystemExit('run exists')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'archive', 'logs', 'out'): (run.dir / sub).mkdir(mode=0o700)
    session, shell = session_env()
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/r333.XXXXXX'], text=True).strip()
    run.meta = {'session': session, 'shell': shell, 'runtime': runtime, 'created': time.strftime('%FT%TZ', time.gmtime())}
    if a.appimage:
        image = Path(a.appimage).resolve(); (run.dir / 'appdir').mkdir(mode=0o700)
        subprocess.run([str(image), '--appimage-extract'], cwd=run.dir / 'appdir', check=True, stdout=subprocess.DEVNULL)
        run.meta.update(appimage=str(image), appdir=str(run.dir / 'appdir/squashfs-root'))
    run.save()
    resident = launch(run, 'setup')
    doc = dict(run.ok('settings.developmentTemplate')['document'])
    doc['schemaVersion'] = 1
    doc['actions'] = {n: {'destination': 'local', 'profileId': None, 'shortcut': ''} for n in ('clipboard', 'desktop', 'edit', 'region', 'window')} | {'record': {'shortcut': ''}}
    doc['archive'] = {'folders': {'images': None, 'other': None, 'text': None, 'videos': None}, 'keepUploads': True, 'root': str(run.dir / 'archive')}
    doc['profiles'] = []
    prefs = dict(doc.get('preferences') or {}); prefs.update({'launchAtLogin': False, 'notifications': False, 'unlockCredentialsAtStartup': False}); doc['preferences'] = prefs
    (run.dir / 'config/putgd').mkdir(exist_ok=True); (run.dir / 'config/putgd/config.json').write_text(json.dumps(doc, indent=1))
    run.ok('settings.reload')
    snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result']['metadata']['validation']['ok'] else None)(run.cli('settings.get')), 20)
    out({'setup': bool(snap), 'quit': quit_resident(run), 'meta': run.meta})


def start(a):
    run = Run(a.name); launch(run, 'main')
    d = run.ok('diagnostics')
    out({'build': d.get('build'), 'release': d.get('release'), 'backend': d.get('backend'), 'qtVersion': d.get('qtVersion'),
         'region': ((d.get('capabilities') or {}).get('capture') or {}).get('region'), 'residents': residents(run)})


def ticks(mapping, x, y, w, h):
    if mapping == 'afa': return int(x * 32767 / (w - 1)), int(y * 32767 / (h - 1))
    return min(32767, math.ceil(x * 32768 / w)), min(32767, math.ceil(y * 32768 / h))


class Input:
    def __init__(self, target, mapping, size, env):
        self.target, self.mapping, self.size, self.env = target, mapping, size, env
    def _send(self, events):
        s = socket.socket(socket.AF_UNIX); s.connect(self.target); f = s.makefile('rwb')
        f.readline(); f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        f.write(json.dumps({'execute': 'input-send-event', 'arguments': {'events': events}}).encode() + b'\n'); f.flush()
        r = json.loads(f.readline()); s.close()
        if 'error' in r: raise RuntimeError(r)
    def move(self, x, y):
        if self.target == 'xdotool': return subprocess.run(['xdotool', 'mousemove', str(x), str(y)], env=self.env, check=True)
        tx, ty = ticks(self.mapping, x, y, *self.size)
        self._send([{'type': 'abs', 'data': {'axis': 'x', 'value': tx}}, {'type': 'abs', 'data': {'axis': 'y', 'value': ty}}])
    def button(self, down):
        if self.target == 'xdotool': return subprocess.run(['xdotool', 'mousedown' if down else 'mouseup', '1'], env=self.env, check=True)
        self._send([{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])


def _unused(path, w, h, rows):
    raw = b''.join(b'\0' + r for r in rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def png_size(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n': return None
    return list(struct.unpack('>II', data[16:24]))


def region(a):
    run = Run(a.name)
    size = [int(v) for v in a.size.split(',')]
    inp = Input(a.input, a.map, size, run.env())
    x0, y0, x1, y1 = [int(v) for v in a.drag.split(',')]
    r = run.ok('action.invoke', {'action': 'region'}); time.sleep(a.selector_wait)
    inp.move(x0, y0); time.sleep(.3); inp.button(True); time.sleep(.2)
    steps = 14
    for i in range(1, steps + 1):
        inp.move(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps); time.sleep(.04)
    time.sleep(.4)
    frame = None
    if a.frame:
        import rfb
        frame = run.dir / 'out' / f'{a.frame}.png'; c = rfb.RFB(a.vnc); c.screenshot(frame); c.close()
    inp.button(False)
    op = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'partial', 'failed', 'interrupted', 'cancelled') else None)(run.cli('operation.inspect', {'id': r['operationId']})), 60, .5)
    item = ((op or {}).get('items') or [{}])[0]
    local = (item.get('local') or {}).get('path')
    hist = run.ok('history.list', {'limit': 20, 'offset': 0})['records']
    entry = next((h for h in hist if h.get('operationId') == r['operationId']), None)
    report = {'drag': [x0, y0, x1, y1], 'map': a.map, 'ticks': [ticks(a.map, x0, y0, *size), ticks(a.map, x1, y1, *size)] if a.input != 'xdotool' else None,
              'expected': [x1 - x0, y1 - y0], 'operation': {'id': (op or {}).get('id'), 'state': (op or {}).get('state'), 'error': (op or {}).get('error'),
              'artifact': item.get('artifact'), 'local': item.get('local')},
              'history': {k: (entry or {}).get(k) for k in ('id', 'name', 'operationId', 'sha256', 'bytes', 'local')} if entry else None,
              'savedFile': {'path': local, 'exists': bool(local and Path(local).exists()), 'pngSize': png_size(Path(local)) if local and Path(local).exists() else None},
              'frame': str(frame) if frame else None}
    if local and Path(local).exists():
        dest = run.dir / 'out' / f"region-{a.label or a.map}-{x0}-{y0}-{x1}-{y1}-{time.strftime('%H%M%S')}.png"; dest.write_bytes(Path(local).read_bytes()); report['copy'] = str(dest)
    import hashlib
    saved = hashlib.sha256(Path(local).read_bytes()).hexdigest() if local and Path(local).exists() else None
    report['sha256'] = {'operation': (item.get('artifact') or {}).get('sha256'), 'history': (entry or {}).get('sha256'), 'savedFile': saved}
    report['sha256']['agree'] = bool(saved) and len(set(report['sha256'].values()) - {True, False}) == 1
    report['pass'] = report['savedFile']['pngSize'] == report['expected'] and report['sha256']['agree']
    results = run.dir / 'results.json'; doc = json.loads(results.read_text()) if results.exists() else {}
    doc.setdefault('regions', []).append(report); results.write_text(json.dumps(doc, indent=1, default=str) + '\n')
    out(report)


def memory(a):
    run = Run(a.name)
    rs = [p for p in residents(run) if p['comm'] == 'putgd']
    if len(rs) != 1: raise SystemExit(f'resident not unique: {rs}')
    pid = rs[0]['pid']; page = os.sysconf('SC_PAGE_SIZE'); samples = []; t0 = time.monotonic(); load = [os.getloadavg()]
    while True:
        e = time.monotonic() - t0; samples.append({'t': round(e, 1), 'rssMiB': round(int(Path(f'/proc/{pid}/statm').read_text().split()[1]) * page / 2**20, 3)})
        if e >= a.seconds: break
        time.sleep(min(10, max(a.seconds - e, .1)))
    roll = {l.split(':')[0]: int(l.split(':')[1].split()[0]) * 1024 for l in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]}
    load.append(os.getloadavg())
    report = {'pid': pid, 'exe': rs[0]['exe'], 'seconds': a.seconds, 'finalRssMiB': samples[-1]['rssMiB'], 'peakRssMiB': max(s['rssMiB'] for s in samples),
              'pssMiB': round(roll['Pss'] / 2**20, 3), 'samples': samples, 'load': load}
    results = run.dir / 'results.json'; doc = json.loads(results.read_text()) if results.exists() else {}
    doc.setdefault('memory', []).append(report); results.write_text(json.dumps(doc, indent=1) + '\n'); out(report)


def quit_cmd(a): run = Run(a.name); out({**quit_resident(run), 'left': residents(run)})


def cleanup(a):
    run = Run(a.name)
    left = residents(run)
    if left: raise SystemExit(f'processes still running: {left}')
    import shutil
    shutil.rmtree(run.meta['runtime'], ignore_errors=True)
    if run.meta.get('appdir'): shutil.rmtree(run.dir / 'appdir', ignore_errors=True)
    out({'removed': [run.meta['runtime'], str(run.dir / 'appdir') if run.meta.get('appdir') else None], 'kept': str(run.dir)})


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='cmd', required=True)
    x = s.add_parser('setup'); x.add_argument('name'); g = x.add_mutually_exclusive_group(required=True); g.add_argument('--appimage'); g.add_argument('--installed', action='store_true')
    for n in ('start', 'quit', 'cleanup'): s.add_parser(n).add_argument('name')
    x = s.add_parser('memory'); x.add_argument('name'); x.add_argument('--seconds', type=int, default=180)
    x = s.add_parser('region'); x.add_argument('name'); x.add_argument('--drag', required=True); x.add_argument('--map', choices=('lab', 'afa'), default='lab')
    x.add_argument('--size', default='1920,1080'); x.add_argument('--frame'); x.add_argument('--vnc', type=int, default=5900); x.add_argument('--label')
    x.add_argument('--input', default=str(ROOT / 'probe/run/in.sock')); x.add_argument('--selector-wait', type=float, default=2.5)
    a = p.parse_args()
    {'setup': setup, 'start': start, 'region': region, 'memory': memory, 'quit': quit_cmd, 'cleanup': cleanup}[a.cmd](a)


if __name__ == '__main__':
    main()
