#!/usr/bin/env python3
"""Recording runs on a physical desktop: pause/resume A/V and format/motion sweeps, builds interleaved (stdlib only).

Run it on the host from a plain SSH shell, holding the desktop's lock. It reads
the desktop session's display, bus and session type from the session process
(plasmashell, gnome-shell or cinnamon), then:

1. opens `cover_player.py play` (magenta, pinned to --output) and fails closed
   unless the player confirms its placement and `cover_player.py precheck`
   finds the output's rectangle covered on the host's loopback VNC tile;
2. starts --video in the middle of the cover: the looped `--av-marker` clip
   (`marker`), a scrolling pattern (`pattern`), or nothing (`still`, the
   frozen control for motion scores);
3. per round, format and build: starts that build's resident with private
   config and data directories under --run, records the target
   (`recording.prepare`, `start`, optional `pause`/`resume`, `stop`, `saveCopy`,
   then `cancel`) and quits it;
4. measures the working clip and the saved copy: `--motion` always; with the
   marker and sound, `--av-sync`, split at the pause with `pairTimesS` into the
   capture step (last two pairs before the pause against the first three after
   it, with whole 33.3 ms frame slips folded out; `rawCaptureStepMs` keeps them) and
   the export's shift after the pause (`exportMinusWorkingMs`);
5. with --frames, keeps one frame of each saved copy only if `check-frame`
   finds it at least half cover colour; otherwise the frame is deleted
   unopened and so are that recording's clips. A region must therefore
   straddle the video's edge (for example 0,0,960,540 on 1920x1080).

A marker clip with fewer than --min-pairs flash/burst pairs didn't show the
marker, so it is deleted unopened. Clips stay on the host; results.json holds
numbers, identities, host load and the kernel's GPU hang count for the run.

  record_run.py --run DIR --build LABEL=PATH [--build ...] --analyzer APPDIR
                [--video marker|pattern|still] [--target monitor|region|window]
                [--rect X,Y,W,H] [--identity NAME] [--format mp4 ...] [--rounds 3]
                [--output NAME] [--vnc-port 5900] [--lead 2] [--pause-at 8]
                [--pause-for 3] [--after 8] [--prepare-timeout 120] [--frames]

PATH is an AppImage (extracted once under DIR for its putgd-cli) or a directory
holding `putgd` and `putgd-cli` (an installed package: /usr/bin). APPDIR is an
extracted AppImage whose putgd-record reports `pairTimesS` (#143 or later).
--pause-for 0 records without a pause. --real-data-home lets an AppImage use
KWin's screencast protocol (its entry must be in the user's own data dir). The same data directory serves every
build, so one portal approval with restoring allowed covers the whole run.
When `prepare` waits for a portal picker, the driver says so and keeps waiting
up to --prepare-timeout.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import selectors
import signal
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
FRAME_MS = 1000 / 30
ANIMATIONS = ('gif', 'webp')
SESSION_VARIABLES = ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP',
                     'WAYLAND_DISPLAY', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION')


class RunError(RuntimeError):
    pass


def log(message):
    print(time.strftime('%H:%M:%S ') + message, file=sys.stderr, flush=True)


def session_environment():
    """The desktop session's environment for clients started from SSH."""
    uid = os.getuid()
    for process in ('plasmashell', 'gnome-shell', 'cinnamon'):
        pids = subprocess.run(['pgrep', '-u', str(uid), '-x', process], capture_output=True, text=True).stdout.split()
        if pids:
            break
    else:
        raise RunError('no desktop session process (plasmashell, gnome-shell, cinnamon) for this user')
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


def split_at_pause(times, offsets, pause_s, guard=0.2):
    before = [o for t, o in zip(times, offsets) if t < pause_s - guard]
    after = [o for t, o in zip(times, offsets) if t > pause_s + guard]
    return before, after


def whole_frames_removed(ms):
    return ms - FRAME_MS * round(ms / FRAME_MS)


