#!/usr/bin/env python3
"""Private QEMU/QMP graphical acceptance lab. Python stdlib only."""
import argparse
import datetime
import shutil
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent

class LabError(RuntimeError):
    pass


def read_json(path):
    return json.loads(Path(path).read_text())


def write_json(path, value):
    target = Path(path)
    temporary = target.with_suffix(target.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(target)


def test_event(run, event):
    """Append only controlled test metadata; never generic input or shell traces."""
    value = {'schemaVersion': 1, 'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(), **event}
    with (Path(run) / 'test-events.jsonl').open('a') as stream:
        stream.write(json.dumps(value) + '\n')
        stream.flush()
        os.fsync(stream.fileno())


def load_profile(name):
    if not re.fullmatch(r'[a-z][a-z0-9-]+', name):
        raise LabError('Invalid profile name')
    path = HERE / 'profiles' / (name + '.yaml')
    if not path.exists():
        raise LabError('Profile unavailable; see tools/lab/profiles')
    return read_json(path)  # JSON is a YAML subset; avoids a host package dependency.


def command(args, timeout=60, check=True, input=None):
    result = subprocess.run([str(a) for a in args], input=input, text=True,
                            capture_output=True, timeout=timeout)
    if check and result.returncode:
        # Commands and stderr can include guest data; never export them implicitly.
        raise LabError(f'{Path(str(args[0])).name} failed (exit {result.returncode})')
    return result


class QMP:
    def __init__(self, path, timeout=10):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(timeout)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile('rwb')
        self.events = []
        self.sequence = 0
        greeting = self._read()
        if 'QMP' not in greeting:
            self.close()
            raise LabError('Missing QMP greeting')
        self.version = greeting['QMP']['version']
        self.execute('qmp_capabilities')

    def _read(self):
        line = self.stream.readline(1024 * 1024)
        if not line or not line.endswith(b'\n'):
            raise LabError('QMP closed or oversized reply')
        return json.loads(line)

    def execute(self, name, arguments=None):
        self.sequence += 1
        request = {'execute': name, 'id': self.sequence}
        if arguments is not None:
            request['arguments'] = arguments
        self.stream.write(json.dumps(request).encode() + b'\n')
        self.stream.flush()
        while True:
            reply = self._read()
            if 'event' in reply:
                self.events.append(reply)
                continue
            if reply.get('id') != self.sequence:
                raise LabError('QMP response ID mismatch')
            if 'error' in reply:
                raise LabError('QMP: ' + reply['error'].get('desc', 'command failed'))
            return reply['return']

    def close(self):
        self.stream.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


def qemu_command(profile, run, overlay, ssh_port, vnc_display, acceleration):
    args = ['qemu-system-x86_64', '-machine', 'q35,accel=' + acceleration,
            '-cpu', 'host' if acceleration == 'kvm' else 'max',
            '-smp', str(profile['cpus']), '-m', str(profile['memory_mib']),
            '-device', profile['video'] + ',id=video0', '-device', 'usb-ehci', '-device', 'usb-tablet',
            '-drive', f'file={overlay},format=qcow2,if=virtio',
            '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22',
            '-device', 'virtio-net-pci,netdev=net0',
            '-qmp', f'unix:{run}/qmp.sock,server=on,wait=off',
            '-vnc', f'127.0.0.1:{vnc_display}', '-display', 'none',
            '-daemonize', '-pidfile', str(run / 'qemu.pid')]
    return args


def validate_readiness(profile, actual, acceleration):
    for key in ('session', 'qt_platform', 'desktop'):
        wanted = profile[key]
        if key == 'desktop':
            valid = wanted in actual.get(key, '').split(':')
        else:
            valid = actual.get(key) == wanted
        if not valid:
            raise LabError(f'Native {key} mismatch: requires {wanted}')
    if not actual.get('display_layout'):
        raise LabError('No graphical display layout')
    return {'evidence_class': 'graphical-kvm' if acceleration == 'kvm' else 'functional-only',
            'performance_eligible': False}


def ppm(path):
    data = Path(path).read_bytes()
    # P6 whitespace/comments are legal in the header; binary bytes stay untouched.
    pattern = rb'P6\s+(?:#[^\n]*\n\s*)?(\d+)\s+(\d+)\s+255[ \t\r\n]'
    match = re.match(pattern, data)
    if not match:
        raise LabError('Expected an 8-bit binary PPM image')
    width, height = map(int, match.groups())
    pixels = data[match.end():]
    if len(pixels) != width * height * 3:
        raise LabError('PPM payload length mismatch')
    return width, height, pixels


def compare_ppm(path, expected):
    width, height, pixels = ppm(path)
    errors = []
    if (width, height) != (expected['width'], expected['height']):
        errors.append('dimensions')
    for sample in expected.get('samples', []):
        x, y = sample['x'], sample['y']
        offset = (y * width + x) * 3
        if not (0 <= x < width and 0 <= y < height) or list(pixels[offset:offset+3]) != sample['rgb']:
            errors.append(f'pixel({x},{y})')
    return {'ok': not errors, 'errors': errors, 'width': width, 'height': height,
            'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}


def locate_fixture_paint(path):
    # Wayland deliberately does not expose a client's global window origin.
    # Locate the independent four-colour fixture in the pre-action QMP frame,
    # never in the capture being tested. Require the whole pattern, uniquely.
    width, height, pixels = ppm(path)
    top = bytes([220, 30, 40]) * 320 + bytes([20, 180, 70]) * 320
    bottom = bytes([35, 70, 220]) * 320 + bytes([235, 190, 20]) * 320
    matches = []
    offset = pixels.find(top)
    while offset >= 0:
        y, remainder = divmod(offset, width * 3)
        x, alignment = divmod(remainder, 3)
        if not alignment and x + 640 <= width and y + 320 <= height:
            if all(pixels[offset + row * width * 3:offset + row * width * 3 + len(top)]
                   == (top if row < 160 else bottom) for row in range(320)):
                matches.append({'x': x, 'y': y, 'width': 640, 'height': 320})
        offset = pixels.find(top, offset + len(top))
    return matches[0] if len(matches) == 1 else None


def compare_paste(actual, expected):
    return {'ok': all(actual.get(key) == value for key, value in expected.items()),
            'expected': expected, 'actual': actual}


def verify_publication(operation, artifacts, public_files, paste):
    items = operation.get('items', [])
    errors = []
    if operation.get('state') != 'completed' or not items or len(items) != len(artifacts) or len(items) != len(public_files):
        return {'ok': False, 'errors': ['incomplete publication']}
    urls = []
    for index, item in enumerate(items):
        expected = item['artifact']['sha256']
        if item.get('uploadStatus') != 'uploaded' or hash_file(artifacts[index]) != expected or hash_file(public_files[index]) != expected:
            errors.append(f'publication bytes {index}')
        urls.append(item.get('receipt', {}).get('directUrl', ''))
    if not all(urls) or paste.get('text') != '\n'.join(urls):
        errors.append('independent direct URL paste')
    return {'ok': not errors, 'errors': errors, 'urls': urls}


def state(run):
    return read_json(Path(run) / 'state.json')


def ssh_args(run, bootstrap=False):
    info = state(run)
    return ['ssh', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=5',
            '-o', 'UserKnownHostsFile=' + info['known_hosts'],
            '-i', info['bootstrap_key'] if bootstrap else str(Path(run) / 'identity'),
            '-p', str(info['ssh_port']), info['guest_user'] + '@127.0.0.1']


def ssh(run, script, timeout=60, bootstrap=False):
    return command(ssh_args(run, bootstrap) + ['sh', '-s'], timeout=timeout, input=script).stdout


def scp_connection(run):
    info = state(run)
    arguments = ['scp', '-q', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
                 '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + info['known_hosts'],
                 '-i', Path(run) / 'identity', '-P', str(info['ssh_port'])]
    return arguments, f"{info['guest_user']}@127.0.0.1:"


def put(run, local, remote):
    arguments, endpoint = scp_connection(run)
    command(arguments + [local, endpoint + str(remote)])


def get(run, remote, local):
    arguments, endpoint = scp_connection(run)
    command(arguments + [endpoint + str(remote), local])


def guest_session(run, body):
    # Discover the real desktop session's environment, not SSH's empty DISPLAY.
    info = state(run)
    user = info['guest_user']
    profile = load_profile(info['profile'])
    process = profile['session_process']
    if process not in ('cinnamon', 'gnome-shell', 'plasmashell'):
        raise LabError('Unsupported native desktop session process')
    script = f'''set -eu
pid=$(pgrep -u {shlex.quote(user)} -x {shlex.quote(process)} | head -n 1)
[ -n "$pid" ]
for name in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY; do
 value=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n "s/^$name=//p")
 [ -z "$value" ] || export "$name=$value"
done
export QT_QPA_PLATFORM={shlex.quote(profile["qt_platform"])}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export XDG_CONFIG_HOME=$HOME/putgd-lab/config
export XDG_DATA_HOME=$HOME/putgd-lab/data
{body}
'''
    return ssh(run, script)


def launch(args):
    profile = load_profile(args.profile)
    golden = Path(args.golden).resolve()
    if not golden.is_file():
        raise LabError('Install and shut down a golden guest first; see docs/vm-lab.md')
    if args.acceleration == 'kvm' and not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise LabError('KVM unavailable; request explicit --acceleration tcg for functional-only evidence')
    run = Path(args.run).resolve()
    run.mkdir(parents=True, mode=0o700, exist_ok=False)
    os.chmod(run, 0o700)
    overlay = run / 'disk.qcow2'
    command(['qemu-img', 'create', '-f', 'qcow2', '-F', 'qcow2', '-b', golden, overlay])
    command(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', run / 'identity'])
    known_hosts = run / 'known_hosts'
    known_hosts.write_bytes(Path(args.known_hosts).read_bytes())
    info = {'schemaVersion': 1, 'id': str(uuid.uuid4()), 'profile': args.profile,
            'guest_user': profile['guest_user'], 'ssh_port': args.ssh_port,
            'vnc_display': args.vnc_display, 'acceleration': args.acceleration,
            'golden': str(golden), 'golden_sha256': hash_file(golden),
            'bootstrap_key': str(Path(args.bootstrap_key).resolve()),
            'known_hosts': str(known_hosts), 'overlay': str(overlay)}
    write_json(run / 'state.json', info)
    result = command(qemu_command(profile, run, overlay, args.ssh_port,
                                  args.vnc_display, args.acceleration), timeout=15, check=False)
    (run / 'qemu.log').write_text(result.stderr)
    if result.returncode:
        raise LabError('QEMU startup failed; private qemu.log preserved')
    info['pid'] = int((run / 'qemu.pid').read_text().strip())
    write_json(run / 'state.json', info)
    try:
        deadline = time.monotonic() + args.timeout
        while True:
            if not Path(f"/proc/{info['pid']}").exists():
                raise LabError('QEMU exited during boot; private qemu.log preserved')
            try:
                ssh(run, 'true\n', bootstrap=True)
                break
            except (LabError, subprocess.TimeoutExpired):
                if time.monotonic() >= deadline:
                    raise LabError('Guest SSH boot timeout')
                time.sleep(2)
        public_key = (run / 'identity.pub').read_text().strip()
        ssh(run, 'umask 077\nmkdir -p ~/.ssh\nprintf "%s\\n" ' + shlex.quote(public_key) + ' >> ~/.ssh/authorized_keys\n', bootstrap=True)
        with QMP(run / 'qmp.sock') as qmp:
            features = qmp.execute('query-commands')
            acceleration_state = qmp.execute('query-kvm')
            write_json(run / 'qmp-features.json', {'version': qmp.version, 'commands': features, 'kvm': acceleration_state})
        return {'run': str(run), 'id': info['id'], 'display': f'127.0.0.1:{5900 + args.vnc_display}',
                'status': 'booted-not-accepted'}
    except BaseException:
        reset(run)
        raise


def hash_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def reset(run):
    run = Path(run)
    info = state(run)
    pid = info.get('pid')
    # Never kill a reused PID: verify both executable and owned disk argument.
    try:
        cmdline = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        owned = any(str(run / 'disk.qcow2').encode() in value for value in cmdline)
        if not owned or not Path(cmdline[0].decode()).name.startswith('qemu-system'):
            return {'status': 'already-stopped'}
        try:
            with QMP(run / 'qmp.sock', timeout=2) as qmp:
                qmp.execute('quit')
        except (OSError, LabError):
            os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + 5
        while Path(f'/proc/{pid}').exists() and time.monotonic() < deadline:
            time.sleep(.1)
        if Path(f'/proc/{pid}').exists():
            os.kill(pid, signal.SIGKILL)
    except FileNotFoundError:
        pass
    return {'status': 'stopped', 'evidence': str(run)}


def input_events(run, events):
    with QMP(Path(run) / 'qmp.sock') as qmp:
        qmp.execute('input-send-event', {'events': events})


def key(run, keys):
    with QMP(Path(run) / 'qmp.sock') as qmp:
        qmp.execute('input-send-event', {'events': [
            {'type': 'key', 'data': {'down': True, 'key': {'type': 'qcode', 'data': value}}}
            for value in keys]})
        try:
            time.sleep(.05)
        finally:
            qmp.execute('input-send-event', {'events': [
                {'type': 'key', 'data': {'down': False, 'key': {'type': 'qcode', 'data': value}}}
                for value in reversed(keys)]})


def pointer(run, x, y, width, height, down=None):
    events = [{'type': 'abs', 'data': {'axis': 'x', 'value': round(x * 32767 / (width - 1))}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': round(y * 32767 / (height - 1))}}]
    if down is not None:
        events.append({'type': 'btn', 'data': {'button': 'left', 'down': down}})
    input_events(run, events)


def screenshot(run, name='witness', head=0):
    path = (Path(run) / f'{name}-head{head}.ppm').resolve()
    if path.parent != Path(run).resolve():
        raise LabError('Witness filename must stay in run directory')
    with QMP(Path(run) / 'qmp.sock') as qmp:
        qmp.execute('screendump', {'filename': str(path), 'device': 'video0', 'head': head})
    return {'path': str(path), 'sha256': hash_file(path)}


def collect(run):
    versions = guest_session(run, '''printf '{"kernel":'
uname -r | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read().strip()),end="")'
printf ',"packages":'
if command -v dpkg-query >/dev/null; then
 dpkg-query -W -f='${Package} ${Version}\\n' 'cinnamon' 'muffin' 'gnome-shell' 'mutter*' 'gjs' 'plasma-workspace' 'kwin-wayland' 'xdg-desktop-portal*' 'libqt6core6*' 'putgd' 2>/dev/null || true
else
 rpm -q gnome-shell mutter gjs plasma-workspace kwin-wayland xdg-desktop-portal qt6-qtbase putgd 2>/dev/null || true
fi | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read().splitlines()),end="")'
printf '}\\n'
''')
    write_json(Path(run) / 'versions.json', json.loads(versions))
    diagnostics = json.loads(guest_session(run, 'putgd-cli diagnostics\n'))
    # This production diagnostics method is explicitly content/credential-free.
    write_json(Path(run) / 'diagnostics.json', diagnostics)
    return {'status': 'collected', 'evidence': str(Path(run).resolve())}


def cli(run, method, params=None):
    result = json.loads(guest_session(run, 'putgd-cli ' + shlex.quote(method) + ' ' + shlex.quote(json.dumps(params or {})) + '\n'))
    if not result.get('ok'):
        raise LabError('Production CLI: ' + result.get('error', {}).get('code', 'unknown'))
    return result['result']


def verify_running_app(run):
    """Reject an old resident inode even when apt installed the new package."""
    receipt = Path(run) / 'package.json'
    if not receipt.is_file():
        raise LabError('Verified package receipt missing; install the requested package first')
    expected = read_json(receipt).get('installedBinarySha256', {}).get('/usr/bin/putgd')
    if not isinstance(expected, str) or not re.fullmatch(r'[0-9a-f]{64}', expected):
        raise LabError('Verified package receipt has no application identity')
    output = ssh(run, """python3 - <<'PY'
import hashlib, json, os
from pathlib import Path
processes = []
for process in Path('/proc').iterdir():
    if not process.name.isdecimal():
        continue
    try:
        if process.stat().st_uid != os.getuid() or (process / 'comm').read_text().strip() != 'putgd':
            continue
        before = (process / 'stat').read_text().rsplit(')', 1)[1].split()[19]
        digest = hashlib.sha256()
        with (process / 'exe').open('rb') as stream:
            for block in iter(lambda: stream.read(1048576), b''):
                digest.update(block)
        after = (process / 'stat').read_text().rsplit(')', 1)[1].split()[19]
        if before != after:
            raise RuntimeError('Resident identity changed during collection')
        processes.append({'pid': int(process.name), 'startTicks': int(before), 'sha256': digest.hexdigest()})
    except (FileNotFoundError, ProcessLookupError):
        continue
print(json.dumps(processes))
PY
""")
    processes = json.loads(output)
    if len(processes) != 1:
        raise LabError('Expected exactly one lab resident; stop stale test processes before retrying')
    actual = processes[0]
    if actual.get('sha256') != expected:
        raise LabError('The running app differs from the installed package; restart the lab resident')
    return actual


def fixture_run(args):
    run = Path(args.run).resolve()
    test_id = uuid.uuid4().hex
    started = time.monotonic()
    artifacts = []
    public_files = []
    for stale in ('parameters.json', 'paste.json', 'paste.ppm', 'artifact.ppm', 'before-head0.ppm', 'final-head0.ppm'):
        (run / stale).unlink(missing_ok=True)
    test_event(run, {'event': 'started', 'testId': test_id, 'profile': state(run)['profile'],
        'scenario': args.scenario, 'clipboardClient': args.clipboard_client, 'timeoutSeconds': args.timeout})
    result = {'schemaVersion': 1, 'testId': test_id, 'profile': state(run)['profile'],
              'scenario': args.scenario + '-independent-paste', 'ok': False,
              'acceptanceScope': 'single-head named action; no full profile certification'}
    try:
        guest_session(run, '''set -eu
mkdir -p ~/putgd-lab/fixture
pkill -x putgd-desktop-f || true
pkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true
rm -f ~/putgd-lab/fixture/ready.json
nohup putgd-launcher --background > ~/putgd-lab/app.log 2>&1 < /dev/null &
nohup putgd-desktop-fixture --output "$HOME/putgd-lab/fixture" > ~/putgd-lab/fixture.log 2>&1 < /dev/null &
''')
        deadline = time.monotonic() + args.timeout
        while True:
            try:
                get(run, 'putgd-lab/fixture/ready.json', run / 'ready.json')
                settings = cli(run, 'settings.get')
                if not cli(run, 'diagnostics').get('journalReady'):
                    raise LabError('Resident journal initialization pending')
                break
            except (LabError, subprocess.TimeoutExpired):
                if time.monotonic() >= deadline:
                    raise LabError('Installed app/fixture readiness timeout')
                time.sleep(1)
        result['runningApp'] = verify_running_app(run)
        ready = read_json(run / 'ready.json')
        result.update(validate_readiness(load_profile(state(run)['profile']), ready, state(run)['acceleration']))
        result['clipboardClient'] = args.clipboard_client
        if args.clipboard_client == 'gtk':
            put(run, str(HERE / 'gtk_clipboard_fixture.py'), '/home/' + state(run)['guest_user'] + '/putgd-lab/gtk_clipboard_fixture.py')
            guest_session(run, '''set -eu
pkill -x putgd-desktop-f || true
rm -f ~/putgd-lab/fixture/gtk-ready.json
nohup python3 ~/putgd-lab/gtk_clipboard_fixture.py --output "$HOME/putgd-lab/fixture" > ~/putgd-lab/gtk-fixture.log 2>&1 < /dev/null &
''')
            while True:
                try:
                    get(run, 'putgd-lab/fixture/gtk-ready.json', run / 'gtk-ready.json')
                    gtk = read_json(run / 'gtk-ready.json')
                    if ready['session'] == 'wayland' and 'Wayland' not in gtk['displayBackend']:
                        raise LabError('GTK clipboard client is not native Wayland')
                    result['gtkClient'] = gtk
                    break
                except (LabError, subprocess.TimeoutExpired):
                    if time.monotonic() >= deadline:
                        raise LabError('Independent GTK clipboard client readiness timeout')
                    time.sleep(.2)
        actions = ('region', 'window', 'desktop', 'clipboard')
        profile = load_profile(state(run)['profile'])
        hotkeys = profile.get('fixture_hotkeys', {action: {'shortcut': f'Ctrl+Shift+{index}',
            'keys': ['ctrl', 'shift', str(index)]} for index, action in enumerate(actions, 1)})
        if set(hotkeys) != set(actions) or any(not entry.get('shortcut') or not entry.get('keys') for entry in hotkeys.values()):
            raise LabError('Profile fixture hotkeys must specify all four actions')
        patch = {'actions': {action: {'destination': 'upload' if args.settings else 'local',
                    'shortcut': hotkeys[action]['shortcut']} for action in actions},
                 'archive': {'root': '/home/' + state(run)['guest_user'] + '/putgd-lab/archive', 'keepUploads': True}}
        if args.settings:
            patch.update(read_json(args.settings))
            for profile in patch.get('profiles', []):
                credentials = profile.get('credentials', {})
                if credentials.get('mode') != 'keychain' or set(credentials) != {'mode', 'reference'}:
                    raise LabError('Lab settings require credential references; no session secrets')
                destination = profile.get('settings', {})
                destination['objectPrefix'] = destination.get('objectPrefix', '').rstrip('/') + '/putgd-lab/' + state(run)['id'] + '/'
        cli(run, 'settings.patch', {'expectedRevision': settings['metadata']['acceptedRevision'], 'patch': patch})
        binding_deadline = min(deadline, time.monotonic() + 12)
        while True:
            bindings = cli(run, 'capabilities')['source']['shortcuts']['bindings']
            if all(bindings.get(action, {}).get('bound') for action in actions):
                break
            if time.monotonic() >= binding_deadline:
                result['shortcutBindings'] = bindings
                raise LabError('Configured native fixture shortcuts did not bind')
            time.sleep(.1)
        result['shortcutBindings'] = bindings
        parameters = {'schemaVersion': 1, 'profile': state(run)['profile'],
            'scenario': args.scenario, 'clipboardClient': args.clipboard_client,
            'destination': 'upload' if args.settings else 'local',
            'runningApp': result['runningApp'],
            'runnerSha256': hash_file(Path(__file__)), 'hotkeys': hotkeys,
            'displayLayout': ready['display_layout'], 'qtVersion': ready['qt_version'],
            'acceleration': state(run)['acceleration'],
            'goldenSha256': state(run).get('golden_sha256'),
            'timingSeconds': {'keyHold': .05, 'focusPoll': .1, 'focusAttemptLimit': 2,
                'clipboardSourceSettle': .3, 'selectorSettle': .5, 'operationPoll': .2},
            'selection': {'fixtureOffset': {'x': 30, 'y': 30}, 'width': 80, 'height': 60},
            'timeoutSeconds': args.timeout,
            'replayCommand': [sys.executable, 'tools/lab/lab.py', 'run', '--run', '<disposable-run>',
                '--scenario', args.scenario, '--clipboard-client', args.clipboard_client,
                '--timeout', str(args.timeout), '--keep']}
        package_path = run / 'package.json'
        if package_path.exists():
            parameters['package'] = read_json(package_path)
        write_json(run / 'parameters.json', parameters)
        before = cli(run, 'operation.recent')
        before_ids = {operation['id'] for operation in before['operations']}
        geometry = ready['paint']
        screen = ready['display_layout'][0]
        width, height = screen['width'], screen['height']
        x, y = geometry['x'] + 30, geometry['y'] + 30
        # Fresh setup can map after the fixture. Verify actual visibility rather
        # than assuming launch order or clicking a window hidden underneath it.
        for attempt in range(3):
            focus_deadline = min(deadline, time.monotonic() + 2)
            while True:
                witness = screenshot(run, 'focus-' + str(attempt))
                # GTK replaces the Qt fixture and has its own window position,
                # including on X11. Only the independent witness locates it.
                if ready['session'] == 'wayland' or args.clipboard_client == 'gtk':
                    located = locate_fixture_paint(witness['path'])
                    if located:
                        geometry = located
                        x, y = geometry['x'] + 30, geometry['y'] + 30
                        result['independentPaintGeometry'] = located
                        margins = ready.get('decorationMargins')
                        if margins and args.clipboard_client == 'qt':
                            ready['frame'] = {'x': geometry['x'] - margins['left'],
                                'y': geometry['y'] - margins['top'],
                                'width': 640 + margins['left'] + margins['right'],
                                'height': 420 + margins['top'] + margins['bottom']}
                            result['independentWindowFrame'] = ready['frame']
                visible = compare_ppm(witness['path'], {'width': width, 'height': height,
                    'samples': [{'x': x, 'y': y, 'rgb': [220, 30, 40]},
                                {'x': geometry['x'] + 350, 'y': y, 'rgb': [20, 180, 70]}]})
                if visible['ok'] or time.monotonic() >= focus_deadline:
                    break
                time.sleep(.1)
            if visible['ok']:
                break
            key(run, ['alt', 'tab'])
        else:
            raise LabError('Independent fixture is obscured; focus witnesses preserved')
        pointer(run, x, y, width, height, True)
        pointer(run, x, y, width, height, False)
        time.sleep(.2)
        if args.scenario == 'window' and state(run)['profile'] == 'kde-wayland':
            # Server decorations are absent from native Qt Wayland frameMargins.
            # Ask the compositor's independent, visible picker before capture.
            guest_session(run, 'rm -f ~/putgd-lab/window-oracle.json\n'
                'nohup busctl --user --json=short call org.kde.KWin /KWin org.kde.KWin queryWindowInfo '
                '> ~/putgd-lab/window-oracle.json 2> ~/putgd-lab/window-oracle.err < /dev/null &\n')
            time.sleep(.3)
            pointer(run, x, y, width, height, True)
            pointer(run, x, y, width, height, False)
            oracle_deadline = min(deadline, time.monotonic() + 10)
            while True:
                try:
                    document = json.loads(guest_session(run, 'cat ~/putgd-lab/window-oracle.json\n'))
                    values = document['data'][0]
                    if values['caption']['data'] != 'put.gd independent synthetic paint / paste fixture':
                        raise LabError('Independent KWin picker selected a different window')
                    frame = {field: values[field]['data'] for field in ('x', 'y', 'width', 'height')}
                    if not all(isinstance(value, (int, float)) for value in frame.values()) or frame['width'] <= 0 or frame['height'] <= 0:
                        raise LabError('Independent KWin frame geometry is invalid')
                    ready['frame'] = {field: round(value) for field, value in frame.items()}
                    result['independentWindowFrame'] = {**frame, 'oracle': 'pre-action KWin queryWindowInfo'}
                    break
                except (json.JSONDecodeError, KeyError, IndexError):
                    if time.monotonic() >= oracle_deadline:
                        raise LabError('Independent KWin window geometry query did not complete')
                    time.sleep(.1)
        parameters['selection'].update(start={'x': x, 'y': y},
            end={'x': x + 80, 'y': y + 60}, inputViewport={'width': width, 'height': height})
        parameters['independentFixtureGeometry'] = geometry
        if 'independentWindowFrame' in result:
            parameters['independentWindowFrame'] = result['independentWindowFrame']
        write_json(run / 'parameters.json', parameters)
        result['parameters'] = 'parameters.json'
        screenshot(run, 'before')
        scenario = args.scenario
        cancelled_region = scenario.startswith('region-cancel-')
        if cancelled_region:
            # An independent source must survive cancellation unchanged.
            key(run, ['ctrl', 'alt', 'shift', 't'])
            time.sleep(.3)
        if scenario.startswith('clipboard-'):
            source_key = {'clipboard-text': 't', 'clipboard-image': 'i', 'clipboard-files': 'f'}[scenario]
            key(run, ['ctrl', 'alt', 'shift', source_key])
            time.sleep(.3)
            key(run, hotkeys['clipboard']['keys'])
        else:
            key(run, hotkeys['region' if cancelled_region else scenario]['keys'])
            if cancelled_region:
                time.sleep(.5)
                screenshot(run, 'selector')
                if scenario == 'region-cancel-escape':
                    key(run, ['esc'])
                elif scenario == 'region-cancel-right-click':
                    input_events(run, [{'type': 'btn', 'data': {'button': 'right', 'down': True}}])
                    time.sleep(.05)
                    input_events(run, [{'type': 'btn', 'data': {'button': 'right', 'down': False}}])
                else:
                    pointer(run, x, y, width, height, True)
                    pointer(run, x, y, width, height, False)
            elif scenario == 'region':
                time.sleep(.5)
                pointer(run, x, y, width, height, True)
                pointer(run, x + 80, y + 60, width, height)
                pointer(run, x + 80, y + 60, width, height, False)
        operation = None
        while time.monotonic() < deadline:
            recent = cli(run, 'operation.recent')
            found = [item for item in recent['operations'] if item['id'] not in before_ids]
            if len(found) > 1:
                raise LabError('One native gesture created multiple operations')
            if found:
                operation = cli(run, 'operation.inspect', {'id': found[0]['id']})
                if operation['state'] in ('failed', 'partial', 'cancelled', 'completed') and operation.get('copyStatus') in ('copied', 'failed', 'suppressed', 'no_result'):
                    break
            time.sleep(.2)
        if operation:
            result['operationId'] = operation['id']
            result['operationState'] = operation['state']
            result['copyStatus'] = operation.get('copyStatus')
            result['itemOutcomes'] = [{'uploadStatus': item['uploadStatus'],
                'localStatus': item['local']['status'], 'error': item.get('error', {})}
                for item in operation['items']]
        if cancelled_region:
            if not operation or operation['state'] != 'cancelled' or operation['items']:
                raise LabError('Cancelled selector created artifacts or did not settle as cancelled')
            if operation.get('copyStatus') not in ('no_result', 'suppressed'):
                raise LabError('Cancelled selector left an unresolved copy status')
            guest_session(run, 'rm -f ~/putgd-lab/fixture/paste.json\n')
            key(run, ['ctrl', 'v'])
            time.sleep(.3)
            get(run, 'putgd-lab/fixture/paste.json', run / 'paste.json')
            result['paste'] = compare_paste(read_json(run / 'paste.json'),
                {'text': 'putgd synthetic clipboard text'})
            result['ok'] = result['paste']['ok']
            if not result['ok']:
                raise LabError('Selector cancellation changed the independent clipboard source')
            return result
        if not operation or operation['state'] != 'completed' or operation.get('copyStatus') != 'copied':
            raise LabError('Sharing operation did not complete with a copied result; inspect recorded stage outcomes')
        artifacts = []
        expected = None
        if scenario == 'region':
            expected = read_json(HERE.parent.parent / 'tests/desktop/common.yaml')['oracle']
        elif scenario == 'clipboard-image':
            expected = {'width': 96, 'height': 64, 'samples': [{'x': 20, 'y': 20, 'rgb': [220, 30, 40]}]}
        elif scenario in ('window', 'desktop'):
            rectangle = ready['frame'] if scenario == 'window' else screen
            expected = {'width': rectangle['width'], 'height': rectangle['height'], 'samples': [
                {'x': geometry['x'] + 30 - rectangle['x'], 'y': geometry['y'] + 30 - rectangle['y'], 'rgb': [220, 30, 40]}]}
        parameters['expectedArtifact'] = expected or {'syntheticBytes': scenario}
        write_json(run / 'parameters.json', parameters)
        for index, item in enumerate(operation['items']):
            if item['local']['status'] != 'saved':
                raise LabError('Preserved local result missing')
            artifact = run / f'artifact-{index}'
            get(run, item['local']['path'], artifact)
            artifacts.append(artifact)
            if hash_file(artifact) != item['artifact']['sha256']:
                raise LabError('Stored artifact hash mismatch')
        if expected:
            item = operation['items'][0]
            guest_session(run, 'putgd-desktop-fixture --convert ' + shlex.quote(item['local']['path']) + ' "$HOME/putgd-lab/fixture/artifact.ppm"\n')
            get(run, 'putgd-lab/fixture/artifact.ppm', run / 'artifact.ppm')
            result['artifact'] = compare_ppm(run / 'artifact.ppm', expected)
        elif scenario == 'clipboard-text':
            result['artifact'] = {'ok': len(artifacts) == 1 and artifacts[0].read_bytes() == b'putgd synthetic clipboard text'}
        elif scenario == 'clipboard-files':
            result['artifact'] = {'ok': len(artifacts) == 2 and [path.read_bytes() for path in artifacts] == [b'putgd synthetic file 1\n', b'putgd synthetic file 2\n']}
        if not result['artifact']['ok']:
            raise LabError('Captured bytes/pixels do not match independent fixture')
        pointer(run, x, y, width, height, True)
        pointer(run, x, y, width, height, False)
        guest_session(run, 'rm -f ~/putgd-lab/fixture/paste.json ~/putgd-lab/fixture/paste.ppm ~/putgd-lab/fixture/paste.png\n')
        key(run, ['ctrl', 'v'])
        time.sleep(.5)
        get(run, 'putgd-lab/fixture/paste.json', run / 'paste.json')
        paste = read_json(run / 'paste.json')
        result['pasteMetadata'] = paste
        if args.settings:
            import urllib.request
            public_files = []
            for index, item in enumerate(operation['items']):
                url = item['receipt']['directUrl']
                if not url.startswith(('http://', 'https://')):
                    raise LabError('No public HTTP direct URL')
                public = run / f'public-{index}'
                with urllib.request.urlopen(url, timeout=15) as response, public.open('wb') as output:
                    received = 0
                    while chunk := response.read(1024 * 1024):
                        received += len(chunk)
                        if received > item['artifact']['bytes']:
                            raise LabError('Public object larger than stored artifact')
                        output.write(chunk)
                public_files.append(public)
            result['paste'] = verify_publication(operation, artifacts, public_files, paste)
            result['receipts'] = [item['receipt'] for item in operation['items']]
            if args.history:
                guest_session(run, 'pkill -x putgd\nnohup putgd-launcher > ~/putgd-lab/app-restart.log 2>&1 < /dev/null &\n')
                restart_deadline = time.monotonic() + args.timeout
                while True:
                    try:
                        if cli(run, 'diagnostics').get('journalReady'):
                            break
                    except (LabError, OSError, ValueError, subprocess.TimeoutExpired):
                        pass
                    if time.monotonic() >= restart_deadline:
                        raise LabError('Resident restart timeout')
                    time.sleep(.2)
                result['runningAppAfterRestart'] = verify_running_app(run)
                history_ids = [item['artifact']['id'] for item in operation['items']]
                details = [cli(run, 'history.detail', {'id': history_id}) for history_id in history_ids]
                result['history'] = {'ok': all(detail['receipt']['directUrl'] == item['receipt']['directUrl'] for detail, item in zip(details, operation['items'])), 'ids': history_ids}
                cli(run, 'history.copyLink', {'id': history_ids[0]})
                guest_session(run, 'rm -f ~/putgd-lab/fixture/paste.json ~/putgd-lab/fixture/paste.ppm ~/putgd-lab/fixture/paste.png\n')
                key(run, ['alt', 'tab'])
                pointer(run, x, y, width, height, True)
                pointer(run, x, y, width, height, False)
                key(run, ['ctrl', 'v'])
                time.sleep(.3)
                get(run, 'putgd-lab/fixture/paste.json', run / 'history-paste.json')
                result['history']['ok'] &= read_json(run / 'history-paste.json').get('text') == details[0]['receipt']['directUrl']
                # Preparation is read-only. Exact IDs/effects are retained for review;
                # deletion is a separate explicit production CLI acknowledgement.
                result['deletionPreparation'] = cli(run, 'delete.prepare', {'ids': history_ids})
        elif expected:
            if args.clipboard_client == 'gtk':
                guest_session(run, 'putgd-desktop-fixture --convert "$HOME/putgd-lab/fixture/paste.png" "$HOME/putgd-lab/fixture/paste.ppm"\n')
            get(run, 'putgd-lab/fixture/paste.ppm', run / 'paste.ppm')
            result['paste'] = compare_ppm(run / 'paste.ppm', expected)
        elif scenario == 'clipboard-text':
            result['paste'] = compare_paste(paste, {'text': 'putgd synthetic clipboard text'})
        else:
            from urllib.parse import unquote, urlparse
            pasted_paths = [unquote(urlparse(url).path) for url in paste.get('urls', [])]
            result['paste'] = {'ok': pasted_paths == [item['local']['path'] for item in operation['items']]}
        result['ok'] = result['artifact']['ok'] and result['paste']['ok'] and result.get('history', {'ok': True})['ok']
        if not result['ok']:
            raise LabError('Actual saved pixels or independent paste mismatch')
    except (LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        result['ok'] = False
        result['error'] = str(error)
    finally:
        if result['ok']:
            try:
                expected_process = result.get('runningAppAfterRestart', result['runningApp'])
                if verify_running_app(run) != expected_process:
                    raise LabError('Resident identity changed during the graphical scenario')
            except (LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                result['ok'] = False
                result['error'] = str(error)
        try:
            result['witness'] = screenshot(run, 'final')
            collect(run)
        except (LabError, OSError, ValueError, subprocess.TimeoutExpired):
            result['collectionIncomplete'] = True
        result['elapsedSeconds'] = time.monotonic() - started
        result['evidenceDirectory'] = 'tests/' + test_id
        write_json(run / 'results.json', result)
        import xml.etree.ElementTree as ET
        suite = ET.Element('testsuite', name='desktop-lab', tests='1', failures='0' if result['ok'] else '1')
        case = ET.SubElement(suite, 'testcase', name=result['scenario'], classname=result['profile'], time=str(result['elapsedSeconds']))
        if not result['ok']:
            ET.SubElement(case, 'failure', message=result.get('error', 'mismatch'))
        ET.ElementTree(suite).write(run / 'results.xml', encoding='utf-8', xml_declaration=True)
        evidence = run / 'tests' / test_id
        evidence.mkdir(parents=True, exist_ok=False)
        files = {}
        for name in ('parameters.json', 'results.json', 'results.xml', 'ready.json', 'paste.json',
                     'paste.ppm', 'artifact.ppm', 'before-head0.ppm', 'final-head0.ppm',
                     'versions.json', 'package.json', 'diagnostics.json'):
            source = run / name
            if source.exists():
                shutil.copyfile(source, evidence / name)
                files[name] = hash_file(evidence / name)
        for source in artifacts + public_files:
            if source.exists():
                shutil.copyfile(source, evidence / source.name)
                files[source.name] = hash_file(evidence / source.name)
        write_json(evidence / 'manifest.json', {'schemaVersion': 1, 'testId': test_id,
            'profile': result['profile'], 'scenario': args.scenario, 'files': files})
        test_event(run, {'event': 'finished', 'testId': test_id, 'profile': result['profile'],
            'scenario': args.scenario, 'ok': result['ok'], 'elapsedSeconds': result['elapsedSeconds'],
            'operationId': result.get('operationId'), 'error': result.get('error'),
            'evidenceDirectory': result['evidenceDirectory']})
        if not args.keep:
            reset(run)
    return result


def verify_installed_binaries(run, deb):
    # Development rebuilds retain their version. Confirm actual guest bytes rather
    # than recording only the requested archive's hash.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='putgd-package-') as directory:
        command(['dpkg-deb', '--extract', str(Path(deb).resolve()), directory])
        expected = {}
        for path in (Path(directory) / 'usr/bin').iterdir():
            if path.is_file() and path.name.startswith('putgd'):
                expected['/usr/bin/' + path.name] = hash_file(path)
        if not expected:
            raise LabError('Development package has no application binaries')
        output = ssh(run, 'sha256sum ' + ' '.join(shlex.quote(path) for path in expected) + '\n')
        actual = {}
        for line in output.splitlines():
            digest, path = line.split(maxsplit=1)
            actual[path] = digest
        if actual != expected:
            raise LabError('Installed application binaries differ from requested package')
    return expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('profiles')
    launch_parser = sub.add_parser('launch')
    for name in ('profile', 'golden', 'run', 'bootstrap-key', 'known-hosts'):
        launch_parser.add_argument('--' + name, required=True)
    launch_parser.add_argument('--ssh-port', type=int, default=22220)
    launch_parser.add_argument('--vnc-display', type=int, default=20)
    launch_parser.add_argument('--acceleration', choices=['kvm', 'tcg'], default='kvm')
    launch_parser.add_argument('--timeout', type=int, default=180)
    for name in ('reset', 'collect', 'screenshot', 'key', 'input', 'install', 'run', 'cli'):
        child = sub.add_parser(name)
        child.add_argument('--run', required=True)
        if name == 'cli':
            child.add_argument('--method', required=True)
            child.add_argument('--params', default='{}', help='Production JSON parameters (credential references only)')
        if name == 'screenshot':
            child.add_argument('--head', type=int, default=0)
        if name == 'key':
            child.add_argument('keys', nargs='+')
        if name == 'input':
            child.add_argument('events', help='JSON QMP input-send-event events array')
        if name == 'install':
            child.add_argument('--deb', required=True)
        if name == 'run':
            child.add_argument('--timeout', type=int, default=45)
            child.add_argument('--keep', action='store_true')
            child.add_argument('--scenario', choices=['region', 'window', 'desktop', 'clipboard-text', 'clipboard-image', 'clipboard-files', 'region-cancel-escape', 'region-cancel-right-click', 'region-cancel-zero-area'], default='region')
            child.add_argument('--settings', help='Non-secret synthetic destination patch; keychain references only')
            child.add_argument('--history', action='store_true', help='Restart and verify newly uploaded history/link paste; prepare exact deletion effects')
            child.add_argument('--clipboard-client', choices=['qt', 'gtk'], default='qt', help='Independent native clipboard owner/paste client; GTK requires distro Python GI/GTK4/Cairo')
    args = parser.parse_args()
    try:
        if args.action == 'run' and args.history and not args.settings:
            raise LabError('--history requires --settings upload destination')
        if args.action == 'run' and args.clipboard_client == 'gtk' and not args.scenario.startswith('clipboard-'):
            raise LabError('GTK client is currently scoped to clipboard interoperability scenarios')
        if args.action == 'profiles':
            value = [read_json(path) for path in sorted((HERE / 'profiles').glob('*.yaml'))]
        elif args.action == 'launch':
            value = launch(args)
        elif args.action == 'reset':
            value = reset(args.run)
        elif args.action == 'collect':
            value = collect(args.run)
        elif args.action == 'screenshot':
            value = screenshot(args.run, head=args.head)
        elif args.action == 'key':
            key(args.run, args.keys)
            value = {'status': 'input-sent'}
        elif args.action == 'input':
            input_events(args.run, json.loads(args.events))
            value = {'status': 'input-sent'}
        elif args.action == 'cli':
            value = cli(args.run, args.method, json.loads(args.params))
        elif args.action == 'install':
            # A failed new install must not certify the previous package.
            (Path(args.run) / 'package.json').unlink(missing_ok=True)
            put(args.run, str(Path(args.deb).resolve()), '/tmp/putgd-lab.deb')
            ssh(args.run, 'sudo -n apt-get update\nsudo -n apt-get install --reinstall -y /tmp/putgd-lab.deb\n', timeout=180)
            installed = verify_installed_binaries(args.run, args.deb)
            write_json(Path(args.run) / 'package.json', {'sha256': hash_file(args.deb), 'filename': Path(args.deb).name, 'installedBinarySha256': installed})
            value = {'status': 'installed'}
        else:
            value = fixture_run(args)
        print(json.dumps(value))
        return 1 if isinstance(value, dict) and value.get('ok') is False else 0
    except (LabError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    sys.exit(main())
