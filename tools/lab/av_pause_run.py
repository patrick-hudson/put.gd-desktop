#!/usr/bin/env python3
"""Pause/resume A/V runs on a physical desktop, with builds interleaved round by round (stdlib only).

Run it on the host from a plain SSH shell, holding the desktop's lock. It reads
the desktop session's display, bus and session type from the session process
(plasmashell, gnome-shell or cinnamon), then:

1. opens `cover_player.py play` (magenta, pinned to --output) and fails closed
   unless the player confirms its placement and `cover_player.py precheck`
   finds the output's rectangle covered on the host's loopback VNC tile;
2. starts the looped `--av-marker` clip in the middle of the cover;
3. per round and build: starts that build's resident with private config and
   data directories under --run, records the target (`recording.prepare`,
   `start`, `pause`, `resume`, `stop`, `saveCopy`, then `cancel`) and quits it;
4. measures the working clip and the saved copy with --analyzer's
   `putgd-record --av-sync`, splits the offsets at the pause with `pairTimesS`
   and reports the capture step (last two pairs before the pause against the
   first three after it, whole 33.3 ms frames removed) and the export's shift
   after the pause (`exportMinusWorkingMs`).

A clip with fewer than --min-pairs flash/burst pairs didn't show the marker,
so it is deleted unopened. Clips stay on the host; results.json holds numbers,
identities, host load and the kernel's GPU hang count for the run.

  av_pause_run.py --run DIR --build LABEL=PATH [--build LABEL=PATH ...] --analyzer APPDIR
                  [--target monitor|window] [--identity NAME] [--format mp4|webm]
                  [--rounds 3] [--output NAME] [--vnc-port 5900] [--lead 2]
                  [--pause-at 8] [--pause-for 3] [--after 8] [--prepare-timeout 120]

PATH is an AppImage (extracted once under DIR for its putgd-cli) or a directory
holding `putgd` and `putgd-cli` (an installed package: /usr/bin). APPDIR is an
extracted AppImage whose putgd-record reports `pairTimesS` (#143 or later). The
same data directory serves every build, so one portal approval with restoring
allowed covers the whole run. When `prepare` waits for a portal picker, the
driver says so and keeps waiting up to --prepare-timeout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
FRAME_MS = 1000 / 30
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


def capture_step(times, offsets, pause_s, raw=False):
    """The working clip's change at the pause, less the nearest whole frame unless raw; None without two pairs before and three after."""
    before, after = split_at_pause(times, offsets, pause_s)
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
            version = subprocess.run([str(self.path / 'putgd'), '--version'], capture_output=True, text=True).stdout.strip()
            self.identity = {'kind': 'installed', 'dir': str(self.path), 'version': version}