def fold_frames(offsets):
    """Offsets moved by whole 33.3 ms frames next to the clip's common phase (their circular mean).

    The 30 fps marker and capture slip by exactly one frame at random pairs, so a
    clip's offsets are often split between two values one frame apart; taking a
    median across that split invents a half-frame step."""
    if not offsets:
        return []
    angles = [2 * math.pi * o / FRAME_MS for o in offsets]
    phase = math.atan2(sum(map(math.sin, angles)), sum(map(math.cos, angles))) * FRAME_MS / (2 * math.pi)
    return [o - FRAME_MS * round((o - phase) / FRAME_MS) for o in offsets]


def capture_step(times, offsets, pause_s, raw=False):
    """The working clip's change at the pause, with whole frames folded out unless raw; None without two pairs before and three after."""
    before, after = split_at_pause(times, offsets if raw else fold_frames(offsets), pause_s)
    if len(before) < 2 or len(after) < 3:
        return None
    step = statistics.median(after[:3]) - statistics.median(before[-2:])
    return round(step if raw else whole_frames_removed(step), 1)


def export_shift(working, exported, pause_s):
    """Median offset after the pause in the saved copy minus the working clip's."""
    after_w = split_at_pause(working['pairTimesS'], working['offsetsMs'], pause_s)[1]
    after_e = split_at_pause(exported['pairTimesS'], exported['offsetsMs'], pause_s)[1]
    if not after_w or not after_e:
        return None
    return round(statistics.median(after_e) - statistics.median(after_w), 1)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def pillow_motion(path, samples=8):
    """Mean absolute frame difference across an animated image (the analyzer's FFmpeg has no animated WebP decoder)."""
    from PIL import Image, ImageChops, ImageStat
    image = Image.open(path)
    count = getattr(image, 'n_frames', 1)
    if count < 2:
        return 0.0
    picks = sorted({round(i * (count - 1) / (samples - 1)) for i in range(samples)})
    frames = []
    for index in picks:
        image.seek(index)
        frames.append(image.convert('L').copy())
    diffs = [statistics.mean(ImageStat.Stat(ImageChops.difference(a, b)).mean) for a, b in zip(frames, frames[1:])]
    return round(statistics.mean(diffs), 3)


class Build:
    def __init__(self, label, path, run):
        self.label, self.path = label, Path(path).resolve()
        if self.path.suffix == '.AppImage':
            appdir = run / ('appdir-' + label)
            if not (appdir / 'squashfs-root').is_dir():
                appdir.mkdir(parents=True, exist_ok=True)
                subprocess.run([str(self.path), '--appimage-extract'], cwd=appdir, check=True, stdout=subprocess.DEVNULL)
            self.launcher = [str(self.path), '--background']
            self.cli = str(appdir / 'squashfs-root/usr/bin/putgd-cli')
            self.identity = {'kind': 'appimage', 'file': self.path.name, 'sha256': sha256(self.path)}
        else:
            self.launcher = [str(self.path / 'putgd'), '--background']
            self.cli = str(self.path / 'putgd-cli')
            version = subprocess.run([str(self.path / 'putgd-record'), '--version'], capture_output=True, text=True).stdout.strip()
            self.identity = {'kind': 'installed', 'dir': str(self.path), 'version': version}


