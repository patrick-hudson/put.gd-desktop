#!/usr/bin/env python3
"""#259 lab run on a physical desktop: the review window's Save copy… chooser, frames and memory.

Run on the host from a plain SSH shell while holding its desktop lock:

  lab259.py --app BUILD_DIR --run RUN_DIR --label LABEL [--fallback] [--frames --vnc-port N]
            [--idle 120] [--record-idle 60] [--dialog-idle 30] [--rect X,Y,W,H]

BUILD_DIR holds putgd, putgd-cli and putgd-record, or is a put.gd AppImage. The resident runs with
private XDG config/data/cache/state, runtime and HOME directories under RUN_DIR
(HOME holds three synthetic files, so the chooser lists nothing of the host
user's), a local-only profile, notifications off and Appearance set to Dark.
With --frames a magenta cover (cover_player.py) fills the output first; every
kept picture is cropped to the non-cover area and kept only if everything
outside the crop and its edge strips are cover colour. Full frames are never
written. Memory: RSS from statm once a second, Pss/Private from smaps_rollup
at each window's end, and the TLS, GTK, KDE and Qt platform-theme libraries
each process maps.
"""
import argparse, hashlib, json, os, re, signal, statistics, subprocess, sys, time
from pathlib import Path

LAB = Path(__file__).resolve().parent
sys.path.insert(0, str(LAB))
import rfb  # noqa: E402
import cover_player  # noqa: E402

LIBRARY_GROUPS = {
    'tls': r'libQt6Network|libssl|libcrypto|libqopensslbackend|libgnutls',
    'gtk': r'libgtk-[34]|libgdk-3|libgtk4|libqgtk3|libadwaita',
    'kde': r'libKF[56]|KDEPlasmaPlatformTheme|plasma-integration|libQt6Quick|libLLVM|libgallium|dri/',
    'platformTheme': r'/platformthemes/',
}
MAGENTA = (255, 0, 255)


def session_environment():
    for name in ('cinnamon', 'plasmashell', 'gnome-shell'):
        found = subprocess.run(['pgrep', '-u', str(os.getuid()), '-x', name], capture_output=True, text=True).stdout.split()
        if found:
            raw = Path(f'/proc/{found[0]}/environ').read_bytes().split(b'\0')
            values = dict(item.decode(errors='replace').split('=', 1) for item in raw if b'=' in item)
            keep = {k: values[k] for k in ('DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION') if k in values}
            if name == 'gnome-shell' and 'WAYLAND_DISPLAY' not in keep:
                keep['WAYLAND_DISPLAY'] = 'wayland-0'  # gnome-shell's own environ lacks it
            keep.setdefault('XDG_SESSION_TYPE', 'wayland' if 'WAYLAND_DISPLAY' in keep else 'x11')
            return name, keep
    raise RuntimeError('No cinnamon, plasmashell or gnome-shell session for this user')


