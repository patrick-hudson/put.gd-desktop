#!/usr/bin/env python3
"""#283 review-window lab driver for a physical desktop (stdlib only; run on the host from SSH).

Everything put.gd writes lives under the run ROOT (config, data, state, cache,
runtime, archive). The screen shows only the lab's own pixels: a magenta cover
pinned to the output (cover_player.py, precheck on the loopback VNC tile) with
a dark test card in its middle (put.gd's dark surface colour, a bouncing ball
and a small scrolling SMPTE strip), put.gd's review window above it.

  r283lab.py capture --build DIR --label L --scheme dark|light --root ROOT --out DIR [--before]
      record 6 s of the test card, then capture the review window's client area
      empty and with two masks (and, without --before, a third view with the
      blur selected and the playhead moved), each crop checked on the host.
  r283lab.py e2e-start --build DIR --root ROOT [--still]
      S3 fixture on loopback + resident with the fixture profile (session keys),
      cover and test card; prints {socket, port, region}. The MCP client drives it.
  r283lab.py e2e-stop --root ROOT
  r283lab.py motion --build DIR CLIP
"""
import argparse, json, os, re, signal, socket, struct, subprocess, sys, time, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import rfb  # noqa: E402

ACCESS, SECRET = 'PUTGD_SYNTHETIC_ACCESS', 'putgd-synthetic-secret-not-a-real-key'  # the fixture's own synthetic pair
SESSION_VARIABLES = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP',
                     'WAYLAND_DISPLAY', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION')
# Theme.cpp's two palettes, plus FrameView's fixed surround (#0d0f13).
DARK = ['111317', '181B21', '20242C', '232838', '2A2F38', '343A45', 'E7E9EE', 'C4C9D2', '9AA1AD', '6F7682', '6EA8FE',
        '8DBBFF', '4CC38A', 'E5A443', 'F07A6A', 'F4A497', '1C3328', '3A3020', '3A2326', '9BBCFF', '515F82']
LIGHT = ['F5F6F8', 'FFFFFF', 'EDEFF3', 'DCE7FA', 'D5D9E0', 'AEB5C1', '1B1E24', '3A414D', '585F6B', '8B929D', '1F64D6',
         '1753B5', '1754BD', '146C42', '8A5300', 'B4312A', '96271F', 'E3F4EA', 'FBF0DC', 'FBE7E5', '2759B8', 'B4C8EE']
OWN = [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in DARK + LIGHT + ['0D0F13']]
BG = {'dark': (0x11, 0x13, 0x17), 'light': (0xF5, 0xF6, 0xF8)}
CARD = (1280, 720)
# The test card: put.gd's dark surface, a bouncing ball in the brand blue, and a
# 480x270 scrolling SMPTE strip at (400, 225) for the masks to cover.
CARD_PIPE = ('compositor name=mix background=black sink_1::xpos=400 sink_1::ypos=225 '
             '! video/x-raw,width=1280,height=720,framerate=30/1 ! videoconvert ! gtksink name=video '
             'videotestsrc is-live=true %s '
             '! video/x-raw,width=1280,height=720,framerate=30/1 ! mix.sink_0 '
             'videotestsrc is-live=true pattern=smpte75 horizontal-speed=%d '
             '! video/x-raw,width=480,height=270,framerate=30/1 ! mix.sink_1')
MASKS = [{'kind': 'blur', 'rect': [380, 205, 260, 310], 'start': 0, 'end': 4000000, 'strength': 16},
         {'kind': 'solid', 'rect': [660, 250, 240, 140], 'start': 0, 'end': 5500000}]


def log(message):
    print(time.strftime('%H:%M:%S ') + message, file=sys.stderr, flush=True)