class Driver:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve()
        for name in ('config', 'data', 'cache', 'state', 'working', 'out', 'frames', 'logs'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        os.chmod(self.run, 0o700)
        self.env, self.desktop = session_environment()
        # Every put.gd directory is private to the run: config, data, logs (state) and the AppImage's GStreamer registry (cache).
        self.env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_DATA_HOME=str(self.run / 'data'),
                        XDG_CACHE_HOME=str(self.run / 'cache'), XDG_STATE_HOME=str(self.run / 'state'),
                        PUTGD_RECORD_KEEP_WORKING=str(self.run / 'working'))
        self.real_data = None
        if args.real_data_home:
            # An AppImage's KWin screencast entry only works in the user's own data directory. put.gd's data
            # there is moved into the run afterwards, so it must not exist yet.
            self.real_data = Path(os.environ['HOME']) / '.local/share/putgd'
            if self.real_data.exists():
                raise RunError('%s exists; --real-data-home would mix with it' % self.real_data)
            del self.env['XDG_DATA_HOME']
        self.socket = Path(self.env['XDG_RUNTIME_DIR']) / 'putgd' / 'session.sock'
        self.analyzer = Path(args.analyzer).resolve()
        self.cover = None
        self.resident = None
        self.cover_rect = None

    # -- helpers -------------------------------------------------------------
    def cli(self, build, method, params=None, timeout=60):
        result = subprocess.run([build.cli, method, json.dumps(params or {})], env=self.env, capture_output=True,
                                text=True, timeout=timeout)
        try:
            reply = json.loads(result.stdout)
        except ValueError:
            raise RunError('%s: no JSON reply (exit %d): %s' % (method, result.returncode, result.stderr.strip()[-300:]))
        if not reply.get('ok'):
            error = reply.get('error') or {}
            raise RunError('%s failed: %s %s' % (method, error.get('code'), error.get('message')))
        return reply.get('result') or {}

    def helper(self, *arguments, timeout=300):
        env = dict(os.environ, APPDIR=str(self.analyzer))
        return subprocess.run([str(self.analyzer / 'usr/bin/putgd-record'), *map(str, arguments)], env=env,
                              capture_output=True, text=True, timeout=timeout)

    def av_sync(self, clip):
        result = self.helper('--av-sync', clip)
        try:
            return json.loads(result.stdout)
        except ValueError:
            return {'error': (result.stdout + result.stderr).strip()[-300:]}

    def motion(self, clip):
        result = self.helper('--motion', clip, 8)
        found = re.search(r'^motion=([0-9.]+)', result.stdout, re.M)
        if found:
            return float(found.group(1))
        if True:
            if clip.suffix == '.webp':
                try:
                    return pillow_motion(clip)
                except Exception as error:  # Pillow missing or unreadable file
                    return 'error: ' + type(error).__name__
            return 'error: ' + (result.stdout + result.stderr).strip()[-200:]

    def keep_frame(self, clip, name, duration_us):
        """One frame from the middle of a saved copy, kept only if check-frame finds it half cover colour."""
        frame = self.run / 'frames' / (name + '.png')
        if clip.suffix == '.webp':
            try:
                from PIL import Image
                image = Image.open(clip)
                image.seek(getattr(image, 'n_frames', 1) // 2)
                image.convert('RGB').save(frame)
            except Exception as error:
                return {'ok': False, 'reason': 'no frame: ' + type(error).__name__}
        elif self.helper('--preview', clip, int((duration_us or 0) / 2), frame).returncode != 0 or not frame.is_file():
            return {'ok': False, 'reason': 'no frame decoded'}
        checked = subprocess.run([sys.executable, str(HERE / 'cover_player.py'), 'check-frame', str(frame),
                                  '--min-fraction', '0.5'], capture_output=True, text=True)
        return json.loads(checked.stdout or '{"ok": false}')

    def wait_state(self, build, rid, wanted, timeout):
        started = time.monotonic()
        said = False
        while time.monotonic() - started < timeout:
            info = self.cli(build, 'recording.inspect', {'id': rid})
            if (info.get('error') or {}).get('code'):
                raise RunError('recording error: %s' % json.dumps(info['error']))
            if info.get('state') in wanted:
                return info
            if not said and info.get('state') == 'preparing' and time.monotonic() - started > 5:
                log('prepare is waiting (a portal picker may be open on the desktop)')
                said = True
            time.sleep(0.25)
        raise RunError('timed out waiting for %s' % '/'.join(wanted))

    # -- cover ---------------------------------------------------------------
    def open_cover(self):
        a = self.args
        command = [sys.executable, str(HERE / 'cover_player.py'), 'play']
        if a.video == 'marker':
            marker = self.run / 'marker.mkv'
            if not marker.is_file():
                self.helper('--av-marker', marker, 60).check_returncode()
            command += ['--media', str(marker)]
        elif a.video == 'pattern':
            command += ['--pattern']
        if a.output:
            command += ['--output', a.output]
        if a.audio_device:
            command += ['--audio-device', a.audio_device]
        self.cover = subprocess.Popen(command, env=dict(self.env, GTK_USE_PORTAL='0', NO_AT_BRIDGE='1'),
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                      stderr=open(self.run / 'logs' / 'cover.log', 'a'), start_new_session=True)
        state = self.cover_line(20)
        if not state.get('ok') or state.get('state') != 'covered':
            raise RunError('cover not placed: %s' % json.dumps(state))
        self.cover_rect = state['rect']
        time.sleep(1.5)
        precheck = [sys.executable, str(HERE / 'cover_player.py'), 'precheck', '--port', str(a.vnc_port)]
        if a.output:
            precheck += ['--output', a.output]
        checked = subprocess.run(precheck, env=self.env, capture_output=True, text=True, timeout=60)
        report = json.loads(checked.stdout or '{}')
        if checked.returncode != 0:
            raise RunError('cover precheck failed, nothing recorded: %s' % json.dumps(report))
        if a.video != 'still':
            self.cover.stdin.write('media\n'); self.cover.stdin.flush()
            playing = self.cover_line(30)
            if playing.get('state') != 'playing':
                raise RunError('video not playing: %s' % json.dumps(playing))
        return {'placed': state, 'precheck': report, 'video': a.video}

    def cover_line(self, timeout):
        selector = selectors.DefaultSelector()
        selector.register(self.cover.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise RunError('cover player did not answer within %d s' % timeout)
        line = self.cover.stdout.readline()
        return json.loads(line) if line.strip() else {'ok': False, 'reason': 'cover exited %s' % self.cover.poll()}

    def close_cover(self):
        if self.cover and self.cover.poll() is None:
            try:
                self.cover.stdin.write('quit\n'); self.cover.stdin.flush()
                self.cover.wait(10)
            except (OSError, subprocess.TimeoutExpired):
                os.killpg(self.cover.pid, signal.SIGTERM)
                self.cover.wait(10)

    # -- resident ------------------------------------------------------------
    def start_resident(self, build):
        if self.socket.exists():
            raise RunError('%s exists: another put.gd resident owns this session' % self.socket)
        self.resident = subprocess.Popen(build.launcher, env=self.env, stdin=subprocess.DEVNULL,
                                         stdout=open(self.run / 'logs' / ('resident-%s.log' % build.label), 'a'),
                                         stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                caps = self.cli(build, 'recording.capabilities', timeout=40)
                break
            except (RunError, subprocess.TimeoutExpired):
                time.sleep(0.5)
        else:
            raise RunError('resident did not answer')
        # A fresh configuration has no output; keep local copies inside the run, never in the user's Documents.
        settings = self.cli(build, 'settings.get')
        archive = (settings.get('document') or {}).get('archive') or {}
        root = str(self.run / 'archive')
        if not archive.get('keepUploads') or archive.get('root') != root:
            self.cli(build, 'settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': root}},
                                               'expectedRevision': (settings.get('metadata') or {}).get('observedRevision')})
        return caps

    def stop_resident(self, build):
        if self.resident is None:
            return
        try:
            self.cli(build, 'ui.quit', timeout=15)
        except (RunError, subprocess.TimeoutExpired):
            pass
        try:
            self.resident.wait(20)
        except subprocess.TimeoutExpired:
            os.killpg(self.resident.pid, signal.SIGTERM)
            self.resident.wait(10)
        deadline = time.monotonic() + 10
        while self.socket.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        self.resident = None

    # -- one recording -------------------------------------------------------
    def record(self, build, number, fmt):
        a = self.args
        name = '%s-%s-%d' % (build.label, fmt, number)
        result = {'build': build.label, 'round': number, 'format': fmt,
                  'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        caps = self.start_resident(build)
        copy = self.run / 'out' / ('%s.%s' % (name, fmt))
        try:
            result['acquisition'] = caps.get('acquisition')
            target = {'kind': a.target}
            if a.identity:
                target['identity'] = a.identity
            if a.rect:
                target['rect'] = a.rect
            sound = fmt not in ANIMATIONS
            prepared = self.cli(build, 'recording.prepare', {'target': target, 'format': fmt, 'fps': 30,
                                                              'audio': {'system': sound}})
            rid = prepared['recordingId']
            ready = self.wait_state(build, rid, ('ready',), a.prepare_timeout)
            result['canvas'] = ready.get('canvas')
            time.sleep(a.lead)
            self.cli(build, 'recording.start', {'id': rid})
            started = time.monotonic()
            time.sleep(a.pause_at)
            if a.pause_for > 0:
                self.cli(build, 'recording.pause', {'id': rid})
                result['pauseClipS'] = round(time.monotonic() - started, 3)
                time.sleep(a.pause_for)
                self.cli(build, 'recording.resume', {'id': rid})
            time.sleep(a.after)
            stopped = self.cli(build, 'recording.stop', {'id': rid}, timeout=60)
            result.update(load=os.getloadavg()[0], durationUs=stopped.get('durationUs'), encoder=stopped.get('encoder'),
                          audioEncoder=stopped.get('audioEncoder'))
            self.cli(build, 'recording.saveCopy', {'id': rid, 'path': str(copy), 'overwrite': True})
            done = self.wait_state(build, rid, ('reviewing',), 600)
            result['export'] = (done.get('lastExport') or {}).get('status')
            self.cli(build, 'recording.cancel', {'id': rid})
        finally:
            self.stop_resident(build)
        working = self.run / 'working' / (rid + '.mkv')
        result['clips'] = {}
        for kind, clip in (('working', working), ('export', copy)):
            entry = {'file': clip.name}
            if not clip.is_file():
                entry['error'] = 'missing'
            else:
                entry.update(bytes=clip.stat().st_size, motion=self.motion(clip))
                if a.video == 'marker' and sound:
                    measured = self.av_sync(clip)
                    entry.update({k: measured[k] for k in ('pairs', 'medianMs', 'spreadMs', 'offsetsMs', 'pairTimesS', 'error')
                                  if k in measured})
                    if measured.get('pairs', 0) < a.min_pairs:
                        clip.unlink()  # no marker: the clip may show something else
                        entry['destroyed'] = True
            result['clips'][kind] = entry
        w, e = result['clips']['working'], result['clips']['export']
        if 'pauseClipS' in result and w.get('pairTimesS'):
            result['captureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'])
            result['rawCaptureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'], raw=True)
            if e.get('pairTimesS'):
                result['exportMinusWorkingMs'] = export_shift(w, e, result['pauseClipS'])
        if a.frames and copy.is_file():
            result['frame'] = self.keep_frame(copy, name, result.get('durationUs'))
            if not result['frame'].get('ok'):
                # The cover didn't fill the recording: its clips may show something else.
                for kind, clip in (('working', working), ('export', copy)):
                    clip.unlink(missing_ok=True)
                    result['clips'][kind]['destroyed'] = True
        return result

    def gpu_hangs(self, since):
        out = subprocess.run(['journalctl', '-k', '--since', since, '--no-pager', '-o', 'short-iso'], capture_output=True,
                             text=True).stdout
        return [line for line in out.splitlines() if 'GPU HANG' in line or 'context reset due to GPU hang' in line]

    def main(self):
        a = self.args
        since = time.strftime('%Y-%m-%d %H:%M:%S')
        builds = [Build(*spec.split('=', 1), self.run) for spec in a.build]
        report = {'host': os.uname().nodename, 'kernel': os.uname().release, 'desktop': self.desktop,
                  'session': self.env.get('XDG_SESSION_TYPE'), 'target': a.target, 'rect': a.rect, 'formats': a.format,
                  'video': a.video, 'timing': {'lead': a.lead, 'pauseAt': a.pause_at, 'pauseFor': a.pause_for, 'after': a.after},
                  'builds': {b.label: b.identity for b in builds}, 'analyzer': str(self.analyzer), 'rounds': []}
        out = self.run / 'results.json'
        try:
            # A resident's first start with a fresh configuration can open a window; let that happen before the cover is up.
            for build in builds:
                self.start_resident(build)
                self.stop_resident(build)
            report['cover'] = self.open_cover()
            for number in range(1, a.rounds + 1):
                for fmt in a.format:
                    for build in builds:
                        log('round %d %s %s' % (number, fmt, build.label))
                        try:
                            entry = self.record(build, number, fmt)
                        except (RunError, subprocess.TimeoutExpired) as error:
                            entry = {'build': build.label, 'round': number, 'format': fmt, 'error': str(error)}
                            log('failed: %s' % error)
                        report['rounds'].append(entry)
                        out.write_text(json.dumps(report, indent=1) + '\n')
                        summary = {k: entry.get(k) for k in ('build', 'format', 'round', 'acquisition', 'encoder',
                                                             'captureStepMs', 'exportMinusWorkingMs', 'error', 'load')}
                        summary['motion'] = [c.get('motion') for c in entry.get('clips', {}).values()]
                        summary['medianMs'] = [c.get('medianMs') for c in entry.get('clips', {}).values()]
                        log(json.dumps(summary))
        except RunError as error:
            report['error'] = str(error)
            log('stopped: %s' % error)
        finally:
            self.close_cover()
            if self.real_data is not None and self.real_data.exists():
                self.real_data.rename(self.run / 'real-data-putgd')
            report['gpuHangs'] = self.gpu_hangs(since)
            out.write_text(json.dumps(report, indent=1) + '\n')
        return 1 if report.get('error') else 0