class Driver:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve()
        for name in ('config', 'data', 'working', 'out', 'logs'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        os.chmod(self.run, 0o700)
        self.env, self.desktop = session_environment()
        self.env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_DATA_HOME=str(self.run / 'data'),
                        PUTGD_RECORD_KEEP_WORKING=str(self.run / 'working'))
        self.socket = Path(self.env['XDG_RUNTIME_DIR']) / 'putgd' / 'session.sock'
        self.analyzer = Path(args.analyzer).resolve()
        self.cover = None
        self.resident = None

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

    def analyze(self, clip):
        env = dict(os.environ, APPDIR=str(self.analyzer))
        result = subprocess.run([str(self.analyzer / 'usr/bin/putgd-record'), '--av-sync', str(clip)], env=env,
                                capture_output=True, text=True, timeout=300)
        try:
            return json.loads(result.stdout)
        except ValueError:
            return {'error': (result.stdout + result.stderr).strip()[-300:]}

    def wait_state(self, build, rid, wanted, timeout):
        deadline, said = time.monotonic() + timeout, False
        while time.monotonic() < deadline:
            info = self.cli(build, 'recording.inspect', {'id': rid})
            if info.get('error'):
                raise RunError('recording error: %s' % json.dumps(info['error']))
            if info.get('state') in wanted:
                return info
            if not said and info.get('state') == 'preparing' and time.monotonic() > deadline - timeout + 5:
                log('prepare is waiting (a portal picker may be open on the desktop)')
                said = True
            time.sleep(0.25)
        raise RunError('timed out waiting for %s' % '/'.join(wanted))

    # -- cover ---------------------------------------------------------------
    def open_cover(self):
        marker = self.run / 'marker.mkv'
        if not marker.is_file():
            env = dict(os.environ, APPDIR=str(self.analyzer))
            subprocess.run([str(self.analyzer / 'usr/bin/putgd-record'), '--av-marker', str(marker), '60'], env=env,
                           check=True, capture_output=True, timeout=300)
        command = [sys.executable, str(HERE / 'cover_player.py'), 'play', '--media', str(marker)]
        if self.args.output:
            command += ['--output', self.args.output]
        if self.args.audio_device:
            command += ['--audio-device', self.args.audio_device]
        self.cover = subprocess.Popen(command, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                      stderr=open(self.run / 'logs' / 'cover.log', 'a'), start_new_session=True)
        state = self.cover_line(20)
        if not state.get('ok') or state.get('state') != 'covered':
            raise RunError('cover not placed: %s' % json.dumps(state))
        time.sleep(1.5)
        precheck = [sys.executable, str(HERE / 'cover_player.py'), 'precheck', '--port', str(self.args.vnc_port)]
        if self.args.output:
            precheck += ['--output', self.args.output]
        checked = subprocess.run(precheck, env=self.env, capture_output=True, text=True, timeout=60)
        report = json.loads(checked.stdout or '{}')
        if checked.returncode != 0:
            raise RunError('cover precheck failed, nothing recorded: %s' % json.dumps(report))
        self.cover.stdin.write('media\n'); self.cover.stdin.flush()
        playing = self.cover_line(20)
        if playing.get('state') != 'playing':
            raise RunError('marker not playing: %s' % json.dumps(playing))
        return {'placed': state, 'precheck': report}

    def cover_line(self, timeout):
        import selectors
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
                return self.cli(build, 'recording.capabilities', timeout=40)
            except (RunError, subprocess.TimeoutExpired):
                time.sleep(0.5)
        raise RunError('resident did not answer')

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

    # -- one round -----------------------------------------------------------
    def record(self, build, number):
        a = self.args
        result = {'build': build.label, 'round': number, 'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        caps = self.start_resident(build)
        try:
            result['acquisition'] = caps.get('acquisition')
            target = {'kind': a.target}
            if a.identity:
                target['identity'] = a.identity
            prepared = self.cli(build, 'recording.prepare', {'target': target, 'format': a.format, 'fps': 30,
                                                              'audio': {'system': True}})
            rid = prepared['id']
            self.wait_state(build, rid, ('ready',), a.prepare_timeout)
            time.sleep(a.lead)
            self.cli(build, 'recording.start', {'id': rid})
            started = time.monotonic()
            time.sleep(a.pause_at)
            self.cli(build, 'recording.pause', {'id': rid})
            paused = time.monotonic()
            time.sleep(a.pause_for)
            self.cli(build, 'recording.resume', {'id': rid})
            time.sleep(a.after)
            stopped = self.cli(build, 'recording.stop', {'id': rid}, timeout=60)
            result.update(load=os.getloadavg()[0], durationUs=stopped.get('durationUs'), encoder=stopped.get('encoder'),
                          audioEncoder=stopped.get('audioEncoder'), pauseClipS=round(paused - started, 3))
            copy = self.run / 'out' / ('%s-%d.%s' % (build.label, number, a.format))
            self.cli(build, 'recording.saveCopy', {'id': rid, 'path': str(copy), 'overwrite': True})
            done = self.wait_state(build, rid, ('reviewing',), 300)
            result['export'] = (done.get('lastExport') or {}).get('status')
            self.cli(build, 'recording.cancel', {'id': rid})
        finally:
            self.stop_resident(build)
        working = self.run / 'working' / (rid + '.mkv')
        result['clips'] = {}
        for name, clip in (('working', working), ('export', copy)):
            measured = self.analyze(clip) if clip.is_file() else {'error': 'missing'}
            if measured.get('pairs', 0) < a.min_pairs:
                clip.unlink(missing_ok=True)  # no marker: the clip may show something else
                measured['destroyed'] = True
            result['clips'][name] = {k: measured.get(k) for k in ('pairs', 'medianMs', 'spreadMs', 'offsetsMs', 'pairTimesS',
                                                                 'error', 'destroyed') if k in measured}
            result['clips'][name]['file'] = clip.name
        w, e = result['clips']['working'], result['clips']['export']
        if w.get('pairTimesS'):
            result['captureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'])
            result['rawCaptureStepMs'] = capture_step(w['pairTimesS'], w['offsetsMs'], result['pauseClipS'], raw=True)
        if w.get('pairTimesS') and e.get('pairTimesS'):
            result['exportMinusWorkingMs'] = export_shift(w, e, result['pauseClipS'])
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
                  'session': self.env.get('XDG_SESSION_TYPE'), 'target': a.target, 'format': a.format,
                  'timing': {'lead': a.lead, 'pauseAt': a.pause_at, 'pauseFor': a.pause_for, 'after': a.after},
                  'builds': {b.label: b.identity for b in builds}, 'analyzer': str(self.analyzer), 'rounds': []}
        out = self.run / 'results.json'
        try:
            report['cover'] = self.open_cover()
            for number in range(1, a.rounds + 1):
                for build in builds:
                    log('round %d %s' % (number, build.label))
                    try:
                        entry = self.record(build, number)
                    except (RunError, subprocess.TimeoutExpired) as error:
                        entry = {'build': build.label, 'round': number, 'error': str(error)}
                        log('failed: %s' % error)
                    report['rounds'].append(entry)
                    out.write_text(json.dumps(report, indent=1) + '\n')
                    log(json.dumps({k: entry.get(k) for k in ('build', 'round', 'acquisition', 'encoder', 'captureStepMs',
                                                               'exportMinusWorkingMs', 'error', 'load')}))
        except RunError as error:
            report['error'] = str(error)
            log('stopped: %s' % error)
        finally:
            self.close_cover()
            report['gpuHangs'] = self.gpu_hangs(since)
            out.write_text(json.dumps(report, indent=1) + '\n')
        return 1 if report.get('error') else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--run', required=True)
    parser.add_argument('--build', action='append', required=True, help='LABEL=APPIMAGE or LABEL=BINDIR')
    parser.add_argument('--analyzer', required=True, help='extracted AppImage with a putgd-record that reports pairTimesS')
    parser.add_argument('--target', choices=('monitor', 'window'), default='monitor')
    parser.add_argument('--identity')
    parser.add_argument('--format', choices=('mp4', 'webm'), default='mp4')
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
    return Driver(parser.parse_args(argv)).main()


if __name__ == '__main__':
    sys.exit(main())