def session_environment():
    uid = os.getuid()
    for process in ('plasmashell', 'gnome-shell', 'cinnamon'):
        pids = subprocess.run(['pgrep', '-u', str(uid), '-x', process], capture_output=True, text=True).stdout.split()
        if pids:
            break
    else:
        raise SystemExit('no desktop session process for this user')
    pairs = Path('/proc/%s/environ' % pids[0]).read_bytes().decode(errors='replace').split('\0')
    found = dict(pair.split('=', 1) for pair in pairs if '=' in pair)
    runtime = '/run/user/%d' % uid
    env = {'HOME': os.environ['HOME'], 'PATH': os.environ.get('PATH', '/usr/bin:/bin'), 'LANG': found.get('LANG', 'C.UTF-8'),
           'USER': os.environ.get('USER', ''), 'XDG_RUNTIME_DIR': runtime}
    env.update({name: found[name] for name in SESSION_VARIABLES if found.get(name)})
    if env.get('XDG_SESSION_TYPE') == 'wayland' and not env.get('WAYLAND_DISPLAY'):
        shown = subprocess.run(['systemctl', '--user', 'show-environment'], capture_output=True, text=True).stdout
        env.update(line.split('=', 1) for line in shown.splitlines() if line.startswith('WAYLAND_DISPLAY='))
    if env.get('WAYLAND_DISPLAY') and not env['WAYLAND_DISPLAY'].startswith('/'):
        env['WAYLAND_DISPLAY'] = runtime + '/' + env['WAYLAND_DISPLAY']
    env['QT_QPA_PLATFORM'] = 'wayland' if env.get('XDG_SESSION_TYPE') == 'wayland' else 'xcb'
    return env, process


