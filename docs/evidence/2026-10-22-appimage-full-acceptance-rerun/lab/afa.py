#!/usr/bin/env python3
"""AppImage full-acceptance driver (A-K), run on the desktop's host (or inside a VM guest) from SSH.

Stdlib only, plus the lab's own tools next to it (rfb.py, cover_player.py).
Every run lives under $AFA_ROOT/runs/NAME (default ~/work/put.gd-afa). put.gd
gets private XDG_CONFIG/STATE/CACHE_HOME and XDG_RUNTIME_DIR there. The data
home is the user's own (~/.local/share): AppRun registers KWin's capture and
screencast permissions and the Wayland window-icon entries there, as for any
user. `setup` refuses to run when ~/.local/share/putgd already exists, and
`cleanup` removes only what this run created.

Frames come from a loopback VNC (the host's wall server, or a guest's QEMU VNC
tunnelled into the guest) and stay on the machine. Only crops whose edges are
the crop's own background colour, and that are mostly changed pixels, are
written as PNG. Screen captures put.gd makes are checked for the cover colour
before they are kept (cover_player.py check-frame deletes a failing file).

Subcommands print one JSON document. See `afa.py -h`.
"""
import argparse, base64, collections, hashlib, json, os, re, shutil, signal, socket, struct, subprocess, sys, time, zlib
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
ROOT = Path(os.environ.get('AFA_ROOT', str(Path.home() / 'work/put.gd-afa2')))
SHELLS = ('plasmashell', 'gnome-shell', 'cinnamon')
SESSION_VARIABLES = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP',
                     'WAYLAND_DISPLAY', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION',
                     'XDG_DATA_DIRS', 'LANG')
ACCESS, SECRET = 'PUTGD_SYNTHETIC_ACCESS', 'putgd-synthetic-secret-not-a-real-key'  # tests/fixtures/s3_server.py's own pair
MAGENTA = (255, 0, 255)
DATA_HOME = Path.home() / '.local/share'
# Theme.cpp palettes (dark and light) plus FrameView's surround, from r283lab.py.
DARK = ['111317', '181B21', '20242C', '232838', '2A2F38', '343A45', 'E7E9EE', 'C4C9D2', '9AA1AD', '6F7682', '6EA8FE',
        '8DBBFF', '4CC38A', 'E5A443', 'F07A6A', 'F4A497', '1C3328', '3A3020', '3A2326', '9BBCFF', '515F82']
LIGHT = ['F5F6F8', 'FFFFFF', 'EDEFF3', 'DCE7FA', 'D5D9E0', 'AEB5C1', '1B1E24', '3A414D', '585F6B', '8B929D', '1F64D6',
         '1753B5', '1754BD', '146C42', '8A5300', 'B4312A', '96271F', 'E3F4EA', 'FBF0DC', 'FBE7E5', '2759B8', 'B4C8EE']
OWN = [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in DARK + LIGHT + ['0D0F13']]


def now(): return datetime.now(timezone.utc)
def iso(t=None): return (t or now()).strftime('%Y-%m-%dT%H:%M:%SZ')
def out(obj): print(json.dumps(obj, indent=1, default=str), flush=True)
def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()
def fetch(url, timeout=20):
    import urllib.request
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r: return r.read().decode(errors='replace')
    except Exception as e: return 'error: ' + repr(e)[:200]
def redirect(url, timeout=20):
    import urllib.request
    try:
        with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=timeout) as r: return f'{r.status} {r.url}'
    except Exception as e: return 'error: ' + repr(e)[:200]
def sh(*cmd, env=None, timeout=60):
    r = subprocess.run(list(cmd), env=env, capture_output=True, text=True, timeout=timeout)
    return (r.stdout or '').strip()


def session_environment():
    uid = os.getuid()
    for process in SHELLS:
        pids = subprocess.run(['pgrep', '-u', str(uid), '-x', process], capture_output=True, text=True).stdout.split()
        if pids: break
    else:
        raise SystemExit('no desktop session process (plasmashell, gnome-shell, cinnamon)')
    found = dict(p.split('=', 1) for p in Path(f'/proc/{pids[0]}/environ').read_bytes().decode(errors='replace').split('\0') if '=' in p)
    runtime = f'/run/user/{uid}'
    env = {'HOME': os.environ['HOME'], 'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'USER': os.environ.get('USER', ''),
           'LANG': found.get('LANG', 'C.UTF-8')}
    env.update({k: found[k] for k in SESSION_VARIABLES if found.get(k)})
    if process == 'gnome-shell' or (env.get('XDG_SESSION_TYPE') == 'wayland' and not env.get('WAYLAND_DISPLAY')):
        shown = subprocess.run(['systemctl', '--user', 'show-environment'], capture_output=True, text=True).stdout
        exported = dict(l.split('=', 1) for l in shown.splitlines() if '=' in l)
        for k in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY'):
            if k in exported: env.setdefault(k, exported[k])
    if env.get('WAYLAND_DISPLAY') and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = runtime + '/' + env['WAYLAND_DISPLAY']
    return env, process, int(pids[0])


class Run:
    def __init__(self, name):
        self.name = name
        self.dir = ROOT / 'runs' / name
        self.meta_path = self.dir / 'run.json'
        self.meta = json.loads(self.meta_path.read_text()) if self.meta_path.exists() else {}

    def save(self): self.meta_path.write_text(json.dumps(self.meta, indent=1) + '\n')

    def env(self, private=True):
        env = dict(self.meta['session'])
        env['PIPEWIRE_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        if private:
            for key, sub in (('XDG_CONFIG_HOME', 'config'), ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache')):
                env[key] = str(self.dir / sub)
            env['XDG_RUNTIME_DIR'] = self.meta['runtime']
        else:
            env['XDG_RUNTIME_DIR'] = f'/run/user/{os.getuid()}'
        return env

    @property
    def appdir(self): return Path(self.meta['appdir'])

    def cli(self, method, params=None, timeout=70, private=True):
        r = subprocess.run([str(self.appdir / 'usr/bin/putgd-cli'), method, json.dumps(params or {})], env=self.cli_env(private),
                           timeout=timeout, capture_output=True, text=True)
        try: return json.loads(r.stdout)
        except ValueError: return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}

    def cli_env(self, private=True):
        env = self.env(private)
        lib = self.appdir / 'usr/lib'
        env['LD_LIBRARY_PATH'] = f'{lib}:{lib}/putgd'
        env['QT_PLUGIN_PATH'] = f'{self.appdir}/usr/plugins:{lib}/qt6/plugins'
        return env

    def ok(self, method, params=None, timeout=70, private=True):
        r = self.cli(method, params, timeout, private)
        if not r.get('ok'): raise SystemExit(f'{method}: {json.dumps(r)[:600]}')
        return r['result']

    def socket_request(self, method, params):
        path = str(Path(self.meta['runtime']) / 'putgd/session.sock')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(20); s.connect(path)
            s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'afa-' + method, 'session': path, 'method': method, 'params': params}).encode() + b'\n')
            data = b''
            while not data.endswith(b'\n'):
                chunk = s.recv(65536)
                if not chunk: break
                data += chunk
        return json.loads(data)

    def logs(self, needle=None):
        lines = []
        for path in sorted((self.dir / 'state').rglob('putgd-*.jsonl')):
            for line in path.read_text(errors='replace').splitlines():
                if needle and needle not in line: continue
                try: lines.append(json.loads(line))
                except ValueError: pass
        return lines

    def wait(self, check, timeout, step=.3):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = check()
            if value: return value
            time.sleep(step)
        return None

    def fixture(self, path, body=None):
        import urllib.request
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(f"http://127.0.0.1:{self.meta['port']}{path}", data=data, method='GET' if body is None else 'POST')
        with urllib.request.urlopen(req, timeout=15) as reply: return json.loads(reply.read() or b'null')

    def record(self, key, value):
        results = self.dir / 'results.json'
        doc = json.loads(results.read_text()) if results.exists() else {}
        doc[key] = value
        results.write_text(json.dumps(doc, indent=1, default=str) + '\n')
        return value


# ---------------------------------------------------------------- frames, crops and input

class Frames:
    def __init__(self, port):
        import rfb
        self.c = rfb.RFB(port, host=os.environ.get('AFA_VNC_HOST', '127.0.0.1'))
    def grab(self):
        return self.c.frame()

def pixel(f, x, y):
    w, h, px = f; i = (y * w + x) * 4; return px[i + 2], px[i + 1], px[i]
def differs(a, b, tol=18): return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2])) > tol

