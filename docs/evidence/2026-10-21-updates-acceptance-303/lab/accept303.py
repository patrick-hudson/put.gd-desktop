#!/usr/bin/env python3
"""#303 desktop acceptance driver, run on a lab host as patrick.

Private profile, runtime dir and URL recorder per run NAME under
~/work/put.gd-303/p-NAME. Frames come from the host's loopback VNC and stay
on the host; only crops whose edges are all one background colour (the popup,
menu or window's own) are written as PNG for copying.
"""
import argparse, collections, json, os, signal, subprocess, sys, time
from datetime import datetime, timezone, timedelta
from pathlib import Path

ROOT = Path('/home/patrick/work/put.gd-303')
sys.path.insert(0, str(ROOT / 'src-head/tools/lab'))
SHELLS = {'cinnamon': 'cinnamon', 'plasma': 'plasmashell', 'gnome': 'gnome-shell'}
VNC = {'fedora': 5901, 'debian': 5900, 'mint-builder': 5900, 'kubuntu': 5900}
PARK = (1912, 620)

def now(): return datetime.now(timezone.utc)
def iso(t): return t.strftime('%Y-%m-%dT%H:%M:%SZ')
def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)

def session_env(desktop):
    pid = subprocess.check_output(['pgrep', '-u', str(os.getuid()), '-x', SHELLS[desktop]], text=True).split()[0]
    raw = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
    found = dict(item.decode(errors='replace').split('=', 1) for item in raw if b'=' in item)
    # gnome-shell is the compositor, so its own environ has no WAYLAND_DISPLAY or
    # DISPLAY; take what the session exported to systemd --user.
    if desktop == 'gnome':
        exported = dict(l.split('=', 1) for l in subprocess.check_output(['systemctl', '--user', 'show-environment'], text=True).splitlines() if '=' in l)
        for key in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY'):
            if key in exported: found.setdefault(key, exported[key])
    env = {k: found[k] for k in ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION') if k in found}
    runtime = found.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
    if 'WAYLAND_DISPLAY' in env and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = f"{runtime}/{env['WAYLAND_DISPLAY']}"
    return env

class Run:
    def __init__(self, name):
        self.dir = ROOT / f'p-{name}'; self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}
    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')
    def env(self):
        env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'QT_QPA_PLATFORM')}
        env.update(self.meta['session'])
        for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')):
            env[key] = str(self.dir / sub)
        env['XDG_RUNTIME_DIR'] = self.meta['runtime']
        if self.meta.get('feed'): env['PUTGD_UPDATE_FEED_URL'] = self.meta['feed']
        return env
    def cli(self, method, params=None, timeout=60):
        r = subprocess.run([self.meta['bin'] + '/putgd-cli', method, json.dumps(params or {})], env=self.env(), timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except Exception: return {'ok': False, 'raw': r.stdout[-500:], 'stderr': r.stderr[-500:]}
    def logs(self):
        lines = []
        for path in sorted((self.dir / 'state').rglob('putgd-*.jsonl')):
            for line in path.read_text(errors='replace').splitlines():
                try: lines.append(json.loads(line))
                except Exception: pass
        return lines
    def events(self):
        keep = ('Checking for updates', 'Update check finished', 'Update check failed', 'Notification shown')
        return [e for e in self.logs() if any(s in json.dumps(e) for s in keep)]

def setup(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    run.dir.mkdir(mode=0o700)
    for sub in ('config', 'data', 'cache', 'state', 'archive', 'frames', 'crops'):
        (run.dir / sub).mkdir(mode=0o700)
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/pm303.XXXXXX'], text=True).strip()
    run.meta = {'name': args.name, 'desktop': args.desktop, 'bin': str(Path(args.bin).resolve()), 'runtime': runtime,
                'feed': args.feed, 'session': session_env(args.desktop), 'host': os.uname().nodename, 'notifications': args.notifications}
    # URL recorder: the release-notes link lands here instead of a browser.
    recorder = run.dir / 'record-url.sh'
    recorder.write_text(f'#!/bin/sh\nprintf "%s %s\\n" "$(date -u +%FT%TZ)" "$1" >> {run.dir}/opened-urls.txt\n'); recorder.chmod(0o700)
    apps = run.dir / 'data/applications'; apps.mkdir(parents=True)
    (apps / 'putgd303-url.desktop').write_text(f'[Desktop Entry]\nType=Application\nName=put.gd 303 URL recorder\nExec={recorder} %u\nMimeType=x-scheme-handler/https;x-scheme-handler/http;\nNoDisplay=true\n')
    mime = '[Default Applications]\nx-scheme-handler/https=putgd303-url.desktop\nx-scheme-handler/http=putgd303-url.desktop\n'
    (run.dir / 'config/mimeapps.list').write_text(mime)
    (run.dir / 'data/applications/mimeapps.list').write_text(mime)
    (run.dir / 'config/kdeglobals').write_text('[General]\nBrowserApplication=putgd303-url.desktop\n')
    run.save()
    # Settings as the memory harness: local-only, no shortcuts, notifications as asked.
    # On screen like every launch here; this one only writes the settings.
    env = run.env()
    log = open(run.dir / 'resident-setup.log', 'wb')
    p = subprocess.Popen([run.meta['bin'] + '/putgd', '--background'], env=env, stdout=log, stderr=log)
    def req(method, params=None):
        r = subprocess.run([run.meta['bin'] + '/putgd-cli', method, json.dumps(params or {})], env=env, capture_output=True, text=True, timeout=20)
        return json.loads(r.stdout)
    deadline = time.monotonic() + 20
    while True:
        try:
            if req('diagnostics')['result']['journalReady']: break
        except Exception: pass
        if time.monotonic() > deadline or p.poll() is not None: raise SystemExit('setup resident not ready')
        time.sleep(.3)
    document = req('settings.developmentTemplate')['result']['document']
    document['profiles'] = []
    document['actions'] = {a: {'destination': 'local', 'profileId': None, 'shortcut': ''} for a in document['actions']}
    document['archive']['root'] = str(run.dir / 'archive')
    document['preferences'] = {'notifications': bool(args.notifications), 'launchAtLogin': False}
    revision = req('settings.get')['result']['metadata']['observedRevision']
    ok = req('settings.patch', {'patch': document, 'expectedRevision': revision})['ok']
    p.terminate(); p.wait(timeout=20); log.close()
    out({'setup': ok, 'run': run.meta})

def start(args):
    run = Run(args.name)
    if run.meta.get('pid') and Path(f"/proc/{run.meta['pid']}").exists(): raise SystemExit('already running')
    env = run.env()
    log = open(run.dir / 'resident.log', 'ab')
    p = subprocess.Popen([run.meta['bin'] + '/putgd', '--background'], env=env, stdout=log, stderr=log, stdin=subprocess.DEVNULL, start_new_session=True)
    run.meta['pid'] = p.pid; run.meta.setdefault('starts', []).append(iso(now())); run.save()
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        r = run.cli('diagnostics', timeout=10)
        if r.get('ok') and r['result'].get('journalReady'): break
        time.sleep(.3)
    time.sleep(2)
    out({'pid': p.pid, 'started': run.meta['starts'][-1], 'status': run.cli('update.status').get('result')})

def status(args):
    run = Run(args.name)
    alive = bool(run.meta.get('pid')) and Path(f"/proc/{run.meta['pid']}").exists()
    s = run.cli('update.status') if alive else None
    urls = (run.dir / 'opened-urls.txt').read_text() if (run.dir / 'opened-urls.txt').exists() else ''
    state = run.dir / 'state/putgd/update-check.json'
    out({'alive': alive, 'pid': run.meta.get('pid'), 'starts': run.meta.get('starts'), 'now': iso(now()),
         'status': (s or {}).get('result'), 'events': run.events(), 'openedUrls': urls.splitlines(),
         'stateFile': json.loads(state.read_text()) if state.exists() else None})

def stop(args):
    run = Run(args.name); pid = run.meta.get('pid')
    if pid and Path(f'/proc/{pid}').exists():
        os.kill(pid, signal.SIGTERM)
        for _ in range(100):
            if not Path(f'/proc/{pid}').exists(): break
            time.sleep(.2)
    children = subprocess.run(['pgrep', '-u', str(os.getuid()), '-f', run.meta['bin']], capture_output=True, text=True).stdout.split()
    out({'stopped': pid, 'leftover': children})

def age(args):
    run = Run(args.name); path = run.dir / 'state/putgd/update-check.json'
    state = json.loads(path.read_text()); before = state['attemptedAt']
    state['attemptedAt'] = iso(now() - timedelta(hours=args.hours)); path.write_text(json.dumps(state))
    out({'attemptedAt': [before, state['attemptedAt']], 'notifiedVersion': state.get('notifiedVersion')})

def menu(args):
    run = Run(args.name); env = run.env(); pid = run.meta['pid']
    bus = ['busctl', '--user', '--json=short']
    def call(*a):
        r = subprocess.run(bus + list(a), env=env, capture_output=True, text=True, timeout=10)
        return json.loads(r.stdout) if r.returncode == 0 else {'error': r.stderr.strip()}
    items = call('get-property', 'org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher', 'org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems')
    found = []
    for item in items.get('data', []):
        service = item.split('/')[0]
        owner = call('call', 'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetConnectionUnixProcessID', 's', service)
        if owner.get('data', [None])[0] == pid: found.append(service)
    result = {'watcherItems': items.get('data'), 'ours': found}
    if found:
        layout = call('call', found[0], '/Menu', 'com.canonical.dbusmenu', 'GetLayout', 'iias', '--', '0', '-1', '0')
        def walk(node):
            ident, props, children = node
            entries = []
            for child in children:
                c = child['data'] if isinstance(child, dict) else child
                p = {k: v['data'] for k, v in c[1].items()}
                entries.append({'id': c[0], 'label': p.get('label'), 'type': p.get('type', 'standard'), 'visible': p.get('visible', True), 'enabled': p.get('enabled', True)})
            return entries
        result['revision'] = layout['data'][0]; result['items'] = walk(layout['data'][1])
        result['title'] = call('get-property', found[0], '/StatusNotifierItem', 'org.kde.StatusNotifierItem', 'Title').get('data')
    out(result)

# ---- frames, diffs and verified crops ----
class Frames:
    def __init__(self):
        import rfb
        self.c = rfb.RFB(VNC[os.uname().nodename]); self.rfb = rfb
    def grab(self):
        w, h, px = self.c.frame(); return (w, h, px)

def pixel(f, x, y):
    w, h, px = f; i = (y * w + x) * 4; return px[i + 2], px[i + 1], px[i]
def differs(a, b, tol=18): return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > tol

def components(base, frame, block=8):
    w, h, _ = frame; changed = set()
    for by in range(h // block):
        for bx in range(w // block):
            x, y = bx * block, by * block
            for dx, dy in ((1, 1), (4, 4), (6, 2), (2, 6)):
                if differs(pixel(base, x + dx, y + dy), pixel(frame, x + dx, y + dy)): changed.add((bx, by)); break
    seen, comps = set(), []
    for cell in changed:
        if cell in seen: continue
        queue = collections.deque([cell]); seen.add(cell); cells = []
        while queue:
            cx, cy = queue.popleft(); cells.append((cx, cy))
            for nx in range(cx - 2, cx + 3):
                for ny in range(cy - 2, cy + 3):
                    if (nx, ny) in changed and (nx, ny) not in seen: seen.add((nx, ny)); queue.append((nx, ny))
        xs = [c[0] for c in cells]; ys = [c[1] for c in cells]
        comps.append({'blocks': len(cells), 'box': [min(xs) * block, min(ys) * block, (max(xs) + 1) * block, (max(ys) + 1) * block]})
    return sorted(comps, key=lambda c: -c['blocks'])

def dominant(frame, box):
    x0, y0, x1, y1 = box; counts = collections.Counter()
    for y in range(y0, y1, 3):
        for x in range(x0, x1, 3): counts[tuple(v // 4 * 4 for v in pixel(frame, x, y))] += 1
    return counts.most_common(1)[0][0]

def edge_ok(frame, box, colour, tol, share):
    x0, y0, x1, y1 = box
    edges = [[(x, y0) for x in range(x0, x1)], [(x, y1 - 1) for x in range(x0, x1)], [(x0, y) for y in range(y0, y1)], [(x1 - 1, y) for y in range(y0, y1)]]
    return [sum(not differs(pixel(frame, x, y), colour, tol) for x, y in e) / len(e) >= share for e in edges]

def shrink(frame, box, colour, tol=10, share=.9):
    """Move each edge inwards until that edge line is the region's own background colour."""
    x0, y0, x1, y1 = box
    for _ in range(400):
        ok = edge_ok(frame, (x0, y0, x1, y1), colour, tol, share)
        if all(ok): return [x0, y0, x1, y1]
        if not ok[0]: y0 += 1
        if not ok[1]: y1 -= 1
        if not ok[2]: x0 += 1
        if not ok[3]: x1 -= 1
        if x1 - x0 < 40 or y1 - y0 < 20: return None
    return None

def write_png(frame, box, path):
    import struct, zlib
    x0, y0, x1, y1 = box; w = x1 - x0; raw = bytearray()
    for y in range(y0, y1):
        raw.append(0)
        for x in range(x0, x1): raw += bytes(pixel(frame, x, y))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, y1 - y0, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))

THEME = [(24, 27, 33), (17, 19, 23), (17, 19, 24), (32, 36, 44), (245, 246, 248), (255, 255, 255), (237, 239, 243), (220, 231, 250), (213, 217, 224)]
def theme_share(frame, box):
    x0, y0, x1, y1 = box; total = hit = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            total += 1; hit += any(not differs(pixel(frame, x, y), t, 3) for t in THEME)
    return hit / max(total, 1)

def crop(frame, base, label, run, pick=None, grow=0, theme=False):
    comps = components(base, frame)
    report = {'label': label, 'components': comps[:8]}
    candidates = comps if pick is None else [c for c in comps if pick(c)]
    if not candidates: report['kept'] = None; return report
    box = candidates[0]['box']
    colour = dominant(frame, box)
    inner = shrink(frame, [box[0] + 2, box[1] + 2, box[2] - 2, box[3] - 2], colour)
    report.update(colour=colour, inner=inner)
    if inner:
        report['changedShare'] = round(sum(differs(pixel(base, x, y), pixel(frame, x, y), 0) for y in range(inner[1], inner[3], 3) for x in range(inner[0], inner[2], 3)) / len(range(inner[1], inner[3], 3)) / len(range(inner[0], inner[2], 3)), 3)
        if theme: report['themeShare'] = round(theme_share(frame, inner), 3)
        if all(edge_ok(frame, inner, colour, 10, .9)) and report['changedShare'] >= .5 and (not theme or report['themeShare'] >= .4):
            path = run.dir / 'crops' / f'{label}.png'; write_png(frame, inner, path); report['kept'] = str(path)
    return report

class Input:
    """xdotool on X11; the uinput agent (tools/lab/host_input.py) on Wayland."""
    def __init__(self, run):
        self.run = run; self.x11 = run.meta['session'].get('XDG_SESSION_TYPE') == 'x11'; self.size = (1920, 1080)
    def _qmp(self, events):
        import socket
        s = socket.socket(socket.AF_UNIX); s.connect(f'/run/user/{os.getuid()}/putgd-lab-input.sock'); f = s.makefile('rwb')
        f.readline(); f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        f.write(json.dumps({'execute': 'input-send-event', 'arguments': {'events': events}}).encode() + b'\n'); f.flush()
        reply = json.loads(f.readline()); s.close()
        if 'error' in reply: raise RuntimeError(reply)
    def move(self, x, y):
        if self.x11: return subprocess.run(['xdotool', 'mousemove', str(x), str(y)], env=self.run.env(), check=True)
        self._qmp([{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (self.size[0] - 1))}}, {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (self.size[1] - 1))}}])
    def click(self, x, y, button='left'):
        self.move(x, y); time.sleep(.15)
        if self.x11:
            b = {'left': '1', 'right': '3'}[button]
            subprocess.run(['xdotool', 'mousedown', b], env=self.run.env(), check=True); time.sleep(.12)
            return subprocess.run(['xdotool', 'mouseup', b], env=self.run.env(), check=True)
        self._qmp([{'type': 'btn', 'data': {'down': True, 'button': button}}]); time.sleep(.12)
        self._qmp([{'type': 'btn', 'data': {'down': False, 'button': button}}])
    def key(self, name):
        if self.x11:
            return subprocess.run(['xdotool', 'key', {'down': 'Down', 'ret': 'Return', 'esc': 'Escape', 'up': 'Up'}[name]], env=self.run.env(), check=True)
        self._qmp([{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': name}}}]); time.sleep(.06)
        self._qmp([{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': name}}}])

def notify(args):
    """Wait for the scheduled check, crop the banner, click it, check the recorder."""
    run = Run(args.name); frames = Frames(); inp = Input(run); inp.size = (frames.c.width, frames.c.height)
    s = run.cli('update.status')['result']; due = datetime.fromisoformat(s['nextCheckAt'].replace('Z', '+00:00'))
    report = {'nextCheckAt': s['nextCheckAt'], 'start': run.meta['starts'][-1]}
    wait = (due - now()).total_seconds() - 4
    if wait > args.max_wait: raise SystemExit(f'check is {wait:.0f} s away')
    if wait > 0: time.sleep(wait)
    inp.move(*PARK); time.sleep(1.5); base = frames.grab()
    seen = len([e for e in run.events() if 'Notification shown' in json.dumps(e)])
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        shown = [e for e in run.events() if 'Notification shown' in json.dumps(e)]
        if len(shown) > seen: break
        time.sleep(.25)
    else:
        report['notification'] = None; report['events'] = run.events(); out(report); return
    report['shownAt'] = iso(now()); time.sleep(args.settle)
    frame = frames.grab()
    report['banner'] = crop(frame, base, f'{args.label}-notification', run)
    comps = report['banner']['components']
    if args.click and comps:
        x0, y0, x1, y1 = (report['banner'].get('inner') or comps[0]['box'])
        target = ((x0 + x1) // 2, y0 + (y1 - y0) // 3)
        inp.click(*target); report['clicked'] = target; time.sleep(3)
        inp.move(*PARK)
    report['events'] = run.events()
    report['openedUrls'] = (run.dir / 'opened-urls.txt').read_text().splitlines() if (run.dir / 'opened-urls.txt').exists() else []
    out(report)

def panel(args):
    """Find our tray icon: panel-row changes between a frame without and with it."""
    run = Run(args.name); frames = Frames(); inp = Input(run); inp.size = (frames.c.width, frames.c.height)
    path = run.dir / 'frames/panel-before.raw'
    if args.before:
        inp.move(*PARK); time.sleep(1); f = frames.grab(); path.write_bytes(json.dumps(f[:2]).encode() + b'\n' + f[2]); out({'saved': str(path)}); return
    head, px = path.read_bytes().split(b'\n', 1); w, h = json.loads(head); base = (w, h, px)
    inp.move(*PARK); time.sleep(1); f = frames.grab()
    comps = [c for c in components(base, f) if c['box'][1] < 64 or c['box'][3] > h - 72]
    out({'size': [w, h], 'panelChanges': comps})

def tray(args):
    """Open the tray menu with a real click, crop it, then pick the Update item by keyboard."""
    run = Run(args.name); frames = Frames(); inp = Input(run); inp.size = (frames.c.width, frames.c.height)
    report = {}
    inp.move(*PARK); time.sleep(1); base = frames.grab()
    inp.click(args.x, args.y, args.button); time.sleep(1.2); inp.move(*PARK); time.sleep(.6)
    frame = frames.grab()
    report['menu'] = crop(frame, base, f'{args.label}-tray-menu', run, pick=lambda c: c['blocks'] > 40)
    if args.select is not None:
        for _ in range(args.select): inp.key('down'); time.sleep(.2)
        time.sleep(.4); frame2 = frames.grab()
        report['menuSelected'] = crop(frame2, base, f'{args.label}-tray-menu-selected', run, pick=lambda c: c['blocks'] > 40)
        inp.key('ret'); time.sleep(args.window_wait)
        frame3 = frames.grab()
        report['window'] = crop(frame3, base, f'{args.label}-settings', run, pick=lambda c: c['blocks'] > 400, theme=True)
    else:
        inp.key('esc')
    report['ui'] = run.cli('diagnostics').get('result', {}).get('ui')
    out(report)

def shot(args):
    """Crop whatever changed since a saved baseline (e.g. a window opened by the rail)."""
    run = Run(args.name); frames = Frames(); inp = Input(run); inp.size = (frames.c.width, frames.c.height)
    path = run.dir / f'frames/{args.label}-base.raw'
    inp.move(*PARK); time.sleep(1); f = frames.grab()
    if args.baseline:
        path.write_bytes(json.dumps(f[:2]).encode() + b'\n' + f[2]); out({'saved': str(path)}); return
    head, px = path.read_bytes().split(b'\n', 1); w, h = json.loads(head)
    out(crop(f, (w, h, px), args.label, run, pick=lambda c: c['blocks'] > args.min_blocks, theme=args.theme))

def click(args):
    run = Run(args.name); inp = Input(run)
    import rfb; c = rfb.RFB(VNC[os.uname().nodename]); inp.size = (c.width, c.height); c.close()
    if args.key: inp.key(args.key)
    else: inp.click(args.x, args.y, args.button)
    time.sleep(.3); inp.move(*PARK); out({'ok': True})

def cleanup(args):
    run = Run(args.name)
    stop(args)
    subprocess.run(['rm', '-rf', run.meta['runtime']])
    frames = list((run.dir / 'frames').glob('*'))
    for f in frames: f.unlink()
    out({'runtimeRemoved': run.meta['runtime'], 'framesRemoved': len(frames)})

if __name__ == '__main__':
    p = argparse.ArgumentParser(); sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('setup'); a.add_argument('name'); a.add_argument('--desktop', required=True); a.add_argument('--bin', required=True); a.add_argument('--feed'); a.add_argument('--notifications', type=int, default=1)
    for n in ('start', 'status', 'stop', 'menu', 'cleanup'): sub.add_parser(n).add_argument('name')
    a = sub.add_parser('age'); a.add_argument('name'); a.add_argument('--hours', type=float, default=25)
    a = sub.add_parser('notify'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--settle', type=float, default=1.5); a.add_argument('--click', type=int, default=1); a.add_argument('--max-wait', type=float, default=1000)
    a = sub.add_parser('panel'); a.add_argument('name'); a.add_argument('--before', action='store_true')
    a = sub.add_parser('tray'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('x', type=int); a.add_argument('y', type=int); a.add_argument('--button', default='right'); a.add_argument('--select', type=int); a.add_argument('--window-wait', type=float, default=4)
    a = sub.add_parser('shot'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--baseline', action='store_true'); a.add_argument('--min-blocks', type=int, default=400); a.add_argument('--theme', action='store_true')
    a = sub.add_parser('click'); a.add_argument('name'); a.add_argument('--x', type=int); a.add_argument('--y', type=int); a.add_argument('--button', default='left'); a.add_argument('--key')
    args = p.parse_args(); globals()[args.cmd](args)