class Lab:
    def __init__(self, root, build):
        self.root = Path(root).resolve()
        for sub in ('c', 'd', 's', 'cache', 'r', 'archive', 'out', 'logs', 'working'):
            (self.root / sub).mkdir(parents=True, exist_ok=True)
        os.chmod(self.root, 0o700); os.chmod(self.root / 'r', 0o700)
        self.session, self.desktop = session_environment()
        self.env = dict(self.session, XDG_CONFIG_HOME=str(self.root / 'c'), XDG_DATA_HOME=str(self.root / 'd'),
                        XDG_STATE_HOME=str(self.root / 's'), XDG_CACHE_HOME=str(self.root / 'cache'),
                        XDG_RUNTIME_DIR=str(self.root / 'r'), PIPEWIRE_RUNTIME_DIR=self.session['XDG_RUNTIME_DIR'],
                        PUTGD_RECORD_KEEP_WORKING=str(self.root / 'working'))
        self.build = Path(build).resolve() if build else None
        # An AppImage: its own launcher, the extracted putgd-cli.
        self.launcher = [str(self.build / 'putgd'), '--background'] if self.build else None
        self.cli_path = str(self.build / 'putgd-cli') if self.build else None
        if self.build and self.build.suffix == '.AppImage':
            appdir = self.build.parent / ('appdir-' + self.build.stem)
            if not (appdir / 'squashfs-root').is_dir():
                appdir.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(self.build), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
            self.launcher = [str(self.build), '--background']
            self.cli_path = str(appdir / 'squashfs-root/usr/bin/putgd-cli')
        self.state_path = self.root / 'run.json'
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {'pids': {}}

    def save(self):
        self.state_path.write_text(json.dumps(self.state, indent=1))

    # -- put.gd -------------------------------------------------------------
    def cli(self, method, params=None, timeout=60, ok=True):
        out = subprocess.run([self.cli_path, method, json.dumps(params or {})], env=self.env,
                             capture_output=True, text=True, timeout=timeout)
        try:
            value = json.loads(out.stdout)
        except ValueError:
            raise SystemExit('%s: no JSON (exit %d) %s' % (method, out.returncode, out.stderr[-300:]))
        if ok and not value.get('ok'):
            raise SystemExit('%s: %s' % (method, json.dumps(value.get('error'))))
        return value.get('result') if ok else value

    def socket_request(self, method, params):
        path = str(self.root / 'r/putgd/session.sock')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(15); s.connect(path)
            s.sendall(json.dumps({'schemaVersion': 1, 'requestId': 'r283-' + method, 'session': path,
                                  'method': method, 'params': params}).encode() + b'\n')
            data = b''
            while not data.endswith(b'\n'):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
        return json.loads(data)

    def profile(self, scheme, port=None):
        """A private configuration from the development template, then a restart so no window opens by itself."""
        self.start_resident()
        template = self.cli('settings.developmentTemplate')['document']
        self.write_config(template, scheme, port)
        self.cli('settings.reload')
        for _ in range(100):
            snap = self.cli('settings.get', ok=False)
            if snap.get('ok') and snap['result']['metadata']['validation']['ok']:
                break
            time.sleep(0.2)
        else:
            raise SystemExit('configuration not accepted: %s' % json.dumps(snap)[:600])
        self.stop_resident()
        self.start_resident()

    def write_config(self, template, scheme, port=None):
        config = dict(template)
        config.update({'schemaVersion': 1,
                  'actions': {name: {'destination': 'upload', 'profileId': 'r283-fixture', 'shortcut': ''}
                              for name in ('clipboard', 'desktop', 'edit', 'region', 'window')} | {'record': {'shortcut': ''}},
                  'archive': {'folders': {'images': None, 'other': None, 'text': None, 'videos': None}, 'keepUploads': False,
                              'root': str(self.root / 'archive')},
                  'preferences': {'launchAtLogin': False, 'notifications': False, 'uploadClipboardBehavior': 'list',
                                  'unlockCredentialsAtStartup': False, 'appearance': {'theme': scheme}},
                  'profiles': [{'credentials': {'mode': 'session', 'reference': None}, 'id': 'r283-fixture',
                                'label': 'Synthetic fixture', 'provider': 's3',
                                'settings': {'aclMode': 'public-read', 'addressing': 'path', 'bucket': 'fixture',
                                             'connectTimeoutMs': 5000, 'endpoint': 'http://127.0.0.1:%d' % (port or 9),
                                             'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 120, 'objectPrefix': 'r283/',
                                             'publicUrlBase': 'http://127.0.0.1:%d/fixture' % (port or 9), 'region': 'us-east-1',
                                             'requestTimeoutMs': 120000, 'storageClass': 'STANDARD'}}]})
        (self.root / 'c/putgd').mkdir(parents=True, exist_ok=True)
        (self.root / 'c/putgd/config.json').write_text(json.dumps(config, indent=1))

    def start_resident(self):
        sock = self.root / 'r/putgd/session.sock'
        if sock.exists():
            raise SystemExit('%s exists' % sock)
        resident = subprocess.Popen(self.launcher, env=self.env, stdin=subprocess.DEVNULL,
                                    stdout=open(self.root / 'logs/resident.log', 'a'), stderr=subprocess.STDOUT,
                                    start_new_session=True)
        self.state['pids']['resident'] = resident.pid; self.save()
        self.resident = resident
        for _ in range(150):
            if sock.exists():
                try:
                    self.cli('recording.capabilities', timeout=40)
                    return resident.pid
                except SystemExit:
                    pass
            time.sleep(0.2)
        raise SystemExit('resident did not answer')

    def stop_resident(self):
        pid = self.state['pids'].pop('resident', None)
        if pid is None:
            return
        self.cli('ui.quit', ok=False, timeout=15)
        if getattr(self, 'resident', None) is not None and self.resident.pid == pid:
            try:
                self.resident.wait(20)
            except subprocess.TimeoutExpired:
                os.killpg(pid, signal.SIGTERM)
            self.resident = None
            self.save()
            return
        for _ in range(100):
            if not Path('/proc/%d' % pid).exists():
                break
            time.sleep(0.2)
        else:
            os.killpg(pid, signal.SIGTERM)
        self.save()

    def wait_state(self, rid, wanted, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.cli('recording.inspect', {'id': rid})
            if state['state'] in wanted:
                return state
            time.sleep(0.3)
        raise SystemExit('recording %s never reached %s' % (rid, wanted))

    # -- cover and test card --------------------------------------------------
    def open_cover(self, still=False):
        # The frozen control: the same card without the ball and with the strip standing still.
        pipe = CARD_PIPE % ('pattern=solid-color foreground-color=0xff181b21' if still else
                            'pattern=ball background-color=0xff181b21 foreground-color=0xff9bbcff motion=sweep', 0 if still else 6)
        cover = subprocess.Popen([sys.executable, str(HERE / 'cover_player_r283.py'), 'play', '--pattern', '--video-size', '%dx%d' % CARD],
                                 env=dict(self.session, GTK_USE_PORTAL='0', NO_AT_BRIDGE='1', R283_PATTERN=pipe),
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                 stderr=open(self.root / 'logs/cover.log', 'a'), start_new_session=True)
        self.state['pids']['cover'] = cover.pid; self.save()
        self.cover = cover
        placed = json.loads(cover.stdout.readline() or '{}')
        if placed.get('state') != 'covered':
            raise SystemExit('cover not placed: %s' % placed)
        time.sleep(1.5)
        checked = subprocess.run([sys.executable, str(HERE / 'cover_player_r283.py'), 'precheck', '--port', str(self.vnc_port)],
                                 env=self.session, capture_output=True, text=True, timeout=60)
        if checked.returncode != 0:
            raise SystemExit('cover precheck failed: %s' % checked.stdout)
        cover.stdin.write('media\n'); cover.stdin.flush()
        playing = json.loads(cover.stdout.readline() or '{}')
        if playing.get('state') != 'playing':
            raise SystemExit('card not playing: %s' % playing)
        x, y, w, h = placed['rect']
        # Inset so no row of the cover around the card is recorded.
        self.region = [x + (w - CARD[0]) // 2 + 4, y + (h - CARD[1]) // 2 + 4, CARD[0] - 8, CARD[1] - 8]
        self.output = placed['rect']
        return {'placed': placed, 'precheck': json.loads(checked.stdout or '{}'), 'region': self.region}

    def close_cover(self):
        pid = self.state['pids'].pop('cover', None)
        cover = getattr(self, 'cover', None)
        if cover and cover.poll() is None:
            try:
                cover.stdin.write('quit\n'); cover.stdin.flush(); cover.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                os.killpg(cover.pid, signal.SIGTERM)
        elif pid and Path('/proc/%d' % pid).exists():
            os.killpg(pid, signal.SIGTERM)
        self.save()

    # -- the review window ----------------------------------------------------
    def kwin(self, body):
        """Run a KWin script; its print() lines carrying this run's marker come back."""
        marker = 'R283-%d' % time.monotonic_ns()
        path = self.root / ('kwin-%s.js' % marker)
        path.write_text(body.replace('MARK', marker))
        since = time.strftime('%Y-%m-%d %H:%M:%S')
        name = 'r283' + marker
        out = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', '/Scripting',
                              '--method', 'org.kde.kwin.Scripting.loadScript', str(path), name], env=self.session,
                             capture_output=True, text=True, timeout=10).stdout
        number = int(re.search(r'-?\d+', out).group())
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', '/Scripting/Script%d' % number,
                        '--method', 'org.kde.kwin.Script.run'], env=self.session, capture_output=True, timeout=10)
        time.sleep(0.8)
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', '/Scripting',
                        '--method', 'org.kde.kwin.Scripting.unloadScript', name], env=self.session, capture_output=True, timeout=10)
        path.unlink()
        journal = subprocess.run(['journalctl', '--user', '--since', since, '-o', 'cat'], capture_output=True, text=True).stdout
        lines = [line.split(marker, 1)[1].strip() for line in journal.splitlines() if marker in line]
        return [json.loads(line) for line in lines]

    def place_window(self, at=(40, 40)):
        """Put the review window at `at` above everything; return its client rect and whatever overlaps it from above."""
        if self.session.get('XDG_SESSION_TYPE') == 'wayland':
            found = self.kwin('''
const list = workspace.windowList();
for (const w of list) {
  if (!w.caption.startsWith("Review recording")) continue;
  w.keepAbove = true; w.minimized = false;
  w.frameGeometry = {x: %d, y: %d, width: w.frameGeometry.width, height: w.frameGeometry.height};
  workspace.activeWindow = w;
  const c = w.clientGeometry, f = w.frameGeometry;
  const order = workspace.stackingOrder; const mine = order.indexOf(w); const above = [];
  for (let i = mine + 1; i < order.length; ++i) { const o = order[i];
    if (o.minimized || !o.onCurrentDesktop || o.deleted || !o.visible) continue;
    const g = o.frameGeometry; if (g.x < c.x + c.width && c.x < g.x + g.width && g.y < c.y + c.height && c.y < g.y + g.height) above.push(o.caption + "|" + o.resourceClass); }
  print("MARK " + JSON.stringify({client: [c.x, c.y, c.width, c.height], frame: [f.x, f.y, f.width, f.height], above: above}));
}''' % at)
            if not found:
                return None
            time.sleep(0.6)
            # Read the settled geometry once more.
            again = self.kwin('''
for (const w of workspace.windowList()) { if (!w.caption.startsWith("Review recording")) continue;
  const c = w.clientGeometry; const order = workspace.stackingOrder; const mine = order.indexOf(w); const above = [];
  for (let i = mine + 1; i < order.length; ++i) { const o = order[i]; if (o.minimized || !o.onCurrentDesktop || o.deleted || !o.visible) continue;
    const g = o.frameGeometry; if (g.x < c.x + c.width && c.x < g.x + g.width && g.y < c.y + c.height && c.y < g.y + g.height) above.push(o.caption + "|" + o.resourceClass); }
  print("MARK " + JSON.stringify({client: [c.x, c.y, c.width, c.height], above: above})); }''')
            return again[0] if again else found[0]
        ids = subprocess.run(['xdotool', 'search', '--name', '^Review recording$'], env=self.session, capture_output=True, text=True).stdout.split()
        if not ids:
            return None
        wid = ids[-1]
        for command in (['xdotool', 'windowactivate', '--sync', wid], ['wmctrl', '-i', '-r', wid, '-b', 'add,above'],
                        ['xdotool', 'windowmove', '--sync', wid, str(at[0]), str(at[1])]):
            subprocess.run(command, env=self.session, capture_output=True, timeout=10)
        time.sleep(0.6)
        return self.x11_geometry(wid)

    def x11_geometry(self, wid):
        info = subprocess.run(['xwininfo', '-id', wid], env=self.session, capture_output=True, text=True).stdout
        get = lambda key: int(re.search(key + r':\s+(-?\d+)', info).group(1))
        client = [get('Absolute upper-left X'), get('Absolute upper-left Y'), get('Width'), get('Height')]
        viewable = 'IsViewable' in info
        stacking = subprocess.run(['xprop', '-root', '_NET_CLIENT_LIST_STACKING'], env=self.session, capture_output=True, text=True).stdout
        order = [int(v, 16) for v in re.findall(r'0x[0-9a-f]+', stacking)]
        above = []
        if int(wid) in order:
            for other in order[order.index(int(wid)) + 1:]:
                o = subprocess.run(['xwininfo', '-id', str(other)], env=self.session, capture_output=True, text=True).stdout
                if 'IsViewable' not in o:
                    continue
                g = [int(re.search(k + r':\s+(-?\d+)', o).group(1)) for k in ('Absolute upper-left X', 'Absolute upper-left Y', 'Width', 'Height')]
                if g[0] < client[0] + client[2] and client[0] < g[0] + g[2] and g[1] < client[1] + client[3] and client[1] < g[1] + g[3]:
                    above.append(re.search(r'"(.*)"', o.splitlines()[1] if len(o.splitlines()) > 1 else '"?"').group(1) if '"' in o else hex(other))
        return {'client': client, 'above': above, 'viewable': viewable, 'window': wid}

    # -- frames ---------------------------------------------------------------
    def grab(self):
        client = rfb.RFB(self.vnc_port)
        try:
            client.pointer(self.output[0] + self.output[2] - 30, self.output[1] + self.output[3] - 30)
            time.sleep(0.4)
            client.frame()  # the first frame can predate the pointer move
            return client.frame()
        finally:
            client.close()


