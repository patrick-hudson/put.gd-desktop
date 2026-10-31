#!/usr/bin/env python3
"""Fluidity acceptance on a physical desktop: recorder frame cadence and selector drags, per build (#6).

Run it on the builder from a plain SSH shell while holding the desktop's lock
(tools/lab/desktop_lock.py). It reads the desktop session's environment from
the session process like record_run.py, then:

  fluidity_run.py record --run DIR --build LABEL=PATH [--build ...] [--output NAME]
                         [--fps 60] [--seconds 10] [--target monitor|region] [--rect X,Y,W,H]
                         [--rounds 1] [--real-data-home] [--kwin-entry LABEL ...]
      Per round and build: shows the fluidity fixture (tools/lab/fluidity.py)
      full screen on --output, starts that build's resident with private
      config/data/state directories under --run (PUTGD_RECORD_DEBUG_LOG and
      PUTGD_REGION_DEBUG_LOG inside --run/logs), records the target at --fps
      for --seconds (recording.prepare/start/stop/saveCopy/cancel), quits
      both, and measures the working clip and the saved copy with
      fluidity.analyze_clip and the fixture's frame ticks while the recording
      ran with fluidity.ticks_report. results.json under --run holds every
      number with build identities, encoder, acquisition, host load and the
      recorder's debug-log frame counts.
  fluidity_run.py select --run DIR --build LABEL=PATH [--build ...] [--input-socket PATH]
                         [--action region|record|edit] [--start X,Y] [--end X,Y] [--moves 300]
                         [--duration 1.0] [--rounds 3]
      Per round and build: starts the resident as above, invokes the region
      (or record, or edit) action, drags from --start to --end with --moves pointer events over
      --duration seconds through the lab's uinput agent (host_input.py serve,
      QMP-compatible socket; on X11 without an agent, xdotool), then cancels
      with Escape. The selector's PUTGD_REGION_DEBUG_LOG summary line (moves,
      paints or presents, timing) is collected per build into results.json.

PATH is an AppImage or a directory holding putgd, putgd-cli and putgd-record (a
build tree or /usr/bin). --real-data-home lets an AppImage register KWin's
screencast entry in the user's data directory (see record_run.py).
--kwin-entry LABEL writes a temporary putgd-record desktop entry naming that
build's helper, so KWin offers its screencast protocol to a build tree, and
removes it afterwards.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import record_run  # noqa: E402
import fluidity  # noqa: E402

log = record_run.log
RunError = record_run.RunError


class Agent:
    """The lab input agent's QMP subset over its Unix socket (host_input.py serve)."""

    def __init__(self, path, timeout=10):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.stream = self.sock.makefile('rwb')
        greeting = json.loads(self.stream.readline())
        if 'QMP' not in greeting:
            raise RunError('input agent: no QMP greeting')
        self.execute('qmp_capabilities')

    def execute(self, command, arguments=None):
        message = {'execute': command}
        if arguments is not None:
            message['arguments'] = arguments
        self.stream.write((json.dumps(message) + '\n').encode())
        self.stream.flush()
        while True:
            reply = json.loads(self.stream.readline())
            if 'return' in reply:
                return reply['return']
            if 'error' in reply:
                raise RunError('input agent: %s' % reply['error'])

    def pointer(self, x, y, width, height, down=None, button='left'):
        events = [{'type': 'abs', 'data': {'axis': 'x', 'value': min(32767, int(x * 32768 / width))}},
                  {'type': 'abs', 'data': {'axis': 'y', 'value': min(32767, int(y * 32768 / height))}}]
        if down is not None:
            events.append({'type': 'btn', 'data': {'button': button, 'down': down}})
        self.execute('input-send-event', {'events': events})

    def key(self, qcode):
        for down in (True, False):
            self.execute('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': qcode}}}]})

    def close(self):
        self.sock.close()