def parse_rect(text):
    parts = [int(p) for p in text.split(',')]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError('rectangle must be X,Y,W,H')
    return parts


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--run', required=True)
    parser.add_argument('--build', action='append', required=True, help='LABEL=APPIMAGE or LABEL=BINDIR')
    parser.add_argument('--analyzer', required=True, help='extracted AppImage with a putgd-record that reports pairTimesS')
    parser.add_argument('--video', choices=('marker', 'pattern', 'still'), default='marker')
    parser.add_argument('--target', choices=('monitor', 'region', 'window'), default='monitor')
    parser.add_argument('--rect', type=parse_rect)
    parser.add_argument('--identity')
    parser.add_argument('--format', nargs='+', choices=('mp4', 'webm', 'gif', 'webp'), default=['mp4'])
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output')
    parser.add_argument('--audio-device')
    parser.add_argument('--vnc-port', type=int, default=5900)
    parser.add_argument('--lead', type=float, default=2)
    parser.add_argument('--pause-at', type=float, default=8)
    parser.add_argument('--pause-for', type=float, default=3)
    parser.add_argument('--after', type=float, default=8)
    parser.add_argument('--prepare-timeout', type=float, default=120)
    parser.add_argument('--min-pairs', type=int, default=8)
    parser.add_argument('--frames', action='store_true', help='keep one cover-checked frame of each saved copy')
    parser.add_argument('--real-data-home', action='store_true',
                        help="use the user's data directory (an AppImage's KWin screencast entry); moved into --run afterwards")
    return Driver(parser.parse_args(argv)).main()


if __name__ == '__main__':
    sys.exit(main())