def near(pixel, colours, tolerance=7):
    r, g, b = pixel
    return any(abs(r - c[0]) <= tolerance and abs(g - c[1]) <= tolerance and abs(b - c[2]) <= tolerance for c in colours)


def crop_rgb(frame, rect):
    width, height, bgrx = frame
    x, y, w, h = rect
    if x < 0 or y < 0 or x + w > width or y + h > height:
        raise SystemExit('window %s is not inside the %dx%d frame' % (rect, width, height))
    rows = []
    for row in range(y, y + h):
        line = bgrx[(row * width + x) * 4:(row * width + x + w) * 4]
        rgb = bytearray(w * 3); rgb[0::3] = line[2::4]; rgb[1::3] = line[1::4]; rgb[2::3] = line[0::4]
        rows.append(bytes(rgb))
    return w, h, rows


def check_crop(crop, scheme):
    """Own-pixel share, magenta share and each edge strip's share of the window background."""
    w, h, rows = crop
    cache, own, magenta, total, box = {}, 0, 0, 0, None
    for y in range(0, h, 2):
        row = rows[y]
        for i in range(0, len(row), 6):
            p = (row[i], row[i + 1], row[i + 2])
            if p not in cache:
                cache[p] = near(p, OWN)
            own += cache[p]; total += 1
            if p[0] > 230 and p[1] < 40 and p[2] > 230:
                magenta += 1; x = i // 3
                box = [min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)] if box else [x, y, x, y]
    bg = [BG[scheme]]
    def strip(pixels):
        return sum(near(p, bg, 6) for p in pixels) / max(1, len(pixels))
    pix = lambda row, x: (rows[row][3 * x], rows[row][3 * x + 1], rows[row][3 * x + 2])
    edges = {'top': strip([pix(r, x) for r in range(3) for x in range(w)]),
             'bottom': strip([pix(r, x) for r in range(h - 3, h) for x in range(w)]),
             'left': strip([pix(r, x) for r in range(h) for x in range(3)]),
             'right': strip([pix(r, x) for r in range(h) for x in range(w - 3, w)])}
    return {'ownFraction': round(own / total, 4), 'magentaFraction': round(magenta / total, 5), 'magentaBox': box,
            'edgeBackground': {k: round(v, 4) for k, v in edges.items()}}