class Driver:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve()
        for name in ('config', 'data', 'cache', 'state', 'working', 'out', 'logs'):
            (self.run / name).mkdir(parents=True, exist_ok=True)
        os.chmod(self.run, 0o700)
        self.env, self.desktop = record_run.session_environment()
        self.env.update(XDG_CONFIG_HOME=str(self.run / 'config'), XDG_DATA_HOME=str(self.run / 'data'),
                        XDG_CACHE_HOME=str(self.run / 'cache'), XDG_STATE_HOME=str(self.run / 'state'),
                        PUTGD_RECORD_KEEP_WORKING=str(self.run / 'working'))
        # Lab A/B switches such as PUTGD_RECORD_PIPEWIRE_TIMESTAMPS pass through to the helper.
        self.env.update({k: v for k, v in os.environ.items() if k.startswith('PUTGD_RECORD_') and k not in ('PUTGD_RECORD_KEEP_WORKING', 'PUTGD_RECORD_DEBUG_LOG')})
        if getattr(args, 'real_data_home', False):
            real = Path(os.environ['HOME']) / '.local/share/putgd'
            if real.exists():
                raise RunError('%s exists; --real-data-home would mix with it' % real)
            del self.env['XDG_DATA_HOME']
        if args.runtime_dir:
            # A private runtime directory keeps this resident's session socket apart
            # from a resident the desktop's user is running (workstation checks);
            # the X11 recorder needs no PipeWire socket. Keep the path short.
            runtime = Path(args.runtime_dir).resolve()
            runtime.mkdir(parents=True, exist_ok=True)
            os.chmod(runtime, 0o700)
            if len(str(runtime / 'putgd' / 'session.sock').encode()) > 100:
                raise RunError('--runtime-dir too long for a session socket path')
            self.env['XDG_RUNTIME_DIR'] = str(runtime)
        self.socket = Path(self.env['XDG_RUNTIME_DIR']) / 'putgd' / 'session.sock'

        self.resident = None
        self.fixture = None
        self.fixture_info = None
        self.entries = []
        self.builds = []
        for spec in args.build:
            label, _, path = spec.partition('=')
            if not path:
                raise RunError('--build needs LABEL=PATH')
            if Path(path).is_file() and Path(path).suffix != '.AppImage':
                # record_run.Build recognises AppImages by their suffix and resolves
                # symlinks, so other spellings get a private copy under the run.
                alias = self.run / ('%s.AppImage' % label)
                if not alias.exists():
                    shutil.copy2(path, alias)
                path = str(alias)
            build = record_run.Build(label, path, self.run)
            if build.identity['kind'] == 'appimage':
                # Run the extracted image: its helper then has a stable path a
                # KWin screencast entry can name, unlike a fresh FUSE mount.
                appdir = self.run / ('appdir-' + label) / 'squashfs-root'
                build.launcher = [str(appdir / 'AppRun'), '--background']
                build.helper = appdir / 'usr/bin/putgd-record'
            else:
                build.helper = Path(build.path) / 'putgd-record'
            self.builds.append(build)

    # -- resident -----------------------------------------------------------
    def cli(self, build, method, params=None, timeout=60):
        result = subprocess.run([build.cli, method, json.dumps(params or {})], env=self.env, capture_output=True, text=True, timeout=timeout)
        try:
            reply = json.loads(result.stdout)
        except ValueError:
            raise RunError('%s: no JSON reply (exit %d): %s' % (method, result.returncode, result.stderr.strip()[-300:]))
        if not reply.get('ok'):
            error = reply.get('error') or {}
            raise RunError('%s failed: %s %s' % (method, error.get('code'), error.get('message')))
        return reply.get('result') or {}

    def start_resident(self, build, label):
        if self.socket.exists():
            raise RunError('%s exists: another put.gd resident owns this session' % self.socket)
        env = dict(self.env, PUTGD_RECORD_DEBUG_LOG=str(self.run / 'logs' / ('record-%s.log' % label)),
                   PUTGD_REGION_DEBUG_LOG=str(self.run / 'logs' / ('region-%s.log' % label)))
        self.resident = subprocess.Popen(build.launcher, env=env, stdin=subprocess.DEVNULL,
                                         stdout=open(self.run / 'logs' / ('resident-%s.log' % label), 'a'),
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

    def kwin_entry(self, build):
        """A temporary desktop entry so KWin offers zkde_screencast to this build's helper."""
        helper = build.helper
        directory = Path(os.environ['HOME']) / '.local/share/applications'
        directory.mkdir(parents=True, exist_ok=True)
        entry = directory / ('putgd-record-fluidity-%s.desktop' % build.label)
        entry.write_text('[Desktop Entry]\nType=Application\nName=put.gd screen recorder (fluidity lab %s)\nExec=%s\nNoDisplay=true\n'
                         'StartupWMClass=putgd-record\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n' % (build.label, helper))
        self.entries.append(entry)
        log('wrote %s' % entry)
        # KWin looks the client up in KSycoca; rebuild it so the entry is known at once.
        if shutil.which('kbuildsycoca6'):
            subprocess.run(['kbuildsycoca6'], env=self.env, capture_output=True, timeout=60)

    def cleanup(self):
        for entry in self.entries:
            try:
                entry.unlink()
                log('removed %s' % entry)
            except FileNotFoundError:
                pass
        self.entries = []

    # -- fixture ------------------------------------------------------------
    def chosen_output(self):
        """The session output to cover and record: --output by connector name, else the only one."""
        outputs = json.loads(subprocess.run([sys.executable, str(HERE / 'cover_player.py'), 'outputs'], env=self.env, capture_output=True, text=True, timeout=30).stdout)
        candidates = outputs.get('outputs') or []
        if self.args.output:
            candidates = [o for o in candidates if o.get('name') == self.args.output]
        if len(candidates) != 1:
            raise RunError('choose one output with --output: %s' % json.dumps(outputs))
        return candidates[0]

    def open_fixture(self, ticks, output):
        # GTK names monitors by model; the fixture also matches "WxH+X+Y".
        rect = output['rect']
        command = [sys.executable, str(HERE / 'fluidity.py'), 'fixture', '--seconds', '0', '--ticks', str(ticks),
                   '--output', '%dx%d+%d+%d' % (rect[2], rect[3], rect[0], rect[1])]
        self.fixture = subprocess.Popen(command, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(self.run / 'logs' / 'fixture.log', 'a'), text=True)
        line = self.fixture.stdout.readline()
        try:
            info = json.loads(line)
        except ValueError:
            raise RunError('fixture: no placement line: %r' % line)
        if not info.get('ok'):
            raise RunError('fixture: %s' % info)
        self.fixture_info = info
        return info

    def close_fixture(self):
        fixture, self.fixture = self.fixture, None
        if fixture is None:
            return
        try:
            fixture.stdin.write('quit\n')
            fixture.stdin.flush()
        except OSError:
            pass
        try:
            fixture.wait(10)
        except subprocess.TimeoutExpired:
            fixture.kill()
            fixture.wait(5)

    @staticmethod
    def cpu_ms(pattern):
        """utime+stime of the processes matching pattern (pgrep -f), in milliseconds; None without a match."""
        pids = subprocess.run(['pgrep', '-u', str(os.getuid()), '-f', pattern], capture_output=True, text=True).stdout.split()
        total = 0
        found = False
        for pid in pids:
            try:
                fields = Path('/proc/%s/stat' % pid).read_text().rsplit(')', 1)[1].split()
                total += (int(fields[11]) + int(fields[12])) * 1000 // os.sysconf('SC_CLK_TCK')
                found = True
            except (OSError, IndexError, ValueError):
                continue
        return total if found else None

    def frame(self, name):
        """One frame of the host's loopback VNC tile as evidence (PNG under --run/frames)."""
        if not self.args.frames:
            return None
        try:
            import rfb
            (self.run / 'frames').mkdir(exist_ok=True)
            path = self.run / 'frames' / (name + '.png')
            client = rfb.RFB(self.args.vnc_port)
            try:
                client.screenshot(str(path))
            finally:
                client.close()
            return str(path)
        except Exception as error:  # the tile is evidence, not a gate
            log('frame %s failed: %s' % (name, error))
            return None

    @staticmethod
    def last_tick(ticks):
        try:
            with open(ticks) as handle:
                lines = [line for line in handle if line and not line.startswith('#')]
            return int(lines[-1].split()[1]) if lines else None
        except (OSError, IndexError, ValueError):
            return None

    def wait_state(self, build, rid, wanted, timeout):
        started = time.monotonic()
        while time.monotonic() - started < timeout:
            info = self.cli(build, 'recording.inspect', {'id': rid})
            if (info.get('error') or {}).get('code'):
                raise RunError('recording error: %s' % json.dumps(info['error']))
            if info.get('state') in wanted:
                return info
            time.sleep(0.25)
        raise RunError('timed out waiting for %s' % '/'.join(wanted))

    @staticmethod
    def debug_lines(path, prefixes):
        found = {}
        try:
            for line in Path(path).read_text().splitlines():
                for prefix in prefixes:
                    if prefix in line:
                        found[prefix] = line.split(prefix, 1)[1].strip()
        except OSError:
            pass
        return found

    # -- record -------------------------------------------------------------
    def record_round(self, build, number):
        a = self.args
        label = '%s-%d' % (build.label, number)
        ticks = self.run / 'logs' / ('ticks-%s.log' % label)
        result = {'build': build.label, 'round': number, 'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        output = self.chosen_output()
        info = self.open_fixture(ticks, output)
        result['fixture'] = info
        result['output'] = output
        try:
            caps = self.start_resident(build, label)
            result['acquisition'] = caps.get('acquisition')
            result['liveEncoders'] = (caps.get('gstreamer') or {}).get('usableH264Encoders')
            target = {'kind': a.target}
            if a.target == 'monitor':
                target['identity'] = output['name']
            if a.rect:
                target['rect'] = [int(v) for v in a.rect.split(',')]
            prepared = self.cli(build, 'recording.prepare', {'target': target, 'format': 'mp4', 'fps': a.fps, 'audio': {'system': False}})
            rid = prepared['recordingId']
            ready = self.wait_state(build, rid, ('ready',), 120)
            result['canvas'] = ready.get('canvas')
            time.sleep(1.5)
            tick_start = self.last_tick(ticks)
            compositor_before = self.cpu_ms('kwin_wayland|gnome-shell|cinnamon$|Xorg')
            helper_before = self.cpu_ms('putgd-record')
            self.cli(build, 'recording.start', {'id': rid})
            time.sleep(a.seconds / 2)
            result['frame'] = self.frame('record-' + label)
            time.sleep(a.seconds / 2)
            progress = self.cli(build, 'recording.inspect', {'id': rid})
            result['cpuMs'] = {'compositor': None if compositor_before is None else (self.cpu_ms('kwin_wayland|gnome-shell|cinnamon$|Xorg') or 0) - compositor_before,
                               'recorder': None if helper_before is None else (self.cpu_ms('putgd-record') or 0) - helper_before, 'seconds': a.seconds}
            stopped = self.cli(build, 'recording.stop', {'id': rid}, timeout=60)
            tick_end = self.last_tick(ticks)
            result['progress'] = progress.get('progress')
            result['durationUs'] = stopped.get('durationUs')
            result['load'] = os.getloadavg()[0]
            copy = self.run / 'out' / ('%s.mp4' % label)
            self.cli(build, 'recording.saveCopy', {'id': rid, 'path': str(copy), 'overwrite': True})
            done = self.wait_state(build, rid, ('reviewing',), 600)
            result['export'] = (done.get('lastExport') or {}).get('status')
            self.cli(build, 'recording.cancel', {'id': rid})
        finally:
            self.stop_resident(build)
            self.close_fixture()
        working = self.run / 'working' / (rid + '.mkv')
        result['clips'] = {}
        for name, path in (('working', working), ('copy', copy)):
            if path.is_file():
                try:
                    result['clips'][name] = fluidity.analyze_clip(str(path), a.fps)
                except Exception as error:  # a clip the host cannot decode
                    result['clips'][name] = {'ok': False, 'error': str(error)}
        result['ticks'] = fluidity.ticks_report(str(ticks), 60, tick_start, tick_end) if ticks.is_file() else None
        result['recorder'] = self.debug_lines(self.run / 'logs' / ('record-%s.log' % label), ('frames:', 'timestamps:', 'started with', 'pipeline:'))
        result['recorderEnvironment'] = {k: v for k, v in os.environ.items() if k.startswith('PUTGD_RECORD_')}
        log('%s: %s' % (label, json.dumps({k: result['clips'].get('working', {}).get(k) for k in ('clipFps', 'distinctPerSecond', 'fluidFraction', 'longestStallMs', 'pairs')})))
        return result

    def record(self):
        results = self.header()
        try:
            for number in range(1, self.args.rounds + 1):
                for build in self.builds:
                    if build.label in (self.args.kwin_entry or []):
                        self.kwin_entry(build)
                    try:
                        results['rounds'].append(self.record_round(build, number))
                    except (RunError, subprocess.SubprocessError, OSError, ValueError) as error:
                        log('%s round %d failed: %s' % (build.label, number, error))
                        results['rounds'].append({'build': build.label, 'round': number, 'error': str(error)})
                        self.stop_resident(build)
                        self.close_fixture()
                    finally:
                        self.cleanup()
                    self.write(results)
        finally:
            self.cleanup()
        self.write(results)
        return results

    # -- select -------------------------------------------------------------
    def drag(self, start, end, moves, duration, agent, geometry):
        width, height = geometry[2], geometry[3]
        x0, y0 = start
        x1, y1 = end
        if agent:
            agent.pointer(x0, y0, width, height)
            time.sleep(0.2)
            agent.pointer(x0, y0, width, height, down=True)
        else:
            subprocess.run(['xdotool', 'mousemove', str(x0), str(y0), 'mousedown', '1'], env=self.env, check=True)
        began = time.monotonic()
        for i in range(1, moves + 1):
            x = x0 + (x1 - x0) * i / moves
            y = y0 + (y1 - y0) * i / moves
            if agent:
                agent.pointer(x, y, width, height)
            else:
                subprocess.run(['xdotool', 'mousemove', str(int(x)), str(int(y))], env=self.env, check=True)
            target = began + duration * i / moves
            wait = target - time.monotonic()
            if wait > 0:
                time.sleep(wait)
        took = time.monotonic() - began
        time.sleep(0.3)
        return took

    def escape(self, agent, point, geometry):
        """Cancel the selection while the button is held, then release the button:
        a button left down makes every later press a duplicate the compositor ignores."""
        if agent:
            agent.key('esc')
            time.sleep(0.2)
            agent.pointer(point[0], point[1], geometry[2], geometry[3], down=False)
        else:
            subprocess.run(['xdotool', 'key', 'Escape', 'mouseup', '1'], env=self.env, check=True)

    def select_round(self, build, number, agent):
        a = self.args
        label = '%s-%d' % (build.label, number)
        result = {'build': build.label, 'round': number, 'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
        geometry = self.chosen_output()['rect']
        try:
            self.start_resident(build, label)
            start = tuple(int(v) for v in a.start.split(',')) if a.start else (geometry[0] + geometry[2] // 5, geometry[1] + geometry[3] // 5)
            end = tuple(int(v) for v in a.end.split(',')) if a.end else (geometry[0] + geometry[2] * 4 // 5, geometry[1] + geometry[3] * 4 // 5)
            self.cli(build, 'action.invoke', {'action': a.action})
            time.sleep(a.settle)
            selector = 'putgd-kde-region|putgd-capture|putgd-record|putgd$'
            compositor = 'kwin_wayland|gnome-shell|cinnamon$|Xorg'
            before = {'selector': self.cpu_ms(selector), 'compositor': self.cpu_ms(compositor)}
            result['dragSeconds'] = round(self.drag(start, end, a.moves, a.duration, agent, geometry), 3)
            result['moves'] = a.moves
            # CPU the selector process and the compositor spent during the drag (the
            # selector is sampled while its surfaces still show, before Escape).
            result['cpuMs'] = {key: None if before[key] is None else (self.cpu_ms(pattern) or 0) - before[key]
                               for key, pattern in (('selector', selector), ('compositor', compositor))}
            result['frame'] = self.frame('select-' + label)
            self.escape(agent, end, geometry)
            time.sleep(1.0)
        finally:
            self.stop_resident(build)
        lines = []
        try:
            lines = [line for line in (self.run / 'logs' / ('region-%s.log' % label)).read_text().splitlines() if line.strip()]
        except OSError:
            pass
        result['selector'] = lines
        log('%s: %s' % (label, lines[-1] if lines else 'no selector log line'))
        return result

    def select(self):
        a = self.args
        results = self.header()
        agent = None
        if a.input_socket:
            agent = Agent(a.input_socket)
        elif self.env.get('XDG_SESSION_TYPE') == 'wayland':
            raise RunError('a Wayland session needs the uinput agent: --input-socket')
        try:
            for number in range(1, a.rounds + 1):
                for build in self.builds:
                    try:
                        results['rounds'].append(self.select_round(build, number, agent))
                    except (RunError, subprocess.SubprocessError, OSError, ValueError) as error:
                        log('%s round %d failed: %s' % (build.label, number, error))
                        results['rounds'].append({'build': build.label, 'round': number, 'error': str(error)})
                        self.stop_resident(build)
                    self.write(results)
        finally:
            if agent:
                agent.close()
        self.write(results)
        return results

    # -- results ------------------------------------------------------------
    def header(self):
        uname = os.uname()
        return {'tool': 'fluidity_run.py', 'command': self.args.command, 'host': uname.nodename, 'kernel': uname.release,
                'session': {'type': self.env.get('XDG_SESSION_TYPE'), 'desktop': self.env.get('XDG_CURRENT_DESKTOP'), 'process': self.desktop},
                'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'arguments': {k: v for k, v in vars(self.args).items() if k != 'func'},
                'builds': {b.label: b.identity for b in self.builds}, 'rounds': []}

    def write(self, results):
        (self.run / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('record', 'select'):
        p = commands.add_parser(name)
        p.add_argument('--run', required=True)
        p.add_argument('--build', action='append', required=True, metavar='LABEL=PATH')
        p.add_argument('--output')
        p.add_argument('--rounds', type=int, default=1)
        p.add_argument('--runtime-dir', help='private XDG_RUNTIME_DIR for the resident (short path)')
        p.add_argument('--frames', action='store_true', help='keep one VNC tile frame per round as evidence')
        p.add_argument('--vnc-port', type=int, default=5900)
        if name == 'record':
            p.add_argument('--fps', type=int, default=60)
            p.add_argument('--seconds', type=float, default=10)
            p.add_argument('--target', choices=('monitor', 'region'), default='monitor')
            p.add_argument('--rect')
            p.add_argument('--real-data-home', action='store_true')
            p.add_argument('--kwin-entry', action='append', metavar='LABEL')
        else:
            p.add_argument('--input-socket')
            p.add_argument('--action', choices=('region', 'record', 'edit'), default='region', help='which drag-to-select to open')
            p.add_argument('--start')
            p.add_argument('--end')
            p.add_argument('--moves', type=int, default=300)
            p.add_argument('--duration', type=float, default=1.0)
            p.add_argument('--settle', type=float, default=2.0)
    args = parser.parse_args(argv)
    driver = Driver(args)
    results = driver.record() if args.command == 'record' else driver.select()
    failed = [r for r in results['rounds'] if r.get('error')]
    print(json.dumps({'ok': not failed, 'results': str(driver.run / 'results.json'), 'rounds': len(results['rounds']), 'failed': len(failed)}))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