def components(base, frame, block=8, area=None, ignore=None):
    w, h, _ = frame; changed = set()
    x0, y0, x1, y1 = area or (0, 0, w, h)
    for by in range(y0 // block, y1 // block):
        for bx in range(x0 // block, x1 // block):
            x, y = bx * block, by * block
            if ignore and ignore[0] - 8 <= x < ignore[2] + 8 and ignore[1] - 8 <= y < ignore[3] + 8: continue
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

def edge_ok(frame, box, colours, tol, share):
    x0, y0, x1, y1 = box
    edges = [[(x, y0) for x in range(x0, x1)], [(x, y1 - 1) for x in range(x0, x1)], [(x0, y) for y in range(y0, y1)], [(x1 - 1, y) for y in range(y0, y1)]]
    return [sum(any(not differs(pixel(frame, x, y), c, tol) for c in colours) for x, y in e) / len(e) >= share for e in edges]

def shrink(frame, box, colours, tol=10, share=.9):
    x0, y0, x1, y1 = box
    for _ in range(600):
        ok = edge_ok(frame, (x0, y0, x1, y1), colours, tol, share)
        if all(ok): return [x0, y0, x1, y1]
        if not ok[0]: y0 += 1
        if not ok[1]: y1 -= 1
        if not ok[2]: x0 += 1
        if not ok[3]: x1 -= 1
        if x1 - x0 < 30 or y1 - y0 < 16: return None
    return None

def write_png(frame, box, path):
    x0, y0, x1, y1 = box; raw = bytearray()
    for y in range(y0, y1):
        raw.append(0)
        for x in range(x0, x1): raw += bytes(pixel(frame, x, y))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', x1 - x0, y1 - y0, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))

def own_share(frame, box, extra=()):
    x0, y0, x1, y1 = box; total = hit = 0
    palette = OWN + list(extra)
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            total += 1; p = pixel(frame, x, y); hit += any(not differs(p, t, 6) for t in palette)
    return hit / max(total, 1)

def crop(run, frame, base, label, pick=None, theme=False, extra_edge=(), min_changed=.5, area=None, own_min=.4, ignore=None):
    """Keep the largest changed region if its edges are its own colour and its pixels mostly changed (or put.gd's)."""
    comps = components(base, frame, area=area, ignore=ignore)
    report = {'label': label, 'components': comps[:6]}
    cands = comps if pick is None else [c for c in comps if pick(c)]
    if not cands: report['kept'] = None; return report
    box = cands[0]['box']
    colour = dominant(frame, box)
    colours = [colour, *extra_edge]
    inner = shrink(frame, [max(box[0] + 2, 0), max(box[1] + 2, 0), box[2] - 2, box[3] - 2], colours)
    report.update(colour=colour, inner=inner)
    if inner:
        xs = range(inner[0], inner[2], 3); ys = range(inner[1], inner[3], 3)
        report['changedShare'] = round(sum(differs(pixel(base, x, y), pixel(frame, x, y), 0) for y in ys for x in xs) / (len(xs) * len(ys)), 3)
        if theme: report['ownShare'] = round(own_share(frame, inner), 3)
        area_share = (inner[2] - inner[0]) * (inner[3] - inner[1]) / (frame[0] * frame[1])
        if area_share > .5 and not theme:
            # A crop this large must be the lab's own screen: the cover's hue (also dimmed) or the pattern.
            hue = lambda p: p[0] > 40 and p[2] > 40 and abs(p[0] - p[2]) < 30 and p[1] < p[0] * .35
            pts = [(x, y) for y in range(inner[1], inner[3], 6) for x in range(inner[0], inner[2], 6)]
            inside = lambda x, y: ignore and ignore[0] <= x < ignore[2] and ignore[1] <= y < ignore[3]
            report['coverHueShare'] = round(sum(hue(pixel(frame, x, y)) or inside(x, y) for x, y in pts) / len(pts), 3)
            if report['coverHueShare'] < .6: report['kept'] = None; return report
        if all(edge_ok(frame, inner, colours, 10, .9)) and report['changedShare'] >= min_changed and (not theme or report['ownShare'] >= own_min):
            path = run.dir / 'crops' / f'{label}.png'; write_png(frame, inner, path); report['kept'] = str(path)
        else:
            report['kept'] = None
    return report

def fullshot(run, frame, label):
    """A full-desktop frame, written only when this run allows it (VMs; physical desktops after Patrick's sanitising OK)."""
    if not run.meta.get('fullAllowed'): return None
    path = run.dir / 'full' / f'{label}.png'; path.parent.mkdir(exist_ok=True)
    write_png(frame, [0, 0, frame[0], frame[1]], path); return str(path)

def allow_full(args):
    run = Run(args.name); run.meta['fullAllowed'] = args.state == 'on'; run.save(); out({'fullAllowed': run.meta['fullAllowed']})

def magenta(size):
    """The plain cover as a frame: what the screen shows with nothing of put.gd on it."""
    return (size[0], size[1], bytes((255, 0, 255, 0)) * (size[0] * size[1]))

def save_frame(run, frame, name):
    p = run.dir / 'frames' / f'{name}.raw'; p.write_bytes(json.dumps(frame[:2]).encode() + b'\n' + frame[2]); return p
def load_frame(run, name):
    head, px = (run.dir / 'frames' / f'{name}.raw').read_bytes().split(b'\n', 1); w, h = json.loads(head); return (w, h, px)


class Input:
    """xdotool on X11, the uinput agent on physical Wayland, the VNC tunnel in a VM."""
    def __init__(self, run, size):
        self.run = run; self.mode = run.meta['input']; self.size = size; self.vnc = None
        if self.mode == 'vnc':
            import rfb; self.vnc = rfb.RFB(run.meta['vnc'], host=os.environ.get('AFA_VNC_HOST', '127.0.0.1'))
    def _qmp(self, events):
        s = socket.socket(socket.AF_UNIX); s.connect(f'/run/user/{os.getuid()}/putgd-lab-input.sock'); f = s.makefile('rwb')
        f.readline(); f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
        f.write(json.dumps({'execute': 'input-send-event', 'arguments': {'events': events}}).encode() + b'\n'); f.flush()
        reply = json.loads(f.readline()); s.close()
        if 'error' in reply: raise RuntimeError(reply)
    def _xdo(self, *a): subprocess.run(['xdotool', *a], env=self.run.env(), check=True)
    def move(self, x, y):
        if self.mode == 'xdotool': return self._xdo('mousemove', str(x), str(y))
        if self.mode == 'vnc': return self.vnc.pointer(x, y)
        self._qmp([{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (self.size[0] - 1))}},
                   {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (self.size[1] - 1))}}])
    def button(self, down, button='left'):
        if self.mode == 'xdotool': return self._xdo('mousedown' if down else 'mouseup', {'left': '1', 'right': '3'}[button])
        if self.mode == 'vnc':
            self.vnc.buttons = (1 if button == 'left' else 4) if down else 0
            return self.vnc.sock.sendall(struct.pack('>BBHH', 5, self.vnc.buttons, *self.vnc_pos))
        self._qmp([{'type': 'btn', 'data': {'down': down, 'button': button}}])
    def click(self, x, y, button='left'):
        self.move(x, y); self.vnc_pos = (x, y); time.sleep(.2)
        self.button(True, button); time.sleep(.12); self.button(False, button)
    def drag(self, x0, y0, x1, y1, steps=14):
        self.move(x0, y0); self.vnc_pos = (x0, y0); time.sleep(.3); self.button(True); time.sleep(.2)
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i // steps; y = y0 + (y1 - y0) * i // steps
            self.move(x, y); self.vnc_pos = (x, y); time.sleep(.04)
        time.sleep(.25); self.button(False)
    QCODE = {'Down': 'down', 'Up': 'up', 'Return': 'ret', 'Escape': 'esc', 'Tab': 'tab', 'alt': 'alt', 'ctrl': 'ctrl', 'shift': 'shift',
             'super': 'meta_l', 'BackSpace': 'backspace'}
    XKEY = {'alt': 'alt', 'ctrl': 'ctrl', 'shift': 'shift', 'super': 'super'}
    VKEY = {'Down': 'down', 'Up': 'up', 'Return': 'ret', 'Escape': 'esc', 'Tab': 'tab', 'BackSpace': 'backspace'}
    def key(self, *names, hold=.06):
        """A chord such as key('alt', 'Tab'); single characters are letters/digits."""
        if self.mode == 'xdotool': return self._xdo('key', '+'.join(self.XKEY.get(n, n) for n in names))
        if self.mode == 'vnc': return self.vnc.key(*[self.VKEY.get(n, n) for n in names], hold=hold)
        codes = [self.QCODE.get(n, n.lower()) for n in names]
        self._qmp([{'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': c}}} for c in codes]); time.sleep(hold)
        self._qmp([{'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': c}}} for c in reversed(codes)])
    def hold(self, name, down):
        if self.mode == 'xdotool': return self._xdo('keydown' if down else 'keyup', self.XKEY.get(name, name))
        if self.mode == 'vnc':
            import rfb; return self.vnc.sock.sendall(struct.pack('>BBxxI', 4, 1 if down else 0, rfb.KEYSYMS[name]))
        self._qmp([{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': self.QCODE.get(name, name)}}}])
    def type(self, text):
        if self.mode == 'xdotool': return self._xdo('type', '--delay', '40', text)
        if self.mode == 'vnc': return self.vnc.type(text)
        for ch in text:
            name = {'-': 'minus', '.': 'dot', '/': 'slash', '_': None, ' ': 'spc'}.get(ch, ch.lower())
            if name is None: self.key('shift', 'minus'); continue
            self.key(*(['shift', name] if ch.isupper() else [name])); time.sleep(.03)

def tools(run):
    frames = Frames(run.meta['vnc']); w, h, _ = frames.grab()
    return frames, Input(run, (w, h)), (w, h)

def park(run, inp, size):
    inp.move(size[0] - 8, size[1] // 2); time.sleep(.6)


# ---------------------------------------------------------------- PNG (stdlib) for pixel checks

def png_read(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8]); body = data[pos + 8:pos + 8 + n]; pos += 12 + n
        if kind == b'IHDR': w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
        elif kind == b'IDAT': idat += body
    if depth != 8 or ctype not in (2, 6): raise ValueError(f'unsupported PNG {depth}/{ctype}')
    bpp = 3 if ctype == 2 else 4; raw = zlib.decompress(idat); stride = w * bpp; rows = []; prev = bytearray(stride); i = 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return w, h, bpp, rows

def png_rgb(img, x, y):
    w, h, bpp, rows = img; o = x * bpp; r = rows[y]; return r[o], r[o + 1], r[o + 2]

def region_diff(a, b, rect):
    """Share of sampled pixels in rect that differ between two same-size images."""
    x, y, w, h = rect; pts = [(xx, yy) for yy in range(y, y + h, 3) for xx in range(x, x + w, 3)]
    return round(sum(png_rgb(a, *p) != png_rgb(b, *p) for p in pts) / len(pts), 3)

def outside_same(a, b, rects):
    w, h = a[:2]; pts = [(x, y) for y in range(0, h, 7) for x in range(0, w, 7)
                         if not any(r[0] - 4 <= x < r[0] + r[2] + 4 and r[1] - 4 <= y < r[1] + r[3] + 4 for r in rects)]
    return round(sum(png_rgb(a, *p) == png_rgb(b, *p) for p in pts) / len(pts), 4)

def region_stats(img, rect):
    x, y, w, h = rect; vals = [png_rgb(img, xx, yy) for yy in range(y, y + h, 4) for xx in range(x, x + w, 4)]
    mean = tuple(sum(v[i] for v in vals) / len(vals) for i in range(3))
    spread = max(max(v[i] for v in vals) - min(v[i] for v in vals) for i in range(3))
    return {'mean': [round(m, 1) for m in mean], 'spread': spread}


# ---------------------------------------------------------------- A. identity

def identity(args):
    image = Path(args.appimage).resolve()
    sums = dict(reversed(l.split(None, 1)) for l in Path(args.sums).read_text().splitlines() if l.strip())
    sums = {k.lstrip('*').strip(): v for k, v in sums.items()}
    digest = sha256(image)
    report = {'file': image.name, 'bytes': image.stat().st_size, 'sha256': digest, 'sumsEntry': sums.get(image.name), 'hashMatches': sums.get(image.name) == digest}
    env = dict(os.environ); env.pop('DISPLAY', None); env.pop('WAYLAND_DISPLAY', None)
    vj = subprocess.run([str(image), '--version-json'], capture_output=True, text=True, env=env, timeout=60)
    report['versionJson'] = json.loads(vj.stdout) if vj.returncode == 0 else {'exit': vj.returncode, 'stderr': vj.stderr[-300:]}
    tag = args.tag
    report['tagMatches'] = isinstance(report['versionJson'], dict) and ('v' + str(report['versionJson'].get('version'))) == tag
    report['updateInformation'] = sh(str(image), '--appimage-updateinformation', env=env)
    report['updateInformationMatches'] = report['updateInformation'] == 'zsync|https://app.put.gd/dl/nightly/appimage.zsync'
    # Mount once (no extraction) and list the image root.
    mount = subprocess.Popen([str(image), '--appimage-mount'], stdout=subprocess.PIPE, text=True, env=env, start_new_session=True)
    try:
        point = Path(mount.stdout.readline().strip())
        entries = sorted(p.name for p in point.iterdir() if p.name.endswith('.desktop'))
        report['rootDesktopFiles'] = entries
        main = point / 'putgd.desktop'
        text = main.read_text() if main.exists() else ''
        report['putgdDesktop'] = text
        report['noDisplay'] = bool(re.search(r'(?m)^NoDisplay\s*=\s*true', text))
        report['rootOk'] = entries == ['putgd.desktop'] and not report['noDisplay']
    finally:
        os.killpg(mount.pid, signal.SIGTERM); mount.wait(10)
    out(report)
    if args.save: Path(args.save).write_text(json.dumps(report, indent=1) + '\n')


def versions(args):
    """OS, desktop, compositor, session type of the graphical login session, GPU."""
    env, shell, shell_pid = session_environment()
    osr = dict(l.split('=', 1) for l in Path('/etc/os-release').read_text().splitlines() if '=' in l)
    report = {'os': osr.get('PRETTY_NAME', '').strip('"'), 'osVersionId': osr.get('VERSION_ID', '').strip('"'), 'kernel': os.uname().release,
              'host': os.uname().nodename, 'sessionProcess': shell}
    sessions = []
    for line in sh('loginctl', 'list-sessions', '--no-legend').splitlines():
        sid = line.split()[0]
        props = dict(l.split('=', 1) for l in sh('loginctl', 'show-session', sid, '-p', 'Type', '-p', 'Desktop', '-p', 'Seat', '-p', 'Class', '-p', 'State', '-p', 'Service', '-p', 'Leader').splitlines() if '=' in l)
        if props.get('Class') == 'user' and props.get('Type') in ('wayland', 'x11'): sessions.append(dict(props, Id=sid))
    report['graphicalSessions'] = sessions
    # The shell's own login session: its /proc/<pid>/sessionid is the audit session, so use the cgroup.
    cg = Path(f'/proc/{shell_pid}/cgroup').read_text()
    m = re.search(r'session-(\w+)\.scope', cg)
    report['shellSession'] = m.group(1) if m else cg.strip()[-120:]
    report['envSessionType'] = env.get('XDG_SESSION_TYPE'); report['currentDesktop'] = env.get('XDG_CURRENT_DESKTOP')
    for cmd in (['plasmashell', '--version'], ['kwin_wayland', '--version'], ['kwin_x11', '--version'], ['gnome-shell', '--version'],
                ['cinnamon', '--version'], ['muffin', '--version'], ['Xorg', '-version']):
        if shutil.which(cmd[0]):
            text = subprocess.run(cmd, capture_output=True, text=True, timeout=20, env=env)
            report[cmd[0]] = ((text.stdout or '') + (text.stderr or '')).strip().splitlines()[:2] if cmd[0] == 'Xorg' else (text.stdout or text.stderr).strip().splitlines()[:1]
    for pkg in ('mutter', 'gnome-shell', 'kwin-wayland', 'kwin', 'kwin-common', 'plasma-workspace', 'muffin', 'cinnamon', 'xserver-xorg-core', 'xorg-x11-server-Xorg',
                'xwayland', 'xorg-x11-server-Xwayland', 'qt6-qtbase', 'libqt6core6t64', 'libqt6core6', 'xdg-desktop-portal', 'xdg-desktop-portal-gnome',
                'xdg-desktop-portal-kde', 'xdg-desktop-portal-gtk', 'xdg-desktop-portal-xapp', 'pipewire', 'libpipewire-0.3-0t64', 'libpipewire-0.3-0', 'fuse3', 'libfuse2t64', 'libfuse2'):
        v = sh('rpm', '-q', '--qf', '%{VERSION}-%{RELEASE}', pkg) if shutil.which('rpm') else ''
        if v and 'not installed' not in v: report.setdefault('packages', {})[pkg] = v
        if shutil.which('dpkg-query'):
            v = sh('dpkg-query', '-W', '-f', '${db:Status-Abbrev}${Version}', pkg)
            if v.startswith('ii'): report.setdefault('packages', {})[pkg] = v[3:].strip()
    if shutil.which('dpkg-query'):
        for line in sh('dpkg-query', '-W', '-f', '${db:Status-Abbrev} ${Package} ${Version}\n', 'libmutter-*').splitlines():
            if line.startswith('ii'): report.setdefault('packages', {})[line.split()[1]] = line.split()[2]
    report['gpu'] = [l for l in sh('lspci').splitlines() if re.search('VGA|Display|3D', l)]
    report['fuse'] = {k: bool(shutil.which(k)) for k in ('fusermount', 'fusermount3')}
    report['libfuse2'] = bool(re.search(r'libfuse\.so\.2', sh('sh', '-c', 'ldconfig -p 2>/dev/null || /sbin/ldconfig -p')))
    out(report)
    if args.save: Path(args.save).write_text(json.dumps(report, indent=1) + '\n')


# ---------------------------------------------------------------- setup / start

def config_document(run, template):
    port = run.meta['port']
    doc = dict(template)
    doc.update({
        'schemaVersion': 1,
        'actions': {n: {'destination': 'upload', 'profileId': 'afa-fixture', 'shortcut': ''} for n in ('clipboard', 'desktop', 'edit', 'region', 'window')} | {'record': {'shortcut': ''}},
        'archive': {'folders': {'images': None, 'other': None, 'text': None, 'videos': None}, 'keepUploads': True, 'root': str(run.dir / 'archive')},
        'profiles': [{'credentials': {'mode': 'session', 'reference': None}, 'id': 'afa-fixture', 'label': 'Loopback fixture', 'provider': 's3',
                      'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'fixture', 'connectTimeoutMs': 5000,
                                   'endpoint': f'http://127.0.0.1:{port}', 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120,
                                   'objectPrefix': 'afa/', 'publicUrlBase': f'http://127.0.0.1:{port}/fixture', 'region': 'us-east-1',
                                   'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}}],
    })
    prefs = dict(template.get('preferences') or {})
    prefs.update({'launchAtLogin': False, 'notifications': False, 'uploadClipboardBehavior': 'each_link', 'unlockCredentialsAtStartup': False,
                  'appearance': {'theme': 'system'}, 'updates': {'check': True, 'channel': 'build'}})
    doc['preferences'] = prefs
    return doc


def setup(args):
    run = Run(args.name)
    if run.meta: raise SystemExit('run exists')
    if (DATA_HOME / 'putgd').exists() and not args.allow_data_home: raise SystemExit(f'{DATA_HOME / "putgd"} exists; refusing')
    run.dir.mkdir(parents=True, mode=0o700)
    for sub in ('config', 'state', 'cache', 'archive', 'frames', 'crops', 'out', 'logs', 'working'):
        (run.dir / sub).mkdir(mode=0o700)
    session, shell, _ = session_environment()
    image = Path(args.appimage).resolve()
    appdir = run.dir / 'appdir'
    appdir.mkdir(mode=0o700)
    subprocess.run([str(image), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
    runtime = subprocess.check_output(['mktemp', '-d', '/tmp/afa.XXXXXX'], text=True).strip()
    # Everything that exists before this run, so cleanup removes only what the run added.
    home_paths = [Path.home() / p for p in ('.config/putgd', '.cache/putgd', '.local/state/putgd', '.local/share/putgd')]
    before = {'homePaths': {str(p): sorted(str(f) for f in p.rglob('*')) if p.exists() else None for p in home_paths},
              'applications': sorted(p.name for p in (DATA_HOME / 'applications').glob('*')) if (DATA_HOME / 'applications').exists() else [],
              'dataPutgd': (DATA_HOME / 'putgd').exists(), 'autostart': sorted(p.name for p in (Path.home() / '.config/autostart').glob('*')) if (Path.home() / '.config/autostart').exists() else []}
    run.meta = {'name': args.name, 'appimage': str(image), 'appdir': str(appdir / 'squashfs-root'), 'runtime': runtime, 'session': session,
                'shell': shell, 'host': os.uname().nodename, 'vnc': args.vnc_port, 'input': args.input, 'before': before, 'pids': {},
                'created': iso()}
    run.save()
    # URL recorder: links from notifications land here instead of a browser.
    recorder = run.dir / 'record-url.sh'
    recorder.write_text(f'#!/bin/sh\nprintf "%s %s\\n" "$(date -u +%FT%TZ)" "$1" >> {run.dir}/opened-urls.txt\n'); recorder.chmod(0o700)
    apps = run.dir / 'urldata/applications'; apps.mkdir(parents=True)
    (apps / 'afa-url.desktop').write_text(f'[Desktop Entry]\nType=Application\nName=put.gd acceptance URL recorder\nExec={recorder} %u\nMimeType=x-scheme-handler/https;x-scheme-handler/http;\nNoDisplay=true\n')
    mime = '[Default Applications]\nx-scheme-handler/https=afa-url.desktop\nx-scheme-handler/http=afa-url.desktop\n'
    (run.dir / 'config/mimeapps.list').write_text(mime)
    (run.dir / 'config/kdeglobals').write_text('[General]\nBrowserApplication=afa-url.desktop\n')
    run.meta['session']['XDG_DATA_DIRS'] = f"{run.dir / 'urldata'}:" + run.meta['session'].get('XDG_DATA_DIRS', '/usr/local/share:/usr/share')
    # Loopback S3 fixture (synthetic keys only).
    fixture = subprocess.Popen([sys.executable, '-I', str(HERE / 's3_server.py')], stdout=subprocess.PIPE, stderr=open(run.dir / 'logs/fixture.log', 'w'),
                               text=True, start_new_session=True)
    run.meta['port'] = int(fixture.stdout.readline()); run.meta['pids']['fixture'] = fixture.pid
    run.save()
    # Write the profile with a resident in the real session (on screen, like every launch here), then stop it.
    resident = launch(run, 'setup')
    template = run.ok('settings.developmentTemplate')['document']
    (run.dir / 'config/putgd').mkdir(exist_ok=True)
    (run.dir / 'config/putgd/config.json').write_text(json.dumps(config_document(run, template), indent=1))
    run.ok('settings.reload')
    snap = run.wait(lambda: (lambda s: s if s.get('ok') and s['result']['metadata']['validation']['ok'] else None)(run.cli('settings.get')), 20)
    quit_resident(run, resident)
    out({'setup': bool(snap), 'validation': (snap or {}).get('result', {}).get('metadata', {}).get('validation'), 'run': run.meta})


def launch(run, label):
    """The AppImage as a user starts it from a terminal: no QT_QPA_PLATFORM, the session's own environment."""
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    log = open(run.dir / f'logs/launch-{label}.log', 'ab')
    p = subprocess.Popen([run.meta['appimage'], '--background'], env=run.env(), stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
    run.meta['pids']['apprun'] = p.pid; run.save()
    ready = run.wait(lambda: sock.exists() and (lambda d: d.get('ok') and d['result'].get('journalReady'))(run.cli('diagnostics', timeout=15)), 60)
    if not ready: raise SystemExit(f'resident not ready ({label}); see logs/launch-{label}.log')
    return p


def quit_resident(run, process=None, how='ui.quit'):
    r = run.cli('ui.quit', timeout=15) if how == 'ui.quit' else None
    pid = run.meta['pids'].get('apprun')
    gone = run.wait(lambda: not Path(f'/proc/{pid}').exists(), 40) if pid else True
    if process is not None:
        try: process.wait(5)
        except subprocess.TimeoutExpired: pass
    return {'quit': r, 'apprunGone': bool(gone)}


def tree(run):
    """put.gd processes of this run: the AppRun shell, the resident, putgd-ui and helpers, with their Qt platform."""
    procs = []
    mount_prefix = None
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            exe = os.readlink(p / 'exe'); env = (p / 'environ').read_bytes()
        except OSError: continue
        if run.meta['runtime'].encode() not in env and run.meta['appimage'].encode() not in env: continue
        cmd = (p / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace').strip()
        if 'afa.py' in cmd: continue
        maps = (p / 'maps').read_text(errors='replace') if (p / 'maps').exists() else ''
        platforms = sorted({Path(l.split()[-1]).name for l in maps.splitlines() if '/platforms/' in l and l.split()[-1].endswith('.so')})
        environ = dict(kv.split('=', 1) for kv in env.decode(errors='replace').split('\0') if '=' in kv)
        procs.append({'pid': int(p.name), 'ppid': int((p / 'stat').read_text().split(') ', 1)[1].split()[1]), 'exe': exe, 'comm': (p / 'comm').read_text().strip(),
                      'cmd': cmd[:200], 'qtPlatformPlugins': platforms, 'QT_QPA_PLATFORM': environ.get('QT_QPA_PLATFORM'),
                      'APPIMAGE': environ.get('APPIMAGE')})
    return sorted(procs, key=lambda x: x['pid'])


def start(args):
    run = Run(args.name)
    t0 = time.monotonic()
    launch(run, 'main')
    report = {'startSeconds': round(time.monotonic() - t0, 2)}
    stored = run.socket_request('setup.storeCredentials', {'profileId': 'afa-fixture', 'expectedRevision': run.ok('settings.get')['metadata']['acceptedRevision'],
                                                           'credentials': {'accessKey': ACCESS, 'secretKey': SECRET}})
    if not stored.get('ok'): raise SystemExit('storeCredentials: ' + json.dumps(stored.get('error')))
    job = run.wait(lambda: (lambda j: j if j.get('ok') and j['result']['state'] != 'running' else None)(run.cli('setup.inspect', {'id': stored['result']['setupId']})), 30)
    report['credentials'] = job['result']['state'] if job else None
    sock = Path(run.meta['runtime']) / 'putgd/session.sock'
    report['sessionSock'] = sock.is_socket()
    report['processes'] = tree(run)
    report['diagnostics'] = {k: v for k, v in run.ok('diagnostics').items() if k in ('build', 'release', 'backend', 'qtVersion', 'journalReady', 'capabilities')}
    snap = run.ok('ui.snapshot')
    report['ui'] = {k: snap['diagnostics'].get(k) for k in ('ui', 'trayAvailable', 'trayTint', 'loginStartup', 'update')}
    report['kdeEntries'] = sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))
    run.meta['started'] = iso(); run.save()
    out(run.record('start', report))


def processes(args):
    run = Run(args.name); out(run.record(args.key or 'processes', {'at': iso(), 'processes': tree(run),
                                                                     'entries': sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))}))


# ---------------------------------------------------------------- C. idle memory

def memory(args):
    run = Run(args.name)
    resident = [p for p in tree(run) if p['comm'] == 'putgd']
    if len(resident) != 1: raise SystemExit(f'resident not unique: {resident}')
    pid = resident[0]['pid']; page = os.sysconf('SC_PAGE_SIZE')
    samples = []; start = time.monotonic(); load = [os.getloadavg()]
    while True:
        rss = int(Path(f'/proc/{pid}/statm').read_text().split()[1]) * page
        elapsed = time.monotonic() - start
        samples.append({'t': round(elapsed, 1), 'rssBytes': rss})
        if elapsed >= args.seconds: break
        time.sleep(10 if args.seconds - elapsed > 10 else max(args.seconds - elapsed, 0.1))
    roll = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        k, v = line.split(':', 1); roll[k] = int(v.split()[0]) * 1024
    libs = sorted({Path(l.split()[-1]).name for l in Path(f'/proc/{pid}/maps').read_text().splitlines() if len(l.split()) >= 6 and '/' in l.split()[-1]})
    flagged = {
        'gtk': [l for l in libs if re.search(r'libgtk|libgdk-3|libgdk_pixbuf|libqgtk3', l)],
        'kdePlatformTheme': [l for l in libs if re.search(r'KDEPlasmaPlatformTheme|libKF\d|plasma-integration', l)],
        'libssl': [l for l in libs if re.search(r'libssl|libcrypto|libgnutls|libqopensslbackend', l)],
        'qtNetwork': [l for l in libs if 'libQt6Network' in l],
    }
    load.append(os.getloadavg())
    report = {'pid': pid, 'exe': resident[0]['exe'], 'seconds': args.seconds, 'samples': samples,
              'finalRssMiB': round(samples[-1]['rssBytes'] / 2**20, 3), 'peakRssMiB': round(max(s['rssBytes'] for s in samples) / 2**20, 3),
              'pssMiB': round(roll['Pss'] / 2**20, 3), 'privateMiB': round((roll['Private_Clean'] + roll['Private_Dirty']) / 2**20, 3),
              'budgetMiB': 40, 'flaggedLibraries': flagged, 'libraryCount': len(libs), 'load': load}
    report['pass'] = report['peakRssMiB'] < 40 and not any(flagged.values())
    out(run.record('memory', report))


# ---------------------------------------------------------------- B. tray and window icon

def sni(run):
    env = run.env(); bus = ['busctl', '--user', '--json=short']
    def call(*a):
        r = subprocess.run(bus + list(a), env=env, capture_output=True, text=True, timeout=10)
        return json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {'error': r.stderr.strip()}
    resident = [p['pid'] for p in tree(run) if p['comm'] == 'putgd']
    items = call('get-property', 'org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher', 'org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems')
    ours = []
    for item in items.get('data', []) or []:
        service = item.split('/')[0]
        owner = call('call', 'org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'GetConnectionUnixProcessID', 's', service)
        if owner.get('data', [None])[0] in resident: ours.append(service)
    result = {'watcherItemCount': len(items.get('data') or []), 'ours': ours, 'call': call}
    return result

def tray_menu(args):
    run = Run(args.name); s = sni(run); call = s.pop('call')
    if s['ours']:
        svc = s['ours'][0]
        layout = call('call', svc, '/Menu', 'com.canonical.dbusmenu', 'GetLayout', 'iias', '--', '0', '-1', '0')
        entries = []
        for child in layout['data'][1][2]:
            c = child['data'] if isinstance(child, dict) else child
            p = {k: v['data'] for k, v in c[1].items()}
            entries.append({'id': c[0], 'label': p.get('label'), 'visible': p.get('visible', True), 'enabled': p.get('enabled', True), 'type': p.get('type', 'standard')})
        s['menu'] = entries
        for prop in ('Id', 'Title', 'Status', 'IconName'):
            s[prop] = call('get-property', svc, '/StatusNotifierItem', 'org.kde.StatusNotifierItem', prop).get('data')
    out(run.record('trayMenu', s))

def baseline(args):
    run = Run(args.name); frames, inp, size = tools(run); park(run, inp, size)
    save_frame(run, frames.grab(), args.label); out({'saved': args.label, 'size': size})

def shot(args):
    """Crop what changed since the named baseline."""
    run = Run(args.name); frames, inp, size = tools(run)
    if not args.no_park: park(run, inp, size)
    f = frames.grab(); base = magenta(size) if args.base == 'magenta' else load_frame(run, args.base)
    area = tuple(int(v) for v in args.area.split(',')) if args.area else None
    rep = crop(run, f, base, args.label, pick=lambda c: c['blocks'] >= args.min_blocks, theme=args.theme, area=area, min_changed=args.min_changed)
    out(run.record('shot:' + args.label, rep))

def click(args):
    run = Run(args.name); frames, inp, size = tools(run)
    if args.key: inp.key(*args.key.split('+'))
    elif args.drag: inp.drag(*[int(v) for v in args.drag.split(',')])
    elif args.type: inp.type(args.type)
    else: inp.click(args.x, args.y, args.button)
    time.sleep(.3)
    if args.park: park(run, inp, size)
    out({'ok': True})

def tray_click(args):
    """Right-click (or left) the tray icon with real input, crop the menu, close it with Escape."""
    run = Run(args.name); frames, inp, size = tools(run); park(run, inp, size); base = frames.grab()
    inp.click(args.x, args.y, args.button); time.sleep(1.5); park(run, inp, size); f = frames.grab()
    rep = crop(run, f, base, args.label, pick=lambda c: c['blocks'] > 30, theme=args.theme, min_changed=.3)
    rep['full'] = fullshot(run, f, args.label)
    if args.select is None:
        inp.key('Escape')
    else:
        for _ in range(abs(args.select)): inp.key('Down' if args.select > 0 else 'Up'); time.sleep(.25)
        time.sleep(.5); rep['selected'] = crop(run, frames.grab(), base, args.label + '-selected', pick=lambda c: c['blocks'] > 30, min_changed=.3)
        if args.activate: inp.key('Return')
        else: inp.key('Escape')
    out(run.record('tray:' + args.label, rep))

def find_tray(args):
    """Locate put.gd's tray icon: its tint colour (ui.snapshot trayTint) clustered in a panel strip."""
    run = Run(args.name); frames, inp, size = tools(run); park(run, inp, size); f = frames.grab(); w, h = size
    snap = run.cli('ui.snapshot')
    if not snap.get('ok'): snap = run.cli('ui.snapshot', private=False)  # the instance started at login (default profile)
    tint = snap['result']['diagnostics']['trayTint']['color'].lstrip('#'); colour = tuple(int(tint[i:i + 2], 16) for i in (0, 2, 4))
    strips = {'top': range(0, 48), 'bottom': range(h - 64, h)}
    found = {}
    for name, rows in strips.items():
        pts = [(x, y) for y in rows for x in range(w) if not differs(pixel(f, x, y), colour, 24)]
        bins = collections.Counter(p[0] // 28 for p in pts)
        if bins and bins.most_common(1)[0][1] >= 10:
            best = bins.most_common(1)[0][0]
            pts = [p for p in pts if abs(p[0] // 28 - best) <= 1]
            xs = sorted(p[0] for p in pts); ys = sorted(p[1] for p in pts)
            found[name] = {'pixels': len(pts), 'centre': [xs[len(xs) // 2], ys[len(ys) // 2]], 'box': [xs[0], ys[0], xs[-1], ys[-1]]}
    out(run.record('findTray', {'tint': '#' + tint, 'size': size, 'found': found}))

def window(args):
    """Open Settings: putgd-ui's process and Qt platform, the window (theme-checked), its title bar and the Alt+Tab switcher."""
    run = Run(args.name); frames, inp, size = tools(run); park(run, inp, size)
    base = magenta(size) if args.base == 'magenta' else frames.grab()
    run.ok('ui.show', {'view': 'settings'}); time.sleep(args.wait); park(run, inp, size)
    rep = {'processes': [p for p in tree(run) if p['comm'] in ('putgd', 'putgd-ui')],
           'entries': sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))}
    f = frames.grab()
    rep['window'] = crop(run, f, base, args.label + '-window', pick=lambda c: c['blocks'] > 300, theme=True, min_changed=.3)
    rep['full'] = fullshot(run, f, args.label + '-window')
    comps = [c for c in components(base, f) if c['blocks'] > 300]
    if comps:
        x0, y0, x1, y1 = comps[0]['box']
        rep['titlebar'] = crop(run, f, base, args.label + '-titlebar', area=(x0, max(y0 - 8, 0), x1, min(y0 + args.titlebar, size[1])), pick=lambda c: c['blocks'] > 20, min_changed=.3)
    inp.hold('alt', True); time.sleep(.15); inp.key('Tab'); time.sleep(args.switch_wait)
    g = frames.grab(); inp.hold('alt', False); time.sleep(.5)
    rep['switcher'] = crop(run, g, f, args.label + '-switcher', pick=lambda c: c['blocks'] > 20, min_changed=.3)
    rep['switcherFull'] = fullshot(run, g, args.label + '-switcher')
    out(run.record('window:' + args.label, rep))

def switcher(args):
    """Hold Alt, press Tab, crop the task switcher, release."""
    run = Run(args.name); frames, inp, size = tools(run); park(run, inp, size); base = frames.grab()
    inp.hold('alt', True); time.sleep(.1); inp.key('Tab'); time.sleep(args.wait)
    f = frames.grab(); inp.hold('alt', False)
    rep = crop(run, f, base, args.label, pick=lambda c: c['blocks'] > 20, min_changed=.3)
    out(run.record('switcher:' + args.label, rep))


# ---------------------------------------------------------------- cover (magenta + scrolling pattern)

def cover(args):
    run = Run(args.name)
    if args.action == 'start':
        fifo = run.dir / 'cover.in'
        if not fifo.exists(): os.mkfifo(fifo, 0o600)
        if os.environ.get('AFA_COVER') == 'gtk':
            # VMs without GStreamer's gtksink (Aurora): a GTK 3 + cairo cover and pattern.
            cmd = [sys.executable, str(HERE / 'gtk_pattern.py'), '--size', args.video_size] + ([] if args.pattern else ['--still'])
        else:
            cmd = [sys.executable, str(HERE / 'cover_player.py'), 'play', '--title', 'put.gd acceptance cover']
            if args.pattern: cmd += ['--pattern', '--video-size', args.video_size]
            if args.output: cmd += ['--output', args.output]
        logpath = run.dir / 'logs' / ('cover-%s.jsonl' % time.strftime('%H%M%S'))
        log = open(logpath, 'w')
        p = subprocess.Popen(cmd, env=run.env(private=False), stdin=os.open(fifo, os.O_RDWR), stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        run.meta['pids']['cover'] = p.pid; run.meta['coverLog'] = str(logpath); run.meta['pattern'] = args.video_size if args.pattern else None; run.save()
        states = lambda: [json.loads(x).get('state') for x in logpath.read_text().splitlines() if x.startswith('{')]
        covered = run.wait(lambda: 'covered' in states(), 15)
        pre = None
        if args.precheck and covered:
            time.sleep(1.5)
            pre = subprocess.run([sys.executable, str(HERE / 'cover_player.py'), 'precheck', '--port', str(run.meta['vnc'])] + (['--output', args.output] if args.output else []),
                                 env=run.env(private=False), capture_output=True, text=True, timeout=60)
            pre = pre.stdout.strip().splitlines()[-1:] if pre.stdout else pre.stderr[-300:]
        playing = None
        if args.pattern and covered:
            with open(fifo, 'w') as f: f.write('media\n')
            playing = run.wait(lambda: 'playing' in states(), 10)
        time.sleep(1)
        result = {'pid': p.pid, 'covered': bool(covered), 'playing': bool(playing), 'states': states(), 'precheck': pre}
        if not getattr(args, 'quiet', False): out(result)
        return result
    else:
        pid = run.meta['pids'].pop('cover', None)
        if pid:
            try:
                with open(run.dir / 'cover.in', 'w') as f: f.write('quit\n')
            except OSError: pass
            if not run.wait(lambda: not Path(f'/proc/{pid}').exists(), 10):
                os.kill(pid, signal.SIGTERM)
        run.save()
        if not getattr(args, 'quiet', False): out({'stopped': pid})


def check_capture(run, path, label):
    """A capture is kept only if it shows the cover (magenta, or the pattern on it)."""
    r = subprocess.run([sys.executable, str(HERE / 'cover_player.py'), 'check-frame', str(path), '--min-fraction', '0.5', '--edges', 'top,bottom,left,right'],
                       capture_output=True, text=True, timeout=60)
    last = (r.stdout.strip().splitlines() or ['{}'])[-1]
    try: return json.loads(last)
    except ValueError: return {'ok': False, 'raw': last}


# ---------------------------------------------------------------- clipboard witnesses

def clipboard(run, mime):
    env = run.env(private=False)
    if env.get('XDG_SESSION_TYPE') == 'x11':
        r = subprocess.run(['xclip', '-o', '-selection', 'clipboard', '-t', mime], env=env, capture_output=True, timeout=10)
        return r.stdout if r.returncode == 0 else None
    if shutil.which('wl-paste'):
        r = subprocess.run(['wl-paste', '--no-newline', '--type', mime], env=env, capture_output=True, timeout=10)
        if r.returncode == 0: return r.stdout
    # GNOME (no data-control): a GTK 4 window reads it with focus.
    script = r'''
import sys, gi
gi.require_version("Gtk", "4.0"); gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk, GLib, Gio
mime = sys.argv[1]; result = {}
def done(src, res):
    try:
        stream, _ = src.read_finish(res); data = b""
        while True:
            chunk = stream.read_bytes(65536, None).get_data()
            if not chunk: break
            data += chunk
        sys.stdout.buffer.write(data)
    except Exception as e: sys.stderr.write(repr(e))
    app.quit()
def activate(a):
    w = Gtk.Window(application=a, title="put.gd acceptance clipboard witness"); w.set_default_size(160, 60); w.present()
    GLib.timeout_add(700, lambda: (w.get_clipboard().read_async([mime], 0, None, done), False)[1])
app = Gtk.Application(application_id="gd.put.AcceptanceClipboard", flags=Gio.ApplicationFlags.NON_UNIQUE); app.connect("activate", activate); app.run([])
'''
    r = subprocess.run([sys.executable, '-c', script, mime], env=env, capture_output=True, timeout=20)
    return r.stdout if r.stdout else None


# ---------------------------------------------------------------- D. captures, edit, upload, notifications

def operation_done(run, op_id, timeout=90):
    def check():
        r = run.cli('operation.inspect', {'id': op_id})
        if r.get('ok') and r['result'].get('state') in ('completed', 'partial', 'failed', 'interrupted', 'cancelled'): return r['result']
    return run.wait(check, timeout, .5)

def summarize_op(op):
    items = []
    for it in op.get('items', []):
        items.append({'name': (it.get('artifact') or {}).get('name'), 'mime': (it.get('artifact') or {}).get('mime'), 'uploadStatus': it.get('uploadStatus'),
                      'link': (it.get('receipt') or {}).get('directUrl'), 'local': it.get('local')})
    return {'id': op.get('id'), 'state': op.get('state'), 'copyStatus': op.get('copyStatus'), 'items': items, 'error': op.get('error')}

def capture(args):
    run = Run(args.name); frames, inp, size = tools(run)
    seen = len(run.logs('Notification shown'))
    if args.kind == 'region':
        r = run.ok('action.invoke', {'action': 'region'})
        time.sleep(args.selector_wait)
        x0, y0, x1, y1 = [int(v) for v in args.drag.split(',')]
        inp.drag(x0, y0, x1, y1)
    else:
        if args.kind == 'window' and args.focus:
            fx, fy = [int(v) for v in args.focus.split(',')]; inp.click(fx, fy); time.sleep(.6)
        r = run.ok('action.invoke', {'action': args.kind})
    op = operation_done(run, r['operationId'])
    report = {'kind': args.kind, 'operation': summarize_op(op or {'id': r['operationId'], 'state': 'timeout'})}
    item = (op or {}).get('items', [{}])[0] if op else {}
    local = (item.get('local') or {}).get('path')
    if local and Path(local).exists():
        dest = run.dir / 'out' / f'capture-{args.kind}.png'; shutil.copyfile(local, dest)
        report['png'] = {'bytes': dest.stat().st_size, 'sha256': sha256(dest), 'magic': dest.read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'}
        report['coverCheck'] = check_capture(run, dest, args.kind)
        if report['coverCheck'].get('ok'):
            img = png_read(dest.read_bytes()); report['png']['size'] = img[:2]
    link = (item.get('receipt') or {}).get('directUrl')
    clip = clipboard(run, 'text/plain;charset=utf-8') or clipboard(run, 'text/plain')
    report['clipboard'] = clip.decode(errors='replace')[:300] if clip else None
    report['linkCopied'] = bool(link) and report['clipboard'] is not None and report['clipboard'].strip() == link
    if link:
        import urllib.request
        with urllib.request.urlopen(link, timeout=10) as reply: body = reply.read()
        report['linkServes'] = {'status': 200, 'sha256': hashlib.sha256(body).hexdigest(), 'matchesLocal': bool(local) and hashlib.sha256(body).hexdigest() == report.get('png', {}).get('sha256')}
    history = run.ok('history.list', {'limit': 20, 'offset': 0})['records']
    report['historyEntry'] = next(({'id': h['id'], 'name': h['name'], 'uploadStatus': h.get('uploadStatus'), 'local': (h.get('local') or {}).get('status'), 'linkAvailable': h.get('linkAvailable')}
                                   for h in history if h.get('operationId') == r['operationId']), None)
    report['notifications'] = [n for n in run.logs('Notification shown')[seen:]]
    out(run.record('capture:' + args.kind, report))


def edit(args):
    """Edit & upload: blur + solid redaction on a captured PNG, Save copy… (dialog when the build has #321), Upload."""
    run = Run(args.name); frames, inp, size = tools(run)
    src = run.dir / 'out' / f'capture-{args.source}.png'
    if not src.exists(): raise SystemExit(f'no {src}')
    park(run, inp, size); base = frames.grab()
    r = run.ok('editor.open', {'source': 'file', 'path': str(src)})
    eid = r['operationId']
    state = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
    if not state: raise SystemExit('editor did not open')
    w, h = state['width'], state['height']
    # Both inside the cover's pattern (the middle of the screen), so the blur has detail to soften.
    blur = [w * 30 // 100, h * 32 // 100, w * 18 // 100, h * 34 // 100]; solid = [w * 52 // 100, h * 36 // 100, w * 14 // 100, h * 22 // 100]
    applied = run.wait(lambda: (lambda x: x if x.get('ok') or (x.get('error') or {}).get('code') != 'editor_starting' else None)(
        run.cli('editor.apply', {'id': eid, 'commands': [{'op': 'add', 'shape': {'kind': 'blur', 'rect': blur, 'strength': 24}},
                                                        {'op': 'add', 'shape': {'kind': 'redact', 'rect': solid}}]})), 20)
    report = {'editorId': eid, 'size': [w, h], 'blur': blur, 'solid': solid, 'apply': applied.get('ok'), 'inspect': run.cli('editor.inspect', {'id': eid}).get('result')}
    time.sleep(1.5); park(run, inp, size); ef = frames.grab()
    report['window'] = crop(run, ef, base, 'editor', pick=lambda c: c['blocks'] > 300, theme=True, min_changed=.3, own_min=.05)
    report['full'] = fullshot(run, ef, 'editor')
    # Save copy…
    target = run.dir / 'out' / 'edited-copy.png'
    caps = run.ok('capabilities')
    commands = (caps.get('editor') or {}).get('commands') or []
    report['editorCommands'] = commands
    if 'fileDialog' in json.dumps(commands) or args.dialog:
        opened = run.cli('editor.fileDialog', {'id': eid, 'action': 'open'})
        report['dialogOpen'] = opened
        st = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and (x['result'].get('open') or (x['result'].get('last') or {}).get('status')) else None)(
            run.cli('editor.fileDialog', {'id': eid, 'action': 'inspect'})), 20)
        report['dialogState'] = st
        time.sleep(2.5); park(run, inp, size); df = frames.grab()
        report['dialogShot'] = crop(run, df, ef, 'save-copy-dialog', pick=lambda c: c['blocks'] > 300, min_changed=.3)
        report['dialogFull'] = fullshot(run, df, 'save-copy-dialog')
        if st and st.get('open'):
            # A plain, unique file name in the chooser's own folder (Pictures); cleanup removes it.
            name = f'afa-{run.name}-edited.png'
            inp.key('ctrl', 'a'); time.sleep(.3); inp.type(name); time.sleep(.6); inp.key('Return')
            fin = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and not x['result'].get('open') and (x['result'].get('last') or {}).get('status') else None)(run.cli('editor.fileDialog', {'id': eid, 'action': 'inspect'})), 30)
            report['dialogResult'] = fin
            chosen = ((fin or {}).get('last') or {}).get('path')
            if chosen:
                run.meta.setdefault('userFiles', []).append(chosen); run.save()
                run.wait(lambda: Path(chosen).exists(), 15); target = Path(chosen)
    else:
        report['dialog'] = 'no editor.fileDialog in this build: Save copy… is clicked in the window (see the run log)'
        if not args.no_save_api:
            report['saveCopyApi'] = run.cli('editor.saveCopy', {'id': eid, 'path': str(target), 'overwrite': True})
    report['savedCopy'] = {'exists': target.exists(), 'bytes': target.stat().st_size if target.exists() else 0}
    if target.exists():
        img = png_read(target.read_bytes()); orig = png_read(src.read_bytes())
        report['savedCopy'].update(size=img[:2], solidRegion=region_stats(img, solid), blurChangedShare=region_diff(img, orig, blur))
    # Upload
    seen = len(run.logs('Notification shown'))
    if args.notify: set_notifications(run, True); nmon = notify_monitor(run); park(run, inp, size); nbase = frames.grab()
    sub = run.ok('editor.submit', {'id': eid})
    op = operation_done(run, sub.get('operationId') or eid)
    report['upload'] = summarize_op(op or {'state': 'timeout'})
    link = report['upload']['items'][0]['link'] if report['upload'].get('items') else None
    if link:
        import urllib.request
        with urllib.request.urlopen(link, timeout=10) as reply: body = reply.read()
        img = png_read(body); orig = png_read(src.read_bytes())
        report['uploaded'] = {'size': img[:2], 'solidRegion': region_stats(img, solid), 'blurChangedShare': region_diff(img, orig, blur),
                              'unchangedOutsideShare': outside_same(img, orig, [blur, solid])}
        report['redactionVerified'] = (report['uploaded']['solidRegion']['spread'] <= 2 and max(report['uploaded']['solidRegion']['mean']) <= 2
                                       and report['uploaded']['blurChangedShare'] >= .3 and report['uploaded']['unchangedOutsideShare'] >= .99)
    clip = clipboard(run, 'text/plain;charset=utf-8') or clipboard(run, 'text/plain')
    report['clipboard'] = clip.decode(errors='replace')[:300] if clip else None
    report['linkCopied'] = bool(link) and (report['clipboard'] or '').strip() == link
    if args.notify:
        report['notification'] = notify_check(run, frames, inp, size, nbase, seen, 'edit-upload', monitor=nmon); set_notifications(run, False)
    out(run.record('edit', report))


def notify_monitor(run):
    return subprocess.Popen(['dbus-monitor', "interface='org.freedesktop.Notifications'"], env=run.env(private=False),
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

def notify_check(run, frames, inp, size, base, seen, label, click=True, monitor=None):
    shown = run.wait(lambda: run.logs('Notification shown')[seen:] or None, 30, .25)
    rep = {'logged': shown}
    if not shown: return rep
    time.sleep(1.2); f = frames.grab()
    rep['banner'] = crop(run, f, base, f'{label}-notification', pick=lambda c: 30 < c['blocks'] < 4000, min_changed=.3)
    rep['full'] = fullshot(run, f, f'{label}-notification')
    box = rep['banner'].get('inner') or (rep['banner']['components'][0]['box'] if rep['banner']['components'] else None)
    if click and box:
        target = ((box[0] + box[2]) // 2, box[1] + (box[3] - box[1]) // 3)
        monitor = monitor or notify_monitor(run)
        time.sleep(.8)
        inp.click(*target); rep['clicked'] = target; time.sleep(3); park(run, inp, size)
        monitor.terminate(); signals = monitor.communicate(timeout=5)[0]
        rep['dbusMembers'] = re.findall(r'member=(\w+)', signals)
        notify_call = signals.split('member=Notify', 1)[1][:1500] if 'member=Notify' in signals else ''
        rep['notifyActions'] = re.findall(r'string "(default|[^"]{1,40})"', notify_call.split('array [', 2)[1])[:6] if notify_call.count('array [') >= 2 else None
        rep['actionInvoked'] = re.findall(r'member=ActionInvoked\n\s+uint32 \d+\n\s+string "([^"]*)"', signals)
    urls = run.dir / 'opened-urls.txt'
    rep['openedUrls'] = urls.read_text().splitlines() if urls.exists() else []
    return rep


def set_notifications(run, on):
    snap = run.ok('settings.get')
    return run.cli('settings.patch', {'patch': {'preferences': {'notifications': on}}, 'expectedRevision': snap['metadata']['observedRevision']}).get('ok')

def notify(args):
    """Notifications on, re-upload a capture through Edit & upload with the cover down, crop the banner, click it, notifications off."""
    run = Run(args.name); frames, inp, size = tools(run)
    on = set_notifications(run, True); monitor = notify_monitor(run); time.sleep(.5)
    park(run, inp, size); base = frames.grab(); seen = len(run.logs('Notification shown'))
    src = run.dir / 'out' / f'capture-{args.source}.png'
    r = run.ok('editor.open', {'source': 'file', 'path': str(src)}); eid = r['operationId']
    run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
    sub = run.ok('editor.submit', {'id': eid})
    op = operation_done(run, sub.get('operationId') or eid)
    rep = {'notificationsOn': on, 'operation': summarize_op(op or {}), 'notification': notify_check(run, frames, inp, size, base, seen, args.label, click=not args.no_click, monitor=monitor)}
    rep['notificationsOff'] = set_notifications(run, False)
    out(run.record('notify:' + args.label, rep))


# ---------------------------------------------------------------- E. recording

def rec_wait(run, rid, states, timeout):
    def check():
        r = run.cli('recording.inspect', {'id': rid})
        if r.get('ok') and r['result'].get('state') in states: return r['result']
    return run.wait(check, timeout, .4)

def motion(run, clip):
    """putgd-record --motion (mean frame difference over 8 samples); Pillow for animated WebP, which the bundled FFmpeg can't decode."""
    r = subprocess.run([str(run.appdir / 'usr/bin/putgd-record'), '--motion', str(clip), '8'], env=dict(run.cli_env(), APPDIR=str(run.appdir)),
                       capture_output=True, text=True, timeout=300)
    found = re.search(r'^motion=([0-9.]+) compared=(\d+) samples=(\d+) durationMs=(\d+) size=(\S+)', r.stdout, re.M)
    if found:
        return {'motion': float(found.group(1)), 'compared': int(found.group(2)), 'durationMs': int(found.group(4)), 'size': found.group(5), 'by': 'putgd-record'}
    try:
        from PIL import Image, ImageChops, ImageStat
        import statistics
        image = Image.open(clip); count = getattr(image, 'n_frames', 1)
        picks = sorted({round(i * (count - 1) / 7) for i in range(8)}) if count > 1 else [0]
        frames = []
        for index in picks: image.seek(index); frames.append(image.convert('L').copy())
        diffs = [statistics.mean(ImageStat.Stat(ImageChops.difference(a, b)).mean) for a, b in zip(frames, frames[1:])]
        return {'motion': round(statistics.mean(diffs), 3) if diffs else 0.0, 'frames': count, 'size': '%dx%d' % image.size, 'by': 'pillow'}
    except Exception as error:
        return {'error': ((r.stdout + r.stderr).strip()[-200:]) + ' / ' + type(error).__name__}

def record(args):
    run = Run(args.name); frames, inp, size = tools(run)
    if run.meta.get('recordingId'): run.cli('recording.cancel', {'id': run.meta.pop('recordingId')}); run.save()
    caps = run.ok('recording.capabilities', timeout=40)
    report = {'capabilities': {k: caps.get(k) for k in ('available', 'reason', 'acquisition', 'gstreamer', 'exportEncoders', 'liveCapture', 'kdeScreencast', 'formats')}}
    report['entriesDuring'] = sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))
    rect = [int(v) for v in args.rect.split(',')]
    vw, vh = (int(v) for v in (run.meta.get('pattern') or '0x0').split('x'))
    moving = [(size[0] - vw) // 2, (size[1] - vh) // 2, (size[0] + vw) // 2, (size[1] + vh) // 2] if vw else None
    report['patternRect'] = moving
    park(run, inp, size); base = magenta(size)
    prep = run.ok('recording.prepare', {'target': {'kind': 'region', 'rect': rect}, 'format': 'mp4', 'fps': 30})
    rid = prep['recordingId']
    ready = rec_wait(run, rid, ('ready',), args.prepare_timeout)
    report['prepare'] = {'state': (ready or {}).get('state'), 'target': (ready or {}).get('target')}
    if not ready: out(run.record('record', report)); return
    near = lambda c: c['box'][0] < rect[0] + rect[2] + 80 and c['box'][2] > rect[0] - 80 and c['box'][1] < rect[1] + rect[3] + 80 and c['box'][3] > rect[1] - 80
    time.sleep(1); report['readyShot'] = crop(run, frames.grab(), base, 'record-ready', pick=lambda c: c['blocks'] > 20 and near(c), min_changed=.2, ignore=moving)
    run.ok('recording.start', {'id': rid}); t0 = time.monotonic()
    time.sleep(2); live = run.ok('recording.inspect', {'id': rid}); report['liveEncoder'] = (live.get('progress') or {}).get('encoder')
    rf = frames.grab()
    report['recordingShot'] = crop(run, rf, base, 'recording', pick=lambda c: c['blocks'] > 20 and near(c), min_changed=.2, ignore=moving)
    report['recordingFull'] = fullshot(run, rf, 'recording')
    time.sleep(args.pause_at - 2); run.ok('recording.pause', {'id': rid}); time.sleep(args.pause_for)
    report['paused'] = (run.ok('recording.inspect', {'id': rid}).get('progress') or {}).get('paused')
    run.ok('recording.resume', {'id': rid}); time.sleep(args.after)
    stop = run.ok('recording.stop', {'id': rid}, timeout=60)
    reviewing = rec_wait(run, rid, ('reviewing',), 60)
    report['stopped'] = {'durationUs': (reviewing or {}).get('durationUs'), 'wallSeconds': round(time.monotonic() - t0, 1), 'state': (reviewing or {}).get('state')}
    # Review window: trim and a timed blur, as the window's own controls do it.
    dur = reviewing['durationUs']
    steps = []
    def review(action, **kw):
        r = run.cli('recording.review', dict(id=rid, action=action, **kw)); steps.append({'action': action, **kw, 'ok': r.get('ok'), 'error': r.get('error')}); return r
    review('seek', time=1_000_000); review('setIn')
    review('seek', time=dur - 1_000_000); review('setOut')
    cw, ch = reviewing.get('canvas') or rect[2:]
    review('seek', time=2_000_000)
    review('addMask', kind='blur', rect=[cw // 4, ch // 4, cw // 2, ch // 2])
    review('setMaskIn'); review('seek', time=5_000_000); review('setMaskOut'); review('strength', strength=24)
    review('seek', time=3_000_000)
    report['reviewSteps'] = steps
    insp = run.ok('recording.inspect', {'id': rid}); report['edit'] = insp.get('edit')
    # The review window covers the moving pattern, so the pattern's rectangle is ignored when finding it.
    run.cli('recording.raise', {'id': rid}); time.sleep(2); park(run, inp, size)
    report['reviewShot'] = crop(run, frames.grab(), base, 'review', pick=lambda c: c['blocks'] > 300, theme=True, min_changed=.3, own_min=.05, ignore=moving)
    if args.drop_cover:
        # The real session behind the review window from here on (the clip is already recorded).
        cover(argparse.Namespace(name=args.name, action='stop', quiet=True)); moving = None
        run.cli('recording.raise', {'id': rid}); time.sleep(2); park(run, inp, size)
    revf = frames.grab(); report['reviewFull'] = fullshot(run, revf, 'review')
    # Export all four formats and score each against the still control later.
    report['exports'] = {}
    codec = {'mp4': 'h264', 'webm': 'vp9', 'gif': '', 'webp': ''}
    for fmt in ('mp4', 'webm', 'gif', 'webp'):
        chosen = review('format', format=fmt, codec=codec[fmt])
        path = run.dir / 'out' / f'recording.{fmt}'
        before = (run.ok('recording.inspect', {'id': rid}).get('lastExport') or {}).get('path')
        r = run.cli('recording.saveCopy', {'id': rid, 'path': str(path), 'overwrite': True})
        def finished():
            last = run.ok('recording.inspect', {'id': rid}).get('lastExport') or {}
            return last if last.get('path') == str(path) and last.get('status') in ('saved', 'failed') else None
        last = run.wait(finished, 300, 1) or {}
        entry = {'formatChosen': chosen.get('ok'), 'request': r.get('ok'), 'requestError': r.get('error'), 'status': last.get('status'),
                 'bytes': path.stat().st_size if path.exists() else 0, 'error': last.get('error'), 'durationUs': last.get('durationUs')}
        if path.exists() and path.stat().st_size: entry['motion'] = motion(run, path); entry['sha256'] = sha256(path)
        report['exports'][fmt] = entry
    # One copy through Save copy… (the dialog when the build has #321).
    copy = run.dir / 'out' / 'recording-dialog-copy.mp4'
    review('format', format='mp4', codec='h264')
    dlg = run.cli('recording.review', {'id': rid, 'action': 'saveCopyDialog'})
    report['saveCopyDialog'] = {'open': dlg}
    if dlg.get('ok'):
        time.sleep(2.5); park(run, inp, size); dfr = frames.grab()
        report['saveCopyDialog']['shot'] = crop(run, dfr, revf, 'review-save-dialog', pick=lambda c: c['blocks'] > 300, min_changed=.3)
        report['saveCopyDialog']['full'] = fullshot(run, dfr, 'review-save-dialog')
        inp.key('ctrl', 'a'); time.sleep(.3); inp.type(f'afa-{run.name}-review.mp4'); time.sleep(.6); inp.key('Return')
        def chosen():
            st = run.cli('recording.review', {'id': rid, 'action': 'inspect'}).get('result') or {}
            fd = st.get('fileDialog') or {}
            return fd if not fd.get('open') and (fd.get('last') or {}).get('status') else None
        fd = run.wait(chosen, 60, 1)
        report['saveCopyDialog']['inspect'] = fd
        path = ((fd or {}).get('last') or {}).get('path')
        if path:
            copy = Path(path); run.meta.setdefault('userFiles', []).append(path); run.save()
            run.wait(lambda: (lambda l: l if l.get('path') == path and l.get('status') in ('saved', 'failed') else None)(run.ok('recording.inspect', {'id': rid}).get('lastExport') or {}), 300, 1)
            report['saveCopyDialog']['lastExport'] = run.ok('recording.inspect', {'id': rid}).get('lastExport')
    report['saveCopyDialog']['exists'] = copy.exists()
    if copy.exists(): report['saveCopyDialog']['bytes'] = copy.stat().st_size; report['saveCopyDialog']['motion'] = motion(run, copy)
    # Upload the MP4 to the fixture.
    seen = len(run.logs('Notification shown'))
    up = run.cli('recording.upload', {'id': rid, 'revision': run.ok('recording.inspect', {'id': rid})['edit']['revision']})
    if up.get('ok'):
        op = operation_done(run, up['result']['operationId'], 240); report['upload'] = summarize_op(op or {})
    else:
        report['upload'] = up
    report['uploadNotifications'] = run.logs('Notification shown')[seen:]
    report['entriesAfter'] = sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))
    run.meta['recordingId'] = rid; run.save()
    out(run.record('record', report))


def control(args):
    """Static control: the same region of the cover with the pattern stopped (cover only), recorded and scored."""
    run = Run(args.name)
    rect = [int(v) for v in args.rect.split(',')]
    prep = run.ok('recording.prepare', {'target': {'kind': 'region', 'rect': rect}, 'format': 'mp4', 'fps': 30}); rid = prep['recordingId']
    rec_wait(run, rid, ('ready',), 120); run.ok('recording.start', {'id': rid}); time.sleep(args.seconds)
    run.ok('recording.stop', {'id': rid}, timeout=60); rec_wait(run, rid, ('reviewing',), 60)
    rep = {}
    for fmt, codec in (('mp4', 'h264'), ('gif', '')):
        run.cli('recording.review', {'id': rid, 'action': 'format', 'format': fmt, 'codec': codec})
        path = run.dir / 'out' / f'control.{fmt}'
        run.cli('recording.saveCopy', {'id': rid, 'path': str(path), 'overwrite': True})
        run.wait(lambda: (lambda l: l if l.get('path') == str(path) and l.get('status') in ('saved', 'failed') else None)(run.ok('recording.inspect', {'id': rid}).get('lastExport') or {}), 300, 1)
        rep[fmt] = {'bytes': path.stat().st_size if path.exists() else 0, 'motion': motion(run, path) if path.exists() else None}
    run.cli('recording.cancel', {'id': rid})
    out(run.record('control', rep))


# ---------------------------------------------------------------- G. history

def history(args):
    run = Run(args.name); frames, inp, size = tools(run)
    records = run.ok('history.list', {'limit': 50, 'offset': 0})
    rep = {'total': records['total'], 'records': [{'id': r['id'], 'name': r['name'], 'mime': r['mime'], 'uploadStatus': r.get('uploadStatus'),
                                                   'local': (r.get('local') or {}).get('status'), 'linkAvailable': r.get('linkAvailable')} for r in records['records']]}
    rep['previews'] = {}
    for r in records['records']:
        p = run.cli('history.preview', {'id': r['id']}, timeout=60)
        res = p.get('result') or {}
        path = res.get('previewPath')
        rep['previews'][r['name']] = {'ok': p.get('ok'), 'state': res.get('state'), 'source': res.get('source'), 'exists': bool(path) and Path(path).exists(),
                                      'error': p.get('error')}
    image = next((r for r in records['records'] if r['mime'] == 'image/png'), None)
    if image:
        cp = run.cli('history.copyImage', {'id': image['id']})
        data = clipboard(run, 'image/png')
        local = (image.get('local') or {}).get('path')
        rep['copyImage'] = {'reply': cp, 'clipboardPng': bool(data) and data[:8] == b'\x89PNG\r\n\x1a\n', 'clipboardBytes': len(data or b'')}
        if data and data[:8] == b'\x89PNG\r\n\x1a\n' and local and Path(local).exists():
            a = png_read(data); b = png_read(Path(local).read_bytes())
            rep['copyImage']['sameSize'] = a[:2] == b[:2]
            rep['copyImage']['samePixels'] = a[:2] == b[:2] and all(png_rgb(a, x, y) == png_rgb(b, x, y) for y in range(0, a[1], max(a[1] // 40, 1)) for x in range(0, a[0], max(a[0] // 40, 1)))
    park(run, inp, size); base = frames.grab()
    run.ok('ui.show', {'view': 'history'}); time.sleep(3); park(run, inp, size)
    hf = frames.grab()
    rep['windowShot'] = crop(run, hf, base, 'history', pick=lambda c: c['blocks'] > 300, theme=True, min_changed=.3)
    rep['full'] = fullshot(run, hf, 'history')
    out(run.record('history', rep))

def delete(args):
    """'Delete from destination…' on one image (the only per-item Delete), then where its bytes still are."""
    run = Run(args.name)
    records = run.ok('history.list', {'limit': 50, 'offset': 0})['records']
    target = next(r for r in records if r['name'].endswith('.png') and r.get('uploadStatus') == 'uploaded')
    link = (target.get('receipt') or {}).get('directUrl'); local = (target.get('local') or {}).get('path')
    prep = run.ok('delete.prepare', {'ids': [target['id']]})
    conf = run.ok('delete.confirm', {'ids': [target['id']], 'acknowledgement': prep['acknowledgement']})
    did = conf.get('id')
    fin = run.wait(lambda: (lambda x: x['result'] if x.get('ok') and x['result'].get('state') in ('completed', 'cancelled', 'failed') else None)(run.cli('delete.inspect', {'id': did})), 90)
    import urllib.request, urllib.error
    try:
        urllib.request.urlopen(link, timeout=10); remote = 'still served'
    except urllib.error.HTTPError as e: remote = f'HTTP {e.code}'
    after = run.ok('history.detail', {'id': target['id']})
    rep = {'item': target['name'], 'prepare': prep, 'confirm': conf, 'final': fin, 'remoteAfter': remote, 'remoteStatus': after.get('remoteStatus'),
           'localPath': local, 'localExistsAfter': bool(local) and Path(local).exists(), 'historyRowKept': bool(after)}
    out(run.record('delete', rep))


# ---------------------------------------------------------------- H. settings, J. update check

def settings(args):
    run = Run(args.name); frames, inp, size = tools(run)
    park(run, inp, size); base = frames.grab()
    run.ok('ui.show', {'view': 'settings'}); time.sleep(3)
    rep = {'schemes': {}}
    for theme in ('dark', 'light', 'system'):
        snap = run.ok('settings.get')
        r = run.cli('settings.patch', {'patch': {'preferences': {'appearance': {'theme': theme}}}, 'expectedRevision': snap['metadata']['observedRevision']})
        time.sleep(2.5); park(run, inp, size)
        view = run.cli('ui.viewState').get('result', {})
        rep['schemes'][theme] = {'patch': r.get('ok'), 'appearance': {k: (view.get('appearance') or {}).get(k) for k in ('theme', 'scheme', 'desktop', 'desktopSource')},
                                 'shot': crop(run, (sf := frames.grab()), base, f'settings-{theme}', pick=lambda c: c['blocks'] > 300, theme=True, min_changed=.3),
                                 'full': fullshot(run, sf, f'settings-{theme}')}
    st = run.ok('update.status')
    rep['updateStatus'] = {k: st.get(k) for k in ('channel', 'channelSetting', 'enabled', 'supported', 'reason', 'notifies', 'checkedAt', 'nextCheckAt', 'error', 'result', 'build')}
    seen = len(run.logs('Notification shown'))
    t = time.monotonic(); chk = run.cli('update.check', timeout=70)
    rep['checkNow'] = {'ok': chk.get('ok'), 'seconds': round(time.monotonic() - t, 2), 'result': (chk.get('result') or {}).get('result'), 'error': chk.get('error')}
    time.sleep(5)
    rep['notificationsAfterCheck'] = run.logs('Notification shown')[seen:]
    rep['liveFeed'] = fetch('https://app.put.gd/update/v1/nightly.json')[:2000]
    out(run.record('settings', rep))


def update_check(args):
    run = Run(args.name)
    tool = Path(args.tool).resolve()
    env = dict(os.environ); env.pop('DISPLAY', None); env.pop('WAYLAND_DISPLAY', None)
    r = subprocess.run([str(tool), '--check-for-update', run.meta['appimage']], capture_output=True, text=True, timeout=180, env=env)
    latest = fetch('https://app.put.gd/update/v1/nightly.json')
    zs = redirect('https://app.put.gd/dl/nightly/appimage.zsync')
    zs = re.sub(r'^(\d+ https://[^/]+/).*?filename%3D([^&]+).*$', r'\1…\2', zs)  # no signed query in evidence
    rep = {'tool': tool.name, 'toolSha256': sha256(tool), 'exit': r.returncode, 'stdout': r.stdout[-1500:], 'stderr': r.stderr[-1500:],
           'feed': latest[:1500], 'zsyncPermalink': zs, 'image': Path(run.meta['appimage']).name}
    out(run.record('updateCheck', rep))


# ---------------------------------------------------------------- I. start at login

def login(args):
    """enable: Launch at login in this run's profile and the session's own autostart (the real config home, see README).
    check: after the real logout/login, what started and from where. disable: turn it off and quit."""
    run = Run(args.name)
    if args.action == 'enable':
        snap = run.ok('settings.get')
        r = run.cli('settings.patch', {'patch': {'preferences': {'launchAtLogin': True}}, 'expectedRevision': snap['metadata']['observedRevision']})
        time.sleep(1)
        entry = run.dir / 'config/autostart/putgd.desktop'
        rep = {'patch': r.get('ok'), 'loginStartup': run.ok('ui.snapshot')['diagnostics'].get('loginStartup'),
               'privateEntry': entry.read_text() if entry.exists() else None}
        script = DATA_HOME / 'putgd/login/appimage-start.sh'
        rep['loginScript'] = script.read_text() if script.exists() else None
        # The session reads ~/.config/autostart: publish the same managed entry there.
        real = Path.home() / '.config/autostart/putgd.desktop'
        if real.exists(): raise SystemExit(f'{real} exists; refusing')
        if entry.exists():
            real.parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(entry, real); real.chmod(0o600)
            run.meta['realAutostart'] = str(real); run.save(); rep['sessionEntry'] = str(real)
        out(run.record('loginEnable', rep))
    elif args.action == 'check':
        uid = os.getuid(); found = []
        for p in Path('/proc').iterdir():
            if not p.name.isdigit(): continue
            try:
                if p.stat().st_uid != uid: continue
                comm = (p / 'comm').read_text().strip()
                if comm not in ('putgd', 'putgd-ui', 'AppRun', 'putgd-launcher'): continue
                env = dict(kv.split('=', 1) for kv in (p / 'environ').read_bytes().decode(errors='replace').split('\0') if '=' in kv)
                found.append({'pid': int(p.name), 'comm': comm, 'exe': os.readlink(p / 'exe'), 'APPIMAGE': env.get('APPIMAGE'),
                              'started': iso(datetime.fromtimestamp(p.stat().st_mtime, timezone.utc)), 'XDG_CONFIG_HOME': env.get('XDG_CONFIG_HOME')})
            except OSError: continue
        env, shell, spid = session_environment()
        real = Path.home() / '.config/autostart/putgd.desktop'
        rep = {'sessionEntryNow': real.read_text() if real.exists() else None, 'processes': found, 'shellStarted': iso(datetime.fromtimestamp(Path(f'/proc/{spid}').stat().st_mtime, timezone.utc)),
               'fromAppImage': any(f['APPIMAGE'] == run.meta['appimage'] for f in found)}
        out(run.record('loginCheck', rep))
    else:
        real = Path(run.meta.get('realAutostart') or '/nonexistent')
        if real.exists(): real.unlink()
        out({'removed': str(real)})


# ---------------------------------------------------------------- K. quit and cleanup

def leftovers(run):
    """put.gd processes of any profile: by executable (an AppImage mount, the AppImage), comm, or the AppImage path in argv."""
    uid = os.getuid(); image = run.meta['appimage']; procs = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            if p.stat().st_uid != uid: continue
            comm = (p / 'comm').read_text().strip(); exe = os.readlink(p / 'exe')
            argv = (p / 'cmdline').read_bytes().split(b'\0')
        except OSError: continue
        if '.mount_putgd' in exe or exe == image or comm in ('putgd', 'putgd-ui', 'putgd-record', 'putgd-launcher', 'AppRun') \
           or (argv and argv[0].decode(errors='replace') in (image,)) or any(a.decode(errors='replace') == image for a in argv[:2]):
            procs.append(f"{p.name} {comm} {exe}")
    mounts = [l.split()[1] for l in Path('/proc/mounts').read_text().splitlines() if '.mount_putgd' in l]
    entries = sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop'))
    return {'processes': procs, 'mounts': mounts, 'entries': entries}

def quit_tray(args):
    """Quit from the tray: real click opens the menu, Up selects the last item (Quit), Return activates."""
    run = Run(args.name)
    rep = {}
    if args.x is not None:
        frames, inp, size = tools(run); park(run, inp, size); base = frames.grab()
        inp.click(args.x, args.y, args.button); time.sleep(1.5); park(run, inp, size)
        opened = frames.grab()
        near = [c for c in components(base, opened) if c['blocks'] > 30 and abs((c['box'][0] + c['box'][2]) / 2 - args.x) < 400]
        rep['menuComponents'] = near[:3]
        if not near: rep['error'] = 'no menu opened'; out(run.record('quit', rep)); return
        # The menu's own rectangle (edges in its background colour), so a panel strip below it is excluded.
        menu = crop(run, opened, base, 'tray-menu-open', pick=lambda c: c['blocks'] > 30, min_changed=.3)
        x0, y0, x1, y1 = menu.get('inner') or near[0]['box']
        # Quit is the menu's last item, at the bottom of the menu in every tray host here.
        target = ((x0 + x1) // 2, y1 - args.inset)
        inp.move(*target); time.sleep(.8)
        rep['hover'] = target
        rep['menu'] = crop(run, frames.grab(), base, 'tray-quit', pick=lambda c: c['blocks'] > 30, min_changed=.3)
        if not args.dry: inp.click(*target)
    else:
        s = sni(run); call = s.pop('call')
        rep['dbusmenu'] = call('call', s['ours'][0], '/Menu', 'com.canonical.dbusmenu', 'Event', 'isvu', '12', 'clicked', 's', '', '0') if s['ours'] else 'no item'
    rep['allGone'] = bool(run.wait(lambda: not leftovers(run)['processes'], 40))
    time.sleep(2); rep['after'] = leftovers(run)
    out(run.record('quit', rep))

def cleanup(args):
    run = Run(args.name); rep = {'removed': []}
    pid = run.meta['pids'].get('fixture')
    if pid and Path(f'/proc/{pid}').exists(): os.kill(pid, signal.SIGTERM); rep['fixtureStopped'] = pid
    if run.meta['pids'].get('cover'): cover(argparse.Namespace(name=args.name, action='stop', quiet=True))
    rep['leftovers'] = leftovers(run)
    for home_path, listed in run.meta['before']['homePaths'].items():
        path = Path(home_path)
        if not path.exists(): continue
        if listed is None:
            shutil.rmtree(path); rep['removed'].append(str(path)); continue
        # Pre-existing (another run's state): remove only files this run added.
        for f in sorted(path.rglob('*'), reverse=True):
            if str(f) not in listed:
                if f.is_dir() and not any(f.iterdir()): f.rmdir(); rep['removed'].append(str(f))
                elif f.is_file() or f.is_symlink(): f.unlink(); rep['removed'].append(str(f))
    if Path(run.meta['runtime']).exists(): shutil.rmtree(run.meta['runtime']); rep['removed'].append(run.meta['runtime'])
    for name in sorted(p.name for p in (DATA_HOME / 'applications').glob('putgd*.desktop')):
        if name not in run.meta['before']['applications']:
            (DATA_HOME / 'applications' / name).unlink(); rep['removed'].append(name)
    real = run.meta.get('realAutostart')
    if real and Path(real).exists(): Path(real).unlink(); rep['removed'].append(real)
    for f in run.meta.get('userFiles', []):
        if Path(f).exists(): Path(f).unlink(); rep['removed'].append(f)
    if args.appimage and Path(run.meta['appimage']).exists():
        Path(run.meta['appimage']).unlink(); rep['removed'].append(run.meta['appimage'])
    for sub in ('appdir', 'frames', 'config', 'cache', 'archive', 'working', 'urldata'):
        p = run.dir / sub
        if p.exists(): shutil.rmtree(p); rep['removed'].append(str(p))
    if args.state and (run.dir / 'state').exists(): shutil.rmtree(run.dir / 'state'); rep['removed'].append(str(run.dir / 'state'))
    if (run.dir / 'cover.in').exists(): (run.dir / 'cover.in').unlink()
    rep['after'] = leftovers(run)
    out(run.record('cleanup', rep))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('identity'); a.add_argument('appimage'); a.add_argument('sums'); a.add_argument('tag'); a.add_argument('--save')
    a = sub.add_parser('versions'); a.add_argument('--save')
    a = sub.add_parser('setup'); a.add_argument('name'); a.add_argument('--appimage', required=True); a.add_argument('--vnc-port', type=int, required=True)
    a.add_argument('--input', choices=('uinput', 'xdotool', 'vnc'), required=True); a.add_argument('--allow-data-home', action='store_true')
    for n in ('start', 'memory', 'tray-menu', 'history', 'delete', 'settings', 'cleanup'):
        a = sub.add_parser(n); a.add_argument('name')
        if n == 'memory': a.add_argument('--seconds', type=int, default=180)
        if n == 'cleanup': a.add_argument('--appimage', action='store_true'); a.add_argument('--state', action='store_true')
    a = sub.add_parser('processes'); a.add_argument('name'); a.add_argument('--key')
    a = sub.add_parser('baseline'); a.add_argument('name'); a.add_argument('--label', required=True)
    a = sub.add_parser('shot'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--base', required=True)
    a.add_argument('--min-blocks', type=int, default=300); a.add_argument('--theme', action='store_true'); a.add_argument('--area'); a.add_argument('--min-changed', type=float, default=.5)
    a.add_argument('--no-park', action='store_true')
    a = sub.add_parser('click'); a.add_argument('name'); a.add_argument('--x', type=int); a.add_argument('--y', type=int); a.add_argument('--button', default='left')
    a.add_argument('--key'); a.add_argument('--drag'); a.add_argument('--type'); a.add_argument('--park', action='store_true')
    a = sub.add_parser('tray-click'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('x', type=int); a.add_argument('y', type=int)
    a.add_argument('--button', default='right'); a.add_argument('--select', type=int); a.add_argument('--activate', action='store_true'); a.add_argument('--theme', action='store_true')
    a = sub.add_parser('find-tray'); a.add_argument('name')
    a = sub.add_parser('allow-full'); a.add_argument('name'); a.add_argument('state', choices=('on', 'off'))
    a = sub.add_parser('window'); a.add_argument('name'); a.add_argument('--label', default='settings'); a.add_argument('--base', default='magenta')
    a.add_argument('--wait', type=float, default=3.5); a.add_argument('--titlebar', type=int, default=56); a.add_argument('--switch-wait', type=float, default=1.5)
    a = sub.add_parser('switcher'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--wait', type=float, default=1.2)
    a = sub.add_parser('cover'); a.add_argument('name'); a.add_argument('action', choices=('start', 'stop')); a.add_argument('--pattern', action='store_true')
    a.add_argument('--output'); a.add_argument('--video-size', default='640x360'); a.add_argument('--precheck', action='store_true')
    a = sub.add_parser('capture'); a.add_argument('name'); a.add_argument('kind', choices=('region', 'window', 'desktop'))
    a.add_argument('--drag', default='200,200,700,520'); a.add_argument('--focus'); a.add_argument('--selector-wait', type=float, default=2.5)
    a = sub.add_parser('edit'); a.add_argument('name'); a.add_argument('--source', default='region'); a.add_argument('--dialog', action='store_true')
    a.add_argument('--no-save-api', action='store_true'); a.add_argument('--notify', action='store_true')
    a = sub.add_parser('notify'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--source', default='region'); a.add_argument('--no-click', action='store_true')
    a = sub.add_parser('record'); a.add_argument('name'); a.add_argument('--rect', required=True); a.add_argument('--pause-at', type=float, default=5)
    a.add_argument('--pause-for', type=float, default=2); a.add_argument('--after', type=float, default=5); a.add_argument('--prepare-timeout', type=float, default=120)
    a.add_argument('--drop-cover', action='store_true')
    a = sub.add_parser('control'); a.add_argument('name'); a.add_argument('--rect', required=True); a.add_argument('--seconds', type=float, default=6)
    a = sub.add_parser('update-check'); a.add_argument('name'); a.add_argument('--tool', required=True)
    a = sub.add_parser('login'); a.add_argument('name'); a.add_argument('action', choices=('enable', 'check', 'disable'))
    a = sub.add_parser('quit-tray'); a.add_argument('name'); a.add_argument('--x', type=int); a.add_argument('--y', type=int); a.add_argument('--button', default='right')
    a.add_argument('--inset', type=int, default=14); a.add_argument('--dry', action='store_true')
    args = p.parse_args()
    {'identity': identity, 'versions': versions, 'setup': setup, 'start': start, 'memory': memory, 'tray-menu': tray_menu, 'history': history,
     'delete': delete, 'settings': settings, 'cleanup': cleanup, 'processes': processes, 'baseline': baseline, 'shot': shot, 'click': click,
     'tray-click': tray_click, 'switcher': switcher, 'find-tray': find_tray, 'allow-full': allow_full, 'window': window, 'cover': cover, 'capture': capture, 'edit': edit, 'notify': notify, 'record': record,
     'control': control, 'update-check': update_check, 'login': login, 'quit-tray': quit_tray}[args.cmd](args)


if __name__ == '__main__':
    main()