def frame_view(crop):
    """The light scheme's dark frame area (letterbox and video) inside the window, and the 16:9 picture in it."""
    w, h, rows = crop
    dark = lambda r, x: rows[r][3 * x] + rows[r][3 * x + 1] + rows[r][3 * x + 2] < 180
    # The card's bright strip spans a third of the picture's width: a frame row is still over half dark.
    rowfrac = [sum(dark(r, x) for x in range(0, w, 2)) / ((w + 1) // 2) for r in range(h)]
    band = [r for r in range(h) if rowfrac[r] > 0.5]
    if not band:
        return None
    # The longest run of frame rows.
    best, start = (0, 0), band[0]
    for a, b in zip(band, band[1:] + [None]):
        if b != a + 1:
            if a - start > best[1] - best[0]:
                best = (start, a)
            if b is not None:
                start = b
    top, bottom = best
    colfrac = [sum(dark(r, x) for r in range(top, bottom + 1, 2)) / ((bottom - top) // 2 + 1) for x in range(w)]
    cols = [x for x in range(w) if colfrac[x] > 0.9]
    left, right = cols[0], cols[-1]
    fw, fh = right - left + 1, bottom - top + 1
    scale = min(fw / CARD[0], fh / CARD[1])
    vw, vh = round(CARD[0] * scale), round(CARD[1] * scale)
    return {'frameView': [left, top, fw, fh], 'video': [left + (fw - vw) // 2, top + (fh - vh) // 2, vw, vh],
            'videoShare': round(vw * vh / (w * h), 4), 'window': [w, h]}


def save_png(crop, path):
    w, h, rows = crop
    raw = b''.join(b'\0' + row for row in rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def shoot(lab, scheme, name, out, results):
    placed = lab.place_window()
    if not placed:
        raise SystemExit('no review window')
    if placed['above']:
        raise SystemExit('windows above the review window: %s' % placed['above'])
    time.sleep(1.0)
    frame = lab.grab()
    crop = crop_rgb(frame, placed['client'])
    del frame
    check = check_crop(crop, scheme)
    ok = check['ownFraction'] >= 0.4 and check['magentaFraction'] < 0.001 and min(check['edgeBackground'].values()) >= 0.9
    entry = {'name': name, 'client': placed['client'], 'check': check, 'kept': ok}
    if ok:
        save_png(crop, Path(out) / (name + '.png'))
        if scheme == 'light':
            entry['measured'] = frame_view(crop)
    results.append(entry)
    log('%s %s' % (name, json.dumps(entry)))
    if not ok:
        raise SystemExit('crop %s failed its content check; nothing kept' % name)


def capture(a):
    lab = Lab(a.root, a.build)
    lab.vnc_port = a.vnc_port
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    results = {'host': os.uname().nodename, 'desktop': lab.desktop, 'session': lab.session.get('XDG_SESSION_TYPE'),
               'build': str(lab.build), 'label': a.label, 'scheme': a.scheme, 'shots': []}
    version = subprocess.run([lab.cli_path, '--version'], capture_output=True, text=True).stdout.strip()
    results['version'] = version
    try:
        results['cover'] = lab.open_cover()
        lab.profile(a.scheme)
        prepared = lab.cli('recording.prepare', {'target': {'kind': 'region', 'rect': lab.region}, 'format': 'mp4', 'fps': 30})
        rid = prepared['recordingId']
        ready = lab.wait_state(rid, ('ready',))
        results['acquisition'] = ready.get('progress', {}).get('acquisition')
        time.sleep(1)
        lab.cli('recording.start', {'id': rid})
        time.sleep(6.5)
        lab.cli('recording.stop', {'id': rid}, timeout=60)
        state = lab.wait_state(rid, ('reviewing',))
        results['durationUs'] = state['durationUs']
        time.sleep(2.5)
        shoot(lab, a.scheme, '%s-%s-empty' % (a.label, a.scheme), out, results['shots'])
        if not a.before:
            results['inspectEmpty'] = lab.cli('recording.review', {'id': rid, 'action': 'inspect'})
        commands = [{'op': 'addMask', 'mask': m} for m in MASKS]
        lab.cli('recording.edit', {'id': rid, 'revision': int(state['edit']['revision']), 'commands': commands})
        time.sleep(2.0)
        shoot(lab, a.scheme, '%s-%s-masks' % (a.label, a.scheme), out, results['shots'])
        if not a.before:
            results['inspectMasks'] = lab.cli('recording.review', {'id': rid, 'action': 'inspect'})
            # The contextual view: the blur selected (its strength shows), the playhead mid-clip.
            blur = next(m['id'] for m in lab.cli('recording.inspect', {'id': rid})['edit']['masks'] if m['kind'] == 'blur')
            lab.cli('recording.review', {'id': rid, 'action': 'selectMask', 'maskId': blur})
            lab.cli('recording.review', {'id': rid, 'action': 'seek', 'time': 2500000})
            time.sleep(2.0)
            shoot(lab, a.scheme, '%s-%s-selected' % (a.label, a.scheme), out, results['shots'])
            results['inspectSelected'] = lab.cli('recording.review', {'id': rid, 'action': 'inspect'})
        lab.cli('recording.cancel', {'id': rid})
    finally:
        lab.stop_resident()
        lab.close_cover()
        results['load'] = os.getloadavg()[0]
        (out / ('%s-%s.json' % (a.label, a.scheme))).write_text(json.dumps(results, indent=1))
    print(json.dumps({'ok': True, 'shots': [s['name'] for s in results['shots']]}))


def e2e_start(a):
    lab = Lab(a.root, a.build)
    lab.vnc_port = a.vnc_port
    tree = Path(a.tree)
    server = subprocess.Popen([sys.executable, str(tree / 'tests/fixtures/s3_server.py')], stdout=subprocess.PIPE,
                              stderr=open(lab.root / 'logs/fixture.log', 'w'), text=True, start_new_session=True)
    port = int(server.stdout.readline())
    lab.state['pids']['fixture'] = server.pid; lab.state['port'] = port; lab.save()
    cover = lab.open_cover(still=a.still)
    lab.state['pids']['cover'] = lab.cover.pid; lab.state['region'] = lab.region; lab.state['pids']['driver'] = os.getpid(); lab.save()
    lab.profile(a.scheme, port)
    revision = lab.cli('settings.get')['metadata']['acceptedRevision']
    stored = lab.socket_request('setup.storeCredentials', {'profileId': 'r283-fixture', 'expectedRevision': revision,
                                'credentials': {'accessKey': ACCESS, 'secretKey': SECRET}})
    if not stored.get('ok'):
        raise SystemExit('storeCredentials: %s' % json.dumps(stored.get('error')))
    for _ in range(100):
        job = lab.cli('setup.inspect', {'id': stored['result']['setupId']})
        if job['state'] != 'running':
            break
        time.sleep(0.2)
    # The cover player quits when its stdin closes: this process stays until e2e-stop asks it to end.
    print(json.dumps({'socket': str(lab.root / 'r/putgd/session.sock'), 'port': port, 'region': lab.region,
                      'credentials': job['state'], 'cover': cover['precheck']}), flush=True)
    stop = lab.root / 'stop'
    try:
        while not stop.exists():
            time.sleep(1)
    finally:
        lab.state = json.loads(lab.state_path.read_text())
        lab.stop_resident()
        lab.close_cover()
        pid = lab.state['pids'].pop('fixture', None)
        if pid and Path('/proc/%d' % pid).exists():
            os.killpg(pid, signal.SIGTERM)
        lab.state['pids'].pop('driver', None)
        lab.save()
        stop.unlink(missing_ok=True)


def e2e_stop(a):
    root = Path(a.root)
    driver = json.loads((root / 'run.json').read_text())['pids'].get('driver')
    (root / 'stop').touch()
    for _ in range(150):
        if not driver or not Path('/proc/%d' % driver).exists():
            break
        time.sleep(0.2)
    print(json.dumps({'stopped': True, 'left': json.loads((root / 'run.json').read_text())['pids']}))


def motion(a):
    out = subprocess.run([str(Path(a.build) / 'putgd-record'), '--motion', a.clip], capture_output=True, text=True, timeout=300)
    print(out.stdout.strip() or out.stderr.strip())


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest='command', required=True)
    c = sub.add_parser('capture')
    c.add_argument('--build', required=True); c.add_argument('--label', required=True); c.add_argument('--root', required=True)
    c.add_argument('--scheme', choices=('dark', 'light'), required=True); c.add_argument('--out', required=True)
    c.add_argument('--before', action='store_true'); c.add_argument('--vnc-port', type=int, default=5900)
    s = sub.add_parser('e2e-start')
    s.add_argument('--build', required=True); s.add_argument('--root', required=True); s.add_argument('--tree', required=True)
    s.add_argument('--scheme', default='dark'); s.add_argument('--still', action='store_true'); s.add_argument('--vnc-port', type=int, default=5900)
    t = sub.add_parser('e2e-stop')
    t.add_argument('--root', required=True); t.add_argument('--build', required=True)
    m = sub.add_parser('motion')
    m.add_argument('--build', required=True); m.add_argument('clip')
    a = p.parse_args()
    {'capture': capture, 'e2e-start': e2e_start, 'e2e-stop': e2e_stop, 'motion': motion}[a.command](a)


if __name__ == '__main__':
    main()
