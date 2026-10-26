#!/usr/bin/env python3
"""Replay one geometry layout on a visible, prepared multi-head lab guest.

  multi_output.py RUN --output NEW_DIR [--case NAME ...] [--appimage FILE]

The guest must already have its native layout, production app and clipboard
dependencies. This driver uses the existing lab transport (also physical.py's
injected transport), production CLI, native GTK pixels/paste, and independent
QMP/VNC witnesses. It does not install desktop packages, unlock, restart a
session, change topology, waive failures, or retire the overlay.

Use --change-command FILE for the output-change cases: FILE contains a reviewed
native-session shell command that resizes/disables only the disposable second
output. Its bytes and result are retained. Restore the layout outside this
driver before a subsequent campaign. All paths are short/private; artifact
directories are new and failed attempts are retained.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lab

COLORS = [(220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20),
          (30, 200, 210), (180, 40, 200), (230, 110, 30), (70, 90, 110)]
CASES = ('inside-0', 'inside-1', 'span', 'window-0', 'window-1', 'desktop',
         'escape', 'right-click', 'focus-change', 'record-region', 'output-change')
TERMINAL = ('completed', 'failed', 'cancelled', 'partial', 'interrupted')
MUTTER_OUTPUTS = r'''
import json
from gi.repository import Gio
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
serial,monitors,logical,properties=bus.call_sync('org.gnome.Mutter.DisplayConfig','/org/gnome/Mutter/DisplayConfig',
 'org.gnome.Mutter.DisplayConfig','GetCurrentState',None,None,0,10000,None).unpack()
outputs=[]
for x,y,scale,transform,primary,members,props in logical:
 for member in members:
  connector=member[0]
  monitor=next(m for m in monitors if m[0][0]==connector)
  mode=next(m for m in monitor[1] if m[6].get('is-current'))
  w,h=mode[1:3]
  if transform%2:w,h=h,w
  outputs.append(dict(name=connector,x=x,y=y,pixelWidth=w,pixelHeight=h,scale=scale,transform=transform))
print(json.dumps(outputs))
'''


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def bounds(monitors):
    left = min(m['x'] for m in monitors); top = min(m['y'] for m in monitors)
    right = max(m['x'] + m['width'] for m in monitors)
    bottom = max(m['y'] + m['height'] for m in monitors)
    return left, top, right - left, bottom - top


def selection(case, monitors):
    if case.startswith('inside-'):
        m = monitors[int(case[-1])]
        return [m['x'] + 80, m['y'] + 80, 160, 120]
    a, b = monitors[:2]
    # Select between interior points, so all four corners are reachable even
    # when the bounding rectangle includes a real gap or a head left/up.
    points = [(m['x'] + m['width'] // 2, m['y'] + m['height'] // 2) for m in (a, b)]
    x, y = min(p[0] for p in points), min(p[1] for p in points)
    return [x, y, max(80, abs(points[1][0] - points[0][0])), max(60, abs(points[1][1] - points[0][1]))]


def pixel_oracle(image, monitors, rect, densities):
    """Check every pixel against the known fullscreen GTK checkerboards.

    Region coordinates are logical. Pixel dimensions follow the largest native
    density of participating outputs. Gaps must be transparent. This reports
    exact mismatch counts; there is no percentage/tolerance pass threshold.
    """
    from PIL import Image
    im = Image.open(image).convert('RGBA')
    x, y, width, height = rect
    participating = [(i, m) for i, m in enumerate(monitors) if
                     m['x'] < x + width and m['x'] + m['width'] > x and
                     m['y'] < y + height and m['y'] + m['height'] > y]
    density = max(densities[i] for i, _ in participating)
    expected_size = (math.ceil((x + width) * density) - math.floor(x * density),
                     math.ceil((y + height) * density) - math.floor(y * density))
    if im.size != expected_size:
        return {'ok': False, 'actualSize': list(im.size), 'expectedSize': list(expected_size), 'reason': 'pixel-size'}
    pixels = im.load(); mismatches = 0; first = []; gaps = 0
    for py in range(im.height):
        gy = (math.floor(y * density) + py + .5) / density
        for px in range(im.width):
            gx = (math.floor(x * density) + px + .5) / density
            expected = (0, 0, 0, 0)
            for i, m in participating:
                if m['x'] <= gx < m['x'] + m['width'] and m['y'] <= gy < m['y'] + m['height']:
                    index = (i * 4 + int((gx - m['x']) // 128) % 2 + 2 * (int((gy - m['y']) // 128) % 2)) % len(COLORS)
                    expected = (*COLORS[index], 255)
                    break
            if expected[3] == 0:
                gaps += 1
            actual = pixels[px, py]
            if actual != expected:
                mismatches += 1
                if len(first) < 8:
                    first.append({'pixel': [px, py], 'actual': list(actual), 'expected': list(expected)})
    return {'ok': mismatches == 0, 'pixels': im.width * im.height, 'mismatches': mismatches,
            'transparentGapPixels': gaps, 'firstMismatches': first, 'size': list(im.size)}


class Campaign:
    def __init__(self, args):
        self.args = args; self.run = Path(args.run).resolve()
        self.out = Path(args.output).resolve()
        if self.out.exists():
            raise RuntimeError('Output directory must be new')
        self.out.mkdir(mode=0o700)
        self.remote = '/home/' + lab.state(self.run)['guest_user'] + '/putgd-lab/mh-' + self.out.name
        # Resolve all control socket paths before launching any isolated app/fixture.
        resolved = json.loads(lab.ssh(self.run, '''python3 - <<'PY'
import json,os
print(json.dumps([os.environ.get('XDG_RUNTIME_DIR','/run/user/%d'%os.getuid())+'/putgd/session.sock']))
PY
'''))
        for value in [str(self.run / 'qmp.sock'), *resolved]:
            if len(value.encode('utf-8')) > 107:
                raise RuntimeError('Unix socket exceeds 107 UTF-8 bytes: ' + value)
        save(self.out / 'socket-preflight.json', {'paths': [{'path': p, 'bytes': len(p.encode())} for p in [str(self.run / 'qmp.sock'), *resolved]]})
        self.fixture_pid = None
        self.fixture_remote = self.remote
        self.recording_id = None
        self.results = []

    def session(self, script):
        return lab.guest_session(self.run, script)

    def cli(self, method, params=None):
        return lab.cli(self.run, method, params)

    def fetch_json(self, remote, local):
        lab.get(self.run, remote, local)
        return json.loads(Path(local).read_text())

    def shots(self, label):
        result = []
        for i in range(len(lab.state(self.run).get('heads', [0]))):
            try:
                shot = lab.screenshot(self.run, 'mh-' + self.out.name + '-' + label, i)
            except lab.LabError as error:
                if getattr(self, 'current', None) == 'output-change':
                    result.append({'head': i, 'noFrameAfterOutputChange': True, 'error': str(error)})
                    continue
                raise
            path = Path(shot['path'])
            target = self.out / path.name
            shutil.copyfile(path, target)
            result.append({'path': target.name, 'sha256': lab.hash_file(target)})
        return result

    def click(self, x, y):
        left, top, width, height = bounds(self.monitors)
        for down in (True, False):
            lab.pointer(self.run, x - left, y - top, width, height, down)
            time.sleep(.1)

    def drag(self, rect):
        left, top, width, height = bounds(self.monitors)
        x, y, w, h = rect
        try:
            lab.pointer(self.run, x - left, y - top, width, height, True); time.sleep(.15)
            for i in range(1, 9):
                lab.pointer(self.run, x + w * i / 8 - left, y + h * i / 8 - top, width, height); time.sleep(.03)
            time.sleep(.15)
            self.shots('drag-' + self.current)
        finally:
            lab.pointer(self.run, x + w - left, y + h - top, width, height, False)

    def wait_operation(self, ident):
        deadline = time.monotonic() + 25
        while True:
            value = self.cli('operation.inspect', {'id': ident})
            if value['state'] in TERMINAL and value.get('copyStatus') != 'pending':
                return value
            if time.monotonic() > deadline:
                raise RuntimeError('Operation did not settle: ' + ident)
            time.sleep(.15)

    def stop_fixture(self):
        if not self.fixture_pid:
            return
        script = r'''python3 - <<'PY'
import os,signal,time,json
from pathlib import Path
pid=%d
p=Path('/proc')/str(pid)
if p.exists():
 args=(p/'cmdline').read_bytes().split(b'\0')
 assert b'/tmp/putgd-multi-output-fixture.py' in args, 'Recorded PID no longer belongs to fixture'
 os.kill(pid,signal.SIGTERM)
 deadline=time.monotonic()+5
 while p.exists() and time.monotonic()<deadline:time.sleep(.1)
 if p.exists() and (p/'stat').read_text().rpartition(')')[2].split()[0]!='Z':os.kill(pid,signal.SIGKILL)
print(json.dumps({'pid':pid,'stopped':True}))
PY
''' % self.fixture_pid
        self.session(script)
        self.fixture_pid = None

    def fixture(self, motion=False, minimum_heads=2):
        self.stop_fixture()
        self.fixture_remote = self.remote + ('-motion' if motion else '')
        source = Path(__file__).with_name('multi_output_fixture.py')
        lab.put(self.run, source, '/tmp/putgd-multi-output-fixture.py')
        self.fixture_pid = int(self.session('nohup python3 /tmp/putgd-multi-output-fixture.py --output ' + shlex.quote(self.fixture_remote) +
                                           (' --motion' if motion else '') + ' > ' + shlex.quote(self.fixture_remote + '.log') + ' 2>&1 < /dev/null & echo $!\n').strip().splitlines()[-1])
        deadline = time.monotonic() + 12
        while True:
            try:
                ready = self.fetch_json(self.fixture_remote + '/ready.json', self.out / ('fixture-motion.json' if motion else 'fixture.json'))
                break
            except (lab.LabError, ValueError, OSError):
                if time.monotonic() > deadline:
                    raise RuntimeError('Native checkerboard readiness timeout')
                time.sleep(.2)
        self.monitors = ready['monitors']
        if len(self.monitors) < minimum_heads or not all(m['fullscreen'] for m in self.monitors):
            raise RuntimeError('Separately fullscreen native output fixtures required on every enabled head')
        if self.profile['session'] == 'wayland' and 'Wayland' not in ready['backend']:
            raise RuntimeError('Fixture is not native Wayland')
        if any(m['allocatedWidth'] != m['width'] or m['allocatedHeight'] != m['height'] for m in self.monitors):
            raise RuntimeError('Fullscreen fixture size differs from its actual native output')
        if any(m['placedRect'] != [m['x'], m['y'], m['width'], m['height']] for m in self.monitors):
            raise RuntimeError('Fullscreen fixture was placed on a different native output')
        self.shots('fixtures-ready')

    def setup(self):
        self.profile = lab.load_profile(lab.state(self.run)['profile'])
        if self.args.appimage:
            package = lab.install_appimage(self.run, self.args.appimage)
            save(self.run / 'package.json', package)
        else:
            package = json.loads((self.run / 'package.json').read_text())
        if package.get('format') == 'appimage':
            original = lab.load_profile
            lab.load_profile = lambda name: {**original(name), 'install': 'appimage'}
        self.package = package
        if self.args.appimage:
            lab.start(self.run)
        capabilities = self.cli('capabilities')
        running = lab.verify_running_app(self.run)
        save(self.out / 'identity.json', {'package': package, 'resident': running,
            'capabilities': capabilities, 'run': lab.state(self.run), 'driverSha256': lab.hash_file(__file__),
            'nativeAdapter': self.profile.get('nativeAdapter')})
        # Retain loginctl/native compositor identity independently of app capability claims.
        native = self.session('''python3 - <<'PY'
import subprocess,os
from pathlib import Path
pid=subprocess.check_output(['pgrep','-u',str(os.getuid()),'-x',%s],text=True).split()[0]
env=dict(x.split('=',1) for x in Path('/proc/'+pid+'/environ').read_bytes().decode().split('\\0') if '=' in x)
session=env.get('XDG_SESSION_ID')
if not session:raise RuntimeError('Native compositor process has no session identity')
subprocess.run(['loginctl','show-session',session,'-p','Type','-p','Desktop','-p','Active','-p','State'],check=True)
print(os.uname())
PY
''' % repr(self.profile['session_process']))
        (self.out / 'native-session.txt').write_text(native)
        settings = self.cli('settings.get')
        self.cli('settings.patch', {'expectedRevision': settings['metadata']['acceptedRevision'],
            'patch': {'actions': {action: {'destination': 'local', 'shortcut': ''} for action in ('region', 'window', 'desktop')},
                      'archive': {'root': self.remote + '-archive', 'keepUploads': True}}})
        # Fullscreen readiness and the independent witnesses establish coverage.
        self.fixture()
        if self.profile['session'] == 'x11':
            native = lab.guest_outputs(self.run, 'xrandr')
            self.native_outputs = [{**o, 'scale': 1.0, 'pixelWidth': o['width'], 'pixelHeight': o['height']}
                                   for o in native['outputs'] if o['enabled']]
        elif self.profile['desktop'] == 'KDE':
            native = json.loads(self.session('kscreen-doctor -j\n'))
            self.native_outputs = [{'name': o['name'], 'x': o['pos']['x'], 'y': o['pos']['y'],
                'scale': o['scale'], 'pixelWidth': o['size']['width'], 'pixelHeight': o['size']['height'],
                'rotation': o['rotation']} for o in native['outputs'] if o['enabled']]
        else:
            native = self.native_outputs = json.loads(self.session('python3 -c ' + shlex.quote(MUTTER_OUTPUTS) + '\n'))
        save(self.out / 'native-outputs.json', native)
        self.densities = []
        for m in self.monitors:
            found = [o for o in self.native_outputs if o['x'] == m['x'] and o['y'] == m['y'] and
                     math.ceil(o['pixelWidth'] / o['scale']) == m['width'] and math.ceil(o['pixelHeight'] / o['scale']) == m['height']]
            if len(found) != 1:
                raise RuntimeError('Native output and GTK geometry do not agree uniquely')
            self.densities.append(found[0]['scale'])
            m['connector'] = found[0]['name']
        save(self.out / 'actual-geometry.json', {'monitors': self.monitors, 'nativeDensities': self.densities})

    def wait_recording(self, ident, states, timeout=30):
        deadline = time.monotonic() + timeout
        while True:
            value = self.cli('recording.inspect', {'id': ident})
            if value['state'] in states:
                return value
            if value['state'] == 'failed' or time.monotonic() > deadline:
                raise RuntimeError('Recording state: ' + json.dumps(value))
            time.sleep(.2)

    def record_clip(self, label):
        rect = selection('inside-1', self.monitors)
        prepared = self.cli('recording.prepare', {'target': {'kind': 'region', 'rect': rect}, 'format': 'mp4', 'fps': 30, 'audio': {'system': False}})
        ident = self.recording_id = prepared['recordingId']
        result = {'id': ident, 'rect': rect}
        try:
            result['ready'] = self.wait_recording(ident, ('ready',))
            result['readyWitnesses'] = self.shots(label + '-ready')
            self.cli('recording.start', {'id': ident})
            time.sleep(1)
            result['recording'] = self.cli('recording.inspect', {'id': ident})
            result['recordingWitnesses'] = self.shots(label + '-recording')
            time.sleep(3)
            self.cli('recording.stop', {'id': ident})
            result['review'] = self.wait_recording(ident, ('reviewing',), 60)
            path = self.remote + '-' + label + '.mp4'
            self.cli('recording.saveCopy', {'id': ident, 'path': path})
            deadline = time.monotonic() + 90
            while True:
                value = self.cli('recording.inspect', {'id': ident})
                if (value.get('lastExport') or {}).get('status') == 'saved':
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('Recording export timeout')
                time.sleep(.2)
            lab.get(self.run, path, self.out / (label + '.mp4'))
            prefix = ''
            helper = 'putgd-record'
            if self.package.get('format') == 'appimage':
                root = self.package.get('analysisRoot')
                prefix = ('root=' + shlex.quote(root) if root else 'root="$HOME/putgd-lab/appimage/squashfs-root"') + '\nexport LD_LIBRARY_PATH="$root/usr/lib:$root/usr/lib/putgd"\n'
                helper = '"$root/usr/bin/putgd-record"'
            motion = self.session(prefix + helper + ' --motion ' + shlex.quote(path) + ' 8\n')
            result['motionOutput'] = motion
            result['motion'] = float(re.search(r'motion=([\d.]+)', motion)[1])
            result['placementOk'] = result['recording'].get('overlayPlacement') in ('outside', 'hidden')
            return result
        finally:
            self.cli('recording.cancel', {'id': ident})
            self.recording_id = None

    def recording(self):
        result = {'case': 'record-region', 'ok': False}; self.results.append(result)
        try:
            result['static'] = self.record_clip('static')
            self.fixture(motion=True)
            result['moving'] = self.record_clip('moving')
            result['ok'] = result['static']['motion'] < .5 and result['moving']['motion'] > max(1, result['static']['motion'] * 5) and result['moving']['placementOk']
        except Exception as error:
            result['error'] = str(error)
            result['failureWitnesses'] = self.shots('recording-failure')
        finally:
            save(self.out / 'record-region.json', result)

    def one(self, name):
        self.current = name
        result = {'case': name, 'ok': False}
        self.results.append(result)
        try:
            cancel = name in ('escape', 'right-click', 'focus-change', 'output-change')
            action = 'window' if name.startswith('window') else 'desktop' if name == 'desktop' else 'region'
            m = self.monitors[int(name[-1])] if name.startswith('window') else self.monitors[0]
            self.click(m['x'] + 40, m['y'] + 40)
            lab.key(self.run, ['ctrl', 'shift', 't']); time.sleep(.2)
            operation = self.cli('action.invoke', {'action': action})
            ident = operation.get('operationId', operation.get('id'))
            if not ident:
                raise RuntimeError('No production operation ID')
            result['operationId'] = ident
            time.sleep(.6)
            result['selectorWitnesses'] = self.shots('selector-' + name)
            rect = None
            if cancel:
                if name == 'escape':
                    lab.key(self.run, ['esc'])
                elif name == 'right-click':
                    for down in (True, False):
                        lab.input_events(self.run, [{'type': 'btn', 'data': {'button': 'right', 'down': down}}]); time.sleep(.05)
                elif name == 'focus-change':
                    lab.key(self.run, ['alt', 'tab'])
                else:
                    if not self.args.change_command:
                        self.cli('operation.cancel', {'id': ident})
                        result.update(blocked='No reviewed output-change command supplied'); return
                    body = Path(self.args.change_command).read_text()
                    (self.out / 'output-change-command.sh').write_text(body)
                    (self.out / 'output-change-result.txt').write_text(self.session(body))
                    save(self.out / 'outputs-after-change.json', lab.guest_outputs(self.run, lab.layout_backend(self.profile)))
                settled = self.wait_operation(ident)
                result['operation'] = settled
                result['ok'] = settled['state'] in ('cancelled', 'failed') and not settled.get('items')
                if settled['state'] == 'failed':
                    result['ok'] = result['ok'] and settled.get('error', {}).get('code') in ('display_changed', 'cancelled')
            else:
                if action == 'region':
                    rect = selection(name, self.monitors)
                    result['selection'] = rect
                    self.drag(rect)
                settled = self.wait_operation(ident)
                result['operation'] = settled
                if settled['state'] != 'completed' or len(settled.get('items', [])) != 1:
                    raise RuntimeError('Capture did not complete with one artifact')
                item = settled['items'][0]
                artifact = self.out / (name + '.png')
                lab.get(self.run, item['local']['path'], artifact)
                if lab.hash_file(artifact) != item['artifact']['sha256']:
                    raise RuntimeError('Saved artifact hash differs from operation')
                if action == 'desktop':
                    rect = list(bounds(self.monitors))
                elif action == 'window':
                    rect = [m[k] for k in ('x', 'y', 'width', 'height')]
                result['pixels'] = pixel_oracle(artifact, self.monitors, rect, self.densities)
                result['ok'] = result['pixels']['ok']
            # Paste into the independent GTK app with real keyboard input.
            self.session('rm -f ' + shlex.quote(self.fixture_remote + '/paste.json') + ' ' + shlex.quote(self.fixture_remote + '/paste.png') + '\n')
            self.click(self.monitors[0]['x'] + 40, self.monitors[0]['y'] + 40)
            lab.key(self.run, ['ctrl', 'v'])
            deadline = time.monotonic() + 5
            while True:
                try:
                    paste = self.fetch_json(self.fixture_remote + '/paste.json', self.out / (name + '-paste.json'))
                    break
                except (lab.LabError, ValueError, OSError):
                    if time.monotonic() > deadline:
                        raise RuntimeError('Focused independent GTK paste did not produce a witness')
                    time.sleep(.2)
            result['paste'] = paste
            if cancel:
                result['ok'] = result['ok'] and paste == {'kind': 'text', 'text': 'putgd multi-output cancellation sentinel'}
            else:
                pasted = self.out / (name + '-paste.png')
                lab.get(self.run, self.fixture_remote + '/paste.png', pasted)
                from PIL import Image
                a = Image.open(artifact).convert('RGBA'); b = Image.open(pasted).convert('RGBA')
                result['pastePixelsEqual'] = a.size == b.size and a.tobytes() == b.tobytes()
                result['ok'] = result['ok'] and result['pastePixelsEqual']
            result['finalWitnesses'] = self.shots('result-' + name)
        except Exception as error:
            result['error'] = str(error)
            try:
                lab.key(self.run, ['esc'])
                result['failureWitnesses'] = self.shots('failure-' + name)
                if result.get('operationId'):
                    result['finalOperation'] = self.cli('operation.inspect', {'id': result['operationId']})
                    if result['finalOperation']['state'] not in TERMINAL:
                        self.cli('operation.cancel', {'id': result['operationId']})
            except Exception as cleanup:
                result['cleanupError'] = str(cleanup)
        finally:
            save(self.out / (name + '.json'), result)

    def cleanup(self):
        errors = []
        if self.recording_id:
            try:
                self.cli('recording.cancel', {'id': self.recording_id})
            except Exception as error:
                errors.append('recording: ' + str(error))
        try:
            self.stop_fixture()
        except Exception as error:
            errors.append('fixture: ' + str(error))
        save(self.out / 'cleanup.json', {'ok': not errors, 'errors': errors})
        save(self.out / 'results.json', {'ok': not errors and bool(self.results) and all(r['ok'] for r in self.results),
                                       'results': self.results})
        save(self.out / 'sha256.json', {p.name: lab.hash_file(p) for p in self.out.iterdir() if p.is_file() and p.name != 'sha256.json'})
        if errors:
            raise RuntimeError('Campaign cleanup incomplete: ' + '; '.join(errors))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run'); p.add_argument('--output', required=True)
    p.add_argument('--case', action='append', choices=CASES)
    p.add_argument('--change-command', type=Path)
    p.add_argument('--appimage', type=Path)
    a = p.parse_args(); c = Campaign(a)
    try:
        c.setup()
        for name in a.case or CASES:
            c.recording() if name == 'record-region' else c.one(name)
    finally:
        c.cleanup()
    print(json.dumps({'ok': all(r['ok'] for r in c.results), 'output': str(c.out)}))
    return 0 if c.results and all(r['ok'] for r in c.results) else 1


if __name__ == '__main__':
    sys.exit(main())