def rollup(pid):
    values = {}
    for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
        name, value = line.split(':', 1)
        values[name] = int(value.split()[0]) * 1024
    return {key: values.get(key) for key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty', 'Anonymous')}


def libraries(pid):
    groups = {key: set() for key in LIBRARY_GROUPS}
    for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
        parts = line.split()
        if len(parts) < 6:
            continue
        for key, pattern in LIBRARY_GROUPS.items():
            if re.search(pattern, parts[-1]):
                groups[key].add(Path(parts[-1]).name)
    return {key: sorted(value) for key, value in groups.items()}


class Run:
    def __init__(self, args):
        self.args = args
        self.root = Path(args.run).resolve()
        self.root.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.desktop, session = session_environment()
        self.env = {k: v for k, v in os.environ.items() if k in ('PATH', 'LANG', 'LC_ALL', 'USER', 'LOGNAME', 'SHELL', 'TERM')}
        self.env.update(session)
        uid = os.getuid()
        if 'WAYLAND_DISPLAY' in self.env and not self.env['WAYLAND_DISPLAY'].startswith('/'):
            self.env['WAYLAND_DISPLAY'] = f'/run/user/{uid}/' + self.env['WAYLAND_DISPLAY']
        self.runtime = Path(subprocess.check_output(['mktemp', '-d', '/tmp/pg259.XXXXXX'], text=True).strip())
        self.cleanup = {'runtime': str(self.runtime), 'pids': []}
        (self.root / 'cleanup.json').write_text(json.dumps(self.cleanup))
        for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state'), ('HOME', 'home')]:
            path = self.root / name
            path.mkdir(mode=0o700)
            self.env[key] = str(path)
        for name in ('synthetic-notes.txt', 'synthetic-clip.mp4', 'synthetic-image.png'):
            (self.root / 'home' / name).write_bytes(b'put.gd #259 lab file\n')
        self.env['XDG_RUNTIME_DIR'] = str(self.runtime)
        self.env['PIPEWIRE_RUNTIME_DIR'] = f'/run/user/{uid}'
        self.env['PULSE_RUNTIME_PATH'] = f'/run/user/{uid}/pulse'
        if args.fallback:
            self.env['PUTGD_FILE_DIALOG'] = 'fallback'
        self.app = Path(args.app).resolve()
        if self.app.suffix == '.AppImage':
            # An AppImage runs as itself; its extracted putgd-cli talks to it (as record_run.py does).
            appdir = self.root / 'appdir'
            appdir.mkdir()
            subprocess.run([str(self.app), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
            self.bindir = appdir / 'squashfs-root/usr/bin'
            self.launcher = [str(self.app), '--background']
            binaries = {'appimage': hashlib.sha256(self.app.read_bytes()).hexdigest()}
        else:
            self.bindir = self.app
            self.launcher = [str(self.app / 'putgd'), '--background']
            binaries = {name: hashlib.sha256((self.app / name).read_bytes()).hexdigest() for name in ('putgd', 'putgd-ui', 'putgd-record', 'putgd-cli')}
        self.cli = str(self.bindir / 'putgd-cli')
        self.rpid = None
        self.resident = None
        self.cover = None
        self.result = {'schemaVersion': 1, 'label': args.label, 'desktop': self.desktop, 'session': {k: session[k] for k in ('XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE') if k in session},
                       'fallbackForced': args.fallback, 'host': os.uname().nodename, 'kernel': os.uname().release,
                       'app': str(self.app), 'binaries': binaries,
                       'version': json.loads(subprocess.check_output([self.cli, '--version-json']))['version'], 'windows': [], 'events': []}
        socket_path = self.runtime / 'putgd' / 'session.sock'
        if len(str(socket_path).encode()) > 107:
            raise RuntimeError('socket path too long')

    def log(self, **event):
        event['t'] = round(time.monotonic() - self.started, 2)
        self.result['events'].append(event)
        print(json.dumps(event), flush=True)

    def request(self, method, params=None, timeout=60):
        output = subprocess.run([self.cli, method, json.dumps(params or {})], env=self.env, timeout=timeout, capture_output=True).stdout
        return json.loads(output)

    def ready(self):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if self.resident.poll() is not None:
                raise RuntimeError('Resident exited before readiness')
            try:
                reply = self.request('diagnostics', timeout=10)
                if reply['ok'] and reply['result']['journalReady']:
                    return
            except Exception:
                pass
            time.sleep(.3)
        raise RuntimeError('Resident readiness timeout')

    def start_resident(self, platform=None):
        env = dict(self.env)
        if platform:
            env['QT_QPA_PLATFORM'] = platform
        log = (self.root / 'resident.log').open('ab')
        self.resident = subprocess.Popen(self.launcher, env=env, stdout=log, stderr=log)
        self.cleanup['pids'].append(self.resident.pid)
        self.ready()
        # The resident itself: the launcher, or putgd under the AppImage runtime.
        self.rpid = self.resident.pid if self.comm(self.resident.pid) == 'putgd' else self.descendant(self.resident.pid, 'putgd')
        if not self.rpid:
            raise RuntimeError('no putgd process under the launcher')
        self.cleanup['pids'].append(self.rpid)
        (self.root / 'cleanup.json').write_text(json.dumps(self.cleanup))

    def stop_resident(self):
        if self.rpid and self.resident and self.rpid != self.resident.pid and self.comm(self.rpid) == 'putgd':
            os.kill(self.rpid, signal.SIGTERM)
            for _ in range(100):
                if self.comm(self.rpid) != 'putgd':
                    break
                time.sleep(.2)
        if self.resident and self.resident.poll() is None:
            self.resident.terminate()
            try:
                self.resident.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.resident.kill(); self.resident.wait()

    @staticmethod
    def comm(pid):
        try:
            return Path(f'/proc/{pid}/comm').read_text().strip()
        except FileNotFoundError:
            return None

    def descendant(self, root, name):
        pending = [root]
        while pending:
            pid = pending.pop()
            for child in subprocess.run(['pgrep', '-P', str(pid)], capture_output=True, text=True).stdout.split():
                if self.comm(child) == name:
                    return int(child)
                pending.append(child)
        return None

    def helper(self):
        return self.descendant(self.rpid, 'putgd-record')

    def window(self, name, seconds, pids):
        page = os.sysconf('SC_PAGE_SIZE')
        samples = {label: [] for label in pids}
        start = time.monotonic()
        while time.monotonic() - start < seconds:
            for label, pid in pids.items():
                samples[label].append(int(Path(f'/proc/{pid}/statm').read_text().split()[1]) * page)
            time.sleep(1)
        entry = {'window': name, 'seconds': seconds, 'loadavg': os.getloadavg()}
        for label, pid in pids.items():
            values = samples[label]
            entry[label] = {'pid': pid, 'medianRssMiB': round(statistics.median(values) / 2**20, 3), 'peakRssMiB': round(max(values) / 2**20, 3),
                            'smapsRollup': rollup(pid), 'PssMiB': round(rollup(pid)['Pss'] / 2**20, 3), 'libraries': libraries(pid)}
        self.result['windows'].append(entry)
        self.log(window=name, **{label: {k: entry[label][k] for k in ('medianRssMiB', 'PssMiB')} for label in pids})
        return entry

    def frame(self, name):
        """Crop the frame to what is not cover colour; keep it only if the rest is cover."""
        client = rfb.RFB(self.args.vnc_port)
        try:
            width, height, bgrx = client.frame()
        finally:
            client.close()
        def magenta(x, y):
            i = (y * width + x) * 4
            return abs(bgrx[i + 2] - 255) <= 40 and bgrx[i + 1] <= 40 and abs(bgrx[i] - 255) <= 40
        # A desktop panel can rise above the cover once another window is focused.
        # Full-width bands at the top or bottom edge are left out (never kept).
        def band_row(y):
            return sum(not magenta(x, y) for x in range(0, width, 8)) >= 0.9 * (width // 8)
        top = 0
        while top < height // 8 and band_row(top):
            top += 1
        bottom = height
        while bottom > height - height // 8 and band_row(bottom - 1):
            bottom -= 1
        rows = [0] * height; cols = [0] * width
        for y in range(top, bottom, 2):
            for x in range(0, width, 2):
                if not magenta(x, y):
                    rows[y] += 1; cols[x] += 1
        ys = [y for y in range(height) if rows[y] > 3]; xs = [x for x in range(width) if cols[x] > 3]
        if not ys or not xs:
            self.log(frame=name, kept=False, reason='nothing but cover'); return None
        margin = 18
        x0, y0 = max(0, min(xs) - margin), max(top, min(ys) - margin)
        x1, y1 = min(width, max(xs) + margin + 2), min(bottom, max(ys) + margin + 2)
        outside = total = 0
        for y in range(top, bottom, 3):
            for x in range(0, width, 3):
                if x0 <= x < x1 and y0 <= y < y1:
                    continue
                total += 1; outside += magenta(x, y)
        depth = 6
        edge = edge_total = 0
        for y in range(y0, y1, 2):
            for x in list(range(x0, x0 + depth)) + list(range(x1 - depth, x1)):
                edge_total += 1; edge += magenta(x, y)
        for x in range(x0, x1, 2):
            for y in list(range(y0, y0 + depth)) + list(range(y1 - depth, y1)):
                edge_total += 1; edge += magenta(x, y)
        outside_fraction = outside / max(1, total); edge_fraction = edge / max(1, edge_total)
        def runs(values, step):
            found, start = [], None
            for i, v in enumerate(values):
                if v > 3 and start is None:
                    start = i
                if v <= 3 and start is not None:
                    if i - start > step:
                        found.append([start, i])
                    start = None
            if start is not None:
                found.append([start, len(values)])
            return found[:12]
        geometry = {'columns': runs(cols, 4), 'rows': runs(rows, 4)}
        kept = outside_fraction >= 0.995 and edge_fraction >= 0.97
        entry = {'frame': name, 'kept': kept, 'size': [width, height], 'crop': [x0, y0, x1 - x0, y1 - y0], 'panelRows': {'top': top, 'bottom': height - bottom}, 'nonCover': geometry,
                 'outsideCoverFraction': round(outside_fraction, 4), 'edgeCoverFraction': round(edge_fraction, 4)}
        if kept:
            path = self.root / 'frames' / f'{name}.png'
            path.parent.mkdir(exist_ok=True)
            rgb = bytearray(width * height * 3)
            rgb[0::3] = bgrx[2::4]; rgb[1::3] = bgrx[1::4]; rgb[2::3] = bgrx[0::4]
            cover_player.write_png_crop(path, width, rgb, (x0, y0, x1 - x0, y1 - y0))
            entry['path'] = str(path)
        del bgrx
        self.log(**entry)
        return entry

    def start_cover(self):
        env = dict(self.env); env['HOME'] = os.environ['HOME']
        self.cover = subprocess.Popen([sys.executable, str(LAB / 'cover_player.py'), 'play'], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.cleanup['pids'].append(self.cover.pid)
        (self.root / 'cleanup.json').write_text(json.dumps(self.cleanup))
        line = self.cover.stdout.readline()
        state = json.loads(line or '{}')
        if state.get('state') != 'covered' or not state.get('ok'):
            raise RuntimeError(f'cover not placed: {line.strip()}')
        time.sleep(1.5)
        check = subprocess.run([sys.executable, str(LAB / 'cover_player.py'), 'precheck', '--port', str(self.args.vnc_port)], env=env, capture_output=True, text=True)
        self.log(cover=state, precheck=check.stdout.strip()[-300:], precheckExit=check.returncode)
        if check.returncode != 0:
            raise RuntimeError('cover precheck failed')

    def stop_cover(self):
        if self.cover and self.cover.poll() is None:
            try:
                self.cover.stdin.write('quit\n'); self.cover.stdin.flush(); self.cover.wait(timeout=10)
            except Exception:
                self.cover.terminate(); self.cover.wait(timeout=10)

    def center(self, pid):
        # X11 only: centre the review, so the chooser centred on it stays inside the cover.
        env = dict(self.env)
        found = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(pid)], env=env, capture_output=True, text=True).stdout.split()
        for window in found:
            geometry = subprocess.run(['xdotool', 'getwindowgeometry', '--shell', window], env=env, capture_output=True, text=True).stdout
            values = dict(line.split('=', 1) for line in geometry.split() if '=' in line)
            w, h = int(values.get('WIDTH', 0)), int(values.get('HEIGHT', 0))
            if w > 400 and h > 300:
                subprocess.run(['xdotool', 'windowmove', window, str((1920 - w) // 2), str((1080 - h) // 2)], env=env)
                self.log(centered=[(1920 - w) // 2, (1080 - h) // 2, w, h])
                time.sleep(1.5)
                return

    def review(self, op, action, **extra):
        return self.request('recording.review', {'id': op, 'action': action, **extra})

    def main(self):
        self.started = time.monotonic()
        a = self.args
        try:
            # Configure offscreen first: a local-only profile with no shortcuts or notifications, Dark.
            self.start_resident('offscreen')
            document = self.request('settings.developmentTemplate')['result']['document']
            document['profiles'] = []
            document['actions'] = {action: {'destination': 'local', 'profileId': None, 'shortcut': ''} for action in document['actions']}
            document['archive']['root'] = str(self.root / 'archive')
            document['archive']['keepUploads'] = True
            document['preferences'] = {'notifications': False, 'launchAtLogin': False, 'appearance': {'theme': a.theme}}
            revision = self.request('settings.get')['result']['metadata']['observedRevision']
            patched = self.request('settings.patch', {'patch': document, 'expectedRevision': revision})
            if not patched['ok']:
                raise RuntimeError(f'settings.patch: {patched["error"]}')
            self.stop_resident()
            if a.frames:
                self.start_cover()
            self.start_resident()
            time.sleep(5)
            self.result['loadBefore'] = os.getloadavg()
            self.window('resident-idle-before', a.idle, {'resident': self.rpid})
            x, y, w, h = a.rect
            prepared = self.request('recording.prepare', {'target': {'kind': 'region', 'rect': [x, y, w, h]}, 'format': 'mp4', 'audio': {'system': False}}, timeout=130)
            self.log(prepare={'ok': prepared['ok'], 'error': prepared.get('error', {}).get('code'), 'acquisition': (prepared.get('result') or {}).get('acquisition')})
            if not prepared['ok']:
                raise RuntimeError(f'prepare: {prepared["error"]}')
            op = prepared['result']['operationId']
            self.result['operationId'] = op
            for _ in range(240):
                state = self.request('recording.inspect', {'id': op})['result'].get('state')
                if state != 'preparing':
                    break
                time.sleep(.5)
            self.log(prepared=state)
            if state != 'ready':
                raise RuntimeError(f'prepare ended in {state}')
            for command in ('start',):
                reply = self.request(f'recording.{command}', {'id': op})
                if not reply['ok']:
                    raise RuntimeError(f'{command}: {reply["error"]}')
            time.sleep(3)
            stopped = self.request('recording.stop', {'id': op}, timeout=70)
            if not stopped['ok']:
                raise RuntimeError(f'stop: {stopped["error"]}')
            for _ in range(60):
                state = self.request('recording.inspect', {'id': op})['result'].get('state')
                if state == 'reviewing':
                    break
                time.sleep(.5)
            helper = self.helper()
            self.log(reviewing=state, helper=helper)
            if state != 'reviewing' or not helper:
                raise RuntimeError('review window did not open')
            time.sleep(3)
            inspected = self.review(op, 'inspect')['result']
            self.result['reviewBefore'] = {k: inspected.get(k) for k in ('visible', 'size', 'scheme', 'fileDialog')}
            self.window('review-open', a.record_idle, {'resident': self.rpid, 'record': helper})
            if self.env.get('XDG_SESSION_TYPE') == 'x11':
                self.center(helper)
            if a.frames:
                self.frame('review')
            opened = self.review(op, 'saveCopyDialog')
            self.result['saveCopyDialog'] = opened['result'].get('fileDialog') if opened['ok'] else opened['error']
            self.log(saveCopyDialog=self.result['saveCopyDialog'])
            time.sleep(a.chooser_wait)
            self.result['dialogState'] = self.review(op, 'inspect')['result'].get('fileDialog')
            self.window('save-copy-dialog-open', a.dialog_idle, {'resident': self.rpid, 'record': helper})
            if a.frames:
                self.frame('chooser')
            closed = self.review(op, 'closeFileDialog')
            self.result['closed'] = closed['result'].get('fileDialog') if closed['ok'] else closed['error']
            time.sleep(4)
            self.window('after-dialog-closed', a.dialog_idle, {'resident': self.rpid, 'record': helper})
            self.request('recording.cancel', {'id': op})
            time.sleep(8)
            self.result['childrenAfterCancel'] = self.helper()
            self.window('resident-idle-after', a.idle, {'resident': self.rpid})
            self.result['loadAfter'] = os.getloadavg()
        finally:
            self.stop_resident()
            self.stop_cover()
            subprocess.run(['rm', '-rf', str(self.runtime)])
            (self.root / 'results.json').write_text(json.dumps(self.result, indent=2) + '\n')
        return 0


def rect(text):
    values = [int(v) for v in text.split(',')]
    if len(values) != 4:
        raise argparse.ArgumentTypeError('X,Y,W,H')
    return values


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('--app', required=True); p.add_argument('--run', required=True); p.add_argument('--label', required=True)
    p.add_argument('--fallback', action='store_true'); p.add_argument('--frames', action='store_true')
    p.add_argument('--theme', choices=('dark', 'light', 'system'), default='dark')
    p.add_argument('--vnc-port', type=int, default=5900)
    p.add_argument('--idle', type=int, default=120); p.add_argument('--record-idle', type=int, default=60)
    p.add_argument('--dialog-idle', type=int, default=30); p.add_argument('--chooser-wait', type=int, default=6)
    p.add_argument('--rect', type=rect, default=[200, 200, 640, 360])
    sys.exit(Run(p.parse_args()).main())
