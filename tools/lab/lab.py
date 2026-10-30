#!/usr/bin/env python3
"""Private QEMU/QMP graphical acceptance lab. Python stdlib only."""
import argparse
import datetime
from functools import wraps
import shutil
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import struct
import subprocess
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent
SCENARIOS = ('region', 'window', 'desktop', 'clipboard-text', 'clipboard-image', 'clipboard-files',
             'region-cancel-escape', 'region-cancel-right-click', 'region-cancel-zero-area')
sys.path.insert(0, str(HERE))
import overlay_cleanup
import rfb

class LabError(RuntimeError):
    pass


def lifecycle_operation(function):
    @wraps(function)
    def locked(*args, **kwargs):
        try:
            with overlay_cleanup.lifecycle_lock():
                return function(*args, **kwargs)
        except overlay_cleanup.CleanupError as error:
            raise LabError(str(error)) from error
    return locked


def cleanup_overlay(run, *, evidence_manifest=None, dry_run=False, images_root=None):
    try:
        return overlay_cleanup.cleanup(run, evidence_manifest=evidence_manifest, dry_run=dry_run,
                                       images_root=images_root)
    except overlay_cleanup.CleanupError as error:
        raise LabError(str(error)) from error


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


# Host OVMF locations, searched in order. Both pflash files of a pair must come
# from the same package build, so a pair is accepted only when both exist.
UEFI_FIRMWARE = (
    {'code': '/usr/share/edk2/ovmf/OVMF_CODE_4M.qcow2', 'vars': '/usr/share/edk2/ovmf/OVMF_VARS_4M.qcow2',
     'format': 'qcow2'},  # Fedora edk2-ovmf
    {'code': '/usr/share/OVMF/OVMF_CODE_4M.fd', 'vars': '/usr/share/OVMF/OVMF_VARS_4M.fd',
     'format': 'raw'},  # Debian/Ubuntu ovmf
)


def uefi_firmware(profile):
    """Resolve the host OVMF pair for a `firmware: uefi` profile; None for BIOS profiles."""
    kind = profile.get('firmware', 'bios')
    if kind == 'bios':
        return None
    if kind != 'uefi':
        raise LabError('Profile firmware must be "bios" or "uefi"')
    for candidate in UEFI_FIRMWARE:
        if Path(candidate['code']).is_file() and Path(candidate['vars']).is_file():
            return {'type': 'uefi', 'code': candidate['code'], 'vars_template': candidate['vars'],
                    'format': candidate['format']}
    raise LabError('UEFI firmware unavailable; install edk2-ovmf (Fedora) or ovmf (Debian/Ubuntu)')


MAX_HEADS = 4
# QEMU's virtio-gpu default scanout size; extra heads start at this size unless a layout says otherwise.
DEFAULT_HEAD_SIZE = (1280, 800)
MULTIHEAD_VIDEO = ('virtio-vga', 'virtio-gpu-pci')
HEAD_GEOMETRY = re.compile(r'([0-9]{3,4})x([0-9]{3,4})([+-][0-9]{1,5})([+-][0-9]{1,5})')


def parse_layout(value):
    """Parse `WxH+X+Y,...` (one entry per head, in head order; X/Y may be negative)."""
    parts = value.split(',') if isinstance(value, str) else list(value)
    layout = []
    for part in parts:
        match = HEAD_GEOMETRY.fullmatch(str(part).strip())
        if not match:
            raise LabError('Head layout entries must look like 1920x1080+0+0 or 1280x1024-1280+0')
        width, height, x, y = (int(group) for group in match.groups())
        if not (640 <= width <= 5120 and 480 <= height <= 2880):
            raise LabError('Head sizes must be between 640x480 and 5120x2880')
        layout.append({'width': width, 'height': height, 'x': x, 'y': y})
    if not 1 <= len(layout) <= MAX_HEADS:
        raise LabError(f'A head layout has 1 to {MAX_HEADS} entries')
    for index, first in enumerate(layout):
        for second in layout[index + 1:]:
            if (first['x'] < second['x'] + second['width'] and second['x'] < first['x'] + first['width'] and
                    first['y'] < second['y'] + second['height'] and second['y'] < first['y'] + first['height']):
                raise LabError('Head layout rectangles overlap')
    return layout


def format_layout(layout):
    return ','.join(f"{head['width']}x{head['height']}{head['x']:+d}{head['y']:+d}" for head in layout)


def head_plan(profile, heads=None, layout=None):
    """Resolve the head count and optional layout from launch options, then the profile."""
    if layout is None and profile.get('head_layout') is not None:
        layout = profile['head_layout']
    layout = parse_layout(layout) if layout is not None else None
    count = heads if heads is not None else len(layout) if layout else profile.get('display_heads', 1)
    if not isinstance(count, int) or not 1 <= count <= MAX_HEADS:
        raise LabError(f'Heads must be 1 to {MAX_HEADS}')
    if layout is not None and len(layout) != count:
        raise LabError('The head layout needs one entry per head')
    if count > 1 and profile['video'] not in MULTIHEAD_VIDEO:
        raise LabError('Multiple heads need a virtio-gpu video device (virtio-vga or virtio-gpu-pci)')
    return count, layout


def qemu_command(profile, run, overlay, ssh_port, vnc_display, acceleration, firmware=None, heads=1):
    args = ['qemu-system-x86_64', '-machine', 'q35,accel=' + acceleration,
            '-cpu', 'host' if acceleration == 'kvm' else 'max']
    if firmware is not None:
        # Read-only shared code image plus this run's private, writable NVRAM copy.
        args += ['-drive', f"if=pflash,format={firmware['format']},readonly=on,file={firmware['code']}",
                 '-drive', f"if=pflash,format={firmware['format']},file={firmware['vars']}"]
    video = profile['video'] + ',id=video0' + (f',max_outputs={heads}' if heads > 1 else '')
    args += ['-smp', str(profile['cpus']), '-m', str(profile['memory_mib']),
            '-device', video, '-device', 'usb-ehci', '-device', 'usb-tablet']
    # Each extra head gets its own tablet bound to that head's console, so a VNC
    # client on head N sends coordinates relative to head N. The unbound tablet
    # keeps receiving QMP input that names no head.
    for head in range(heads if heads > 1 else 0):
        args += ['-device', f'usb-tablet,id=tablet{head},display=video0,head={head},serial=putgd-head{head}']
    args += ['-drive', f'file={overlay},format=qcow2,if=virtio',
            '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-:22',
            '-device', 'virtio-net-pci,netdev=net0',
            '-qmp', f'unix:{run}/qmp.sock,server=on,wait=off']
    if heads == 1:
        args += ['-vnc', f'127.0.0.1:{vnc_display}']
    else:
        # QEMU's VNC server shows one head per listener, on consecutive displays.
        for head in range(heads):
            args += ['-vnc', f'127.0.0.1:{vnc_display + head},id=head{head},display=video0,head={head}']
    args += ['-display', 'none', '-daemonize', '-pidfile', str(run / 'qemu.pid')]
    return args


def head_ports(vnc_display, heads):
    return [{'head': head, 'port': 5900 + vnc_display + head, 'display': f'127.0.0.1:{5900 + vnc_display + head}'}
            for head in range(heads)]


def request_head_size(port, width, height, timeout=10):
    """Enable one virtio-gpu head at a preferred size with an RFB SetDesktopSize request.

    virtio-gpu reports only head 0 as connected until a display client sends
    size information for a head. QEMU's VNC server forwards a client's
    SetDesktopSize to the device, which hot-plugs that head into the guest
    with the requested size as its preferred mode.
    """
    client = rfb.RFB(port, timeout=timeout)
    try:
        # Raw plus ExtendedDesktopSize, so QEMU reports the request's outcome.
        client.sock.sendall(struct.pack('>BxHii', 2, 2, 0, -308))
        client.sock.sendall(struct.pack('>BxHHBxIHHHHI', 251, width, height, 1, 0, 0, 0, width, height, 0))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            kind = client._read(1)[0]
            if kind == 0:
                client._read(1)
                for _ in range(struct.unpack('>H', client._read(2))[0]):
                    x, y, w, h, encoding = struct.unpack('>HHHHi', client._read(12))
                    if encoding == -308:
                        screens = client._read(4)[0]
                        client._read(16 * screens)
                        # x is the reason (1: this client's request), y the status;
                        # QEMU answers 4 when it forwarded the request to the device.
                        if x == 1:
                            if y not in (0, 4):
                                raise LabError(f'QEMU refused head size on port {port} (status {y})')
                            return {'port': port, 'width': width, 'height': height, 'status': y}
                    elif encoding == 0:
                        client._read(w * h * 4)
                    elif encoding != -223:
                        raise LabError('Unexpected RFB encoding while sizing a head')
            elif kind == 1:
                client._read(3)
                client._read(6 * struct.unpack('>H', client._read(2))[0])
            elif kind == 3:
                client._read(3)
                client._read(struct.unpack('>I', client._read(4))[0])
            elif kind != 2:
                raise LabError('Unexpected RFB message while sizing a head')
        raise LabError(f'No SetDesktopSize answer on port {port}')
    finally:
        client.close()


def enable_heads(vnc_display, heads, layout=None):
    sizes = [(head['width'], head['height']) for head in layout] if layout else [DEFAULT_HEAD_SIZE] * heads
    try:
        return [request_head_size(5900 + vnc_display + head, *sizes[head]) for head in range(heads)]
    except (RuntimeError, OSError, struct.error) as error:
        if isinstance(error, LabError):
            raise
        raise LabError(f'Could not size the guest heads over VNC: {error}') from error


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
    for area in expected['areas']:
        x, y, area_width, area_height = (area[field] for field in ('x', 'y', 'width', 'height'))
        row_pixels = bytes(area['rgb']) * area_width
        if not (0 <= x and 0 <= y and x + area_width <= width and y + area_height <= height) or any(
                pixels[((y + row) * width + x) * 3:((y + row) * width + x + area_width) * 3]
                != row_pixels for row in range(area_height)):
            errors.append(f'area({x},{y},{area_width},{area_height})')
    return {'ok': not errors, 'errors': errors, 'width': width, 'height': height,
            'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest()}


def fixture_oracle(rectangle, paint):
    """Deterministic fixture pixels at geometry established before capture."""
    colors = ([220, 30, 40], [20, 180, 70], [35, 70, 220], [235, 190, 20])
    return {'width': rectangle['width'], 'height': rectangle['height'], 'areas': [
        {'x': paint['x'] - rectangle['x'] + (index % 2) * 320,
         'y': paint['y'] - rectangle['y'] + (index // 2) * 160,
         'width': 320, 'height': 160, 'rgb': color}
        for index, color in enumerate(colors)]}


def kwin_wayland_session(ready):
    # KWin draws server-side window frames that native Qt Wayland frameMargins
    # omit. Decide from the observed session, never the profile name: VM and
    # physical Plasma hosts use differently named profiles.
    return ready['session'] == 'wayland' and 'KDE' in ready.get('desktop', '').split(':')


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


FIXTURE_RGB = ((220, 30, 40), (20, 180, 70), (35, 70, 220), (235, 190, 20))


def locate_scaled_fixture_paint(path, scale, slack=3):
    """The 640x320 four-colour fixture drawn at scale, such as an editor's Fit zoom.

    Smoothing and an image edge may change up to `slack` pixels at each
    boundary, while quadrant interiors keep their exact colours. The origin is
    taken from the boundaries between quadrants, so an edge drawn on the image's
    outer pixels cannot move it. Returns float coordinates, or None unless the
    pattern appears exactly once.
    """
    width, height, pixels = ppm(path)
    red, green, blue, yellow = (bytes(colour) for colour in FIXTURE_RGB)
    across, down = 320 * scale, 160 * scale
    shortest, longest = max(4, int(across) - 2 * slack), int(across) + 1
    stride = width * 3

    def runs(y, colour):
        row, found = pixels[y * stride:(y + 1) * stride], []
        start = row.find(colour * shortest)
        while start >= 0:
            if start % 3:
                start = row.find(colour * shortest, start + 1)
                continue
            end = start
            while row[end:end + 3] == colour:
                end += 3
            if (end - start) // 3 <= longest:
                found.append((start // 3, end // 3))
            start = row.find(colour * shortest, end)
        return found

    def pair(y, left, right):
        """(left start, boundary) of each left|right quadrant pair on row y."""
        rights = runs(y, right)
        return [(start, (end + other) / 2) for start, end in runs(y, left)
                for other, _ in rights if end <= other <= end + 2 * slack]

    tops = {}
    for y in range(height):
        for start, boundary in pair(y, red, green):
            tops.setdefault(start, []).append((y, boundary))
    matches = []
    for start, rows in tops.items():
        ys = [y for y, _ in rows]
        first, last = ys[0], ys[-1]
        if ys != list(range(first, last + 1)) or not down - 2 * slack <= len(ys) <= down + 1:
            continue
        below = next((y for y in range(last + 1, min(height, last + 2 + 2 * slack))
                      if any(abs(x - start) <= 1 for x, _ in pair(y, blue, yellow))), None)
        if below is None:
            continue
        # The bottom half must be complete too, not one matching row.
        bottom = below
        while bottom < height and any(abs(x - start) <= 1 for x, _ in pair(bottom, blue, yellow)):
            bottom += 1
        if not down - 2 * slack <= bottom - below <= down + 1:
            continue
        across_at = sum(boundary for _, boundary in rows) / len(rows)
        down_at = (last + 1 + below) / 2
        matches.append({'x': across_at - across, 'y': down_at - down, 'width': 640 * scale, 'height': 320 * scale})
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


def get(run, remote, local, timeout=60):
    arguments, endpoint = scp_connection(run)
    command(arguments + [endpoint + str(remote), local], timeout=timeout)


def native_display_prefix():
    # GNOME creates its Wayland display after compositor startup, so it may be
    # absent from /proc/gnome-shell/environ. The session manager has the updated
    # value. Without it the resident cannot recognize the native GNOME session
    # and falls back to its graphical route (about twice the idle memory).
    # Preserve that real socket before any caller changes the runtime root.
    return """if [ "${XDG_SESSION_TYPE:-}" = wayland ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
 native_wayland=$(systemctl --user show-environment | sed -n 's/^WAYLAND_DISPLAY=//p')
 [ -n "$native_wayland" ] || exit 70
 export WAYLAND_DISPLAY="$native_wayland"
fi
case "${WAYLAND_DISPLAY:-}" in ""|/*) ;; *) export WAYLAND_DISPLAY="$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY";; esac
"""


def install_format(profile):
    value = profile.get('install', 'deb')
    if value not in ('deb', 'appimage'):
        raise LabError('Profile install must be "deb" or "appimage"')
    return value


def appimage_path_prefix(profile):
    # `install --appimage` writes putgd-launcher/putgd-cli shims there, so the
    # same session commands drive the AppImage and a distro package. Package
    # profiles keep their unchanged PATH.
    if install_format(profile) != 'appimage':
        return ''
    return 'export PATH="$HOME/putgd-lab/bin:$PATH"\n'


def appimage_entries_prefix(profile):
    # AppRun writes its host desktop entries (the KWin capture permission, the
    # recorder's screencast entry and the putgd-ui/putgd-record entries that
    # carry the windows' icons) under XDG_DATA_HOME/applications. The compositor
    # reads the session's own data home, so with the private one below KWin
    # showed its generic Wayland icon (#523). Point only applications/ at the
    # session's; put.gd's own data stays private. A directory with files in it
    # is left as it is.
    if install_format(profile) != 'appimage':
        return ''
    return '''session_data=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n 's/^XDG_DATA_HOME=//p')
session_applications=${session_data:-$HOME/.local/share}/applications
mkdir -p "$session_applications" "$XDG_DATA_HOME"
[ -L "$XDG_DATA_HOME/applications" ] || rmdir "$XDG_DATA_HOME/applications" 2>/dev/null || :
[ -e "$XDG_DATA_HOME/applications" ] || ln -s "$session_applications" "$XDG_DATA_HOME/applications"
'''


def guest_session(run, body):
    # Discover the real desktop session's environment, not SSH's empty DISPLAY.
    info = state(run)
    user = info['guest_user']
    profile = load_profile(info['profile'])
    process = profile['session_process']
    if process not in ('cinnamon', 'gnome-shell', 'plasmashell'):
        raise LabError('Unsupported native desktop session process')
    # PanelTone needs the session's XDG_CONFIG_DIRS and XDG_DATA_DIRS too
    # (docs/appearance.md): Plasma lists ~/.config/kdedefaults in the first and
    # finds the Plasma style's colors file through the second. Without them a
    # Breeze Dark panel reads as Plasma's light default (#229, #243).
    script = f'''set -eu
pid=$(pgrep -u {shlex.quote(user)} -x {shlex.quote(process)} | head -n 1)
[ -n "$pid" ]
for name in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_CONFIG_DIRS XDG_DATA_DIRS; do
 value=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n "s/^$name=//p")
 [ -z "$value" ] || export "$name=$value"
done
export QT_QPA_PLATFORM={shlex.quote(profile["qt_platform"])}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
{appimage_path_prefix(profile)}{native_display_prefix()}export XDG_CONFIG_HOME=$HOME/putgd-lab/config
export XDG_DATA_HOME=$HOME/putgd-lab/data
{appimage_entries_prefix(profile)}{body}
'''
    return ssh(run, script)


def verify_owned_guest(run):
    """Prove SSH targets this user's running overlay, never a physical desktop."""
    run = overlay_cleanup._no_symlinks(Path(run).absolute())
    info = state(run)
    disk = run / 'disk.qcow2'
    if info.get('overlay') != str(disk) or Path(info.get('golden', '')) == disk or not disk.is_file():
        raise LabError('Guest preflight requires an owned disposable overlay')
    overlay_cleanup._no_symlinks(disk)
    port = info.get('ssh_port')
    if not isinstance(port, int) or not 1024 <= port <= 65535:
        raise LabError('Guest preflight requires a private forwarded SSH port')
    process = Path('/proc') / str(info.get('pid'))
    try:
        arguments = (process / 'cmdline').read_bytes().split(b'\0')
        forward = f'hostfwd=tcp:127.0.0.1:{port}-:22'.encode()
        valid = (process.stat().st_uid == os.getuid()
                 and Path(arguments[0].decode()).name.startswith('qemu-system-')
                 and any(str(disk).encode() in value.split(b',') or
                         b'file=' + str(disk).encode() in value.split(b',') for value in arguments)
                 and any(forward in value.split(b',') for value in arguments))
    except (OSError, IndexError, UnicodeError):
        valid = False
    if not valid:
        raise LabError('Guest preflight cannot authenticate the owned QEMU/SSH endpoint')
    with QMP(run / 'qmp.sock') as qmp:
        if qmp.execute('query-status').get('running') is not True:
            raise LabError('Guest preflight requires a running VM')
    return info


def visible_guest_output(run, token, head, port, apply, attempts):
    # A desktop shell can own its bus names before its first frame is painted.
    # Retain every initial blank attempt; later boundary checks never wait.
    deadline = time.monotonic() + (15 if apply else 0)
    count = 0
    while True:
        witness = screenshot(run, token + '-attempt' + str(count), head)
        width, height, pixels = ppm(witness['path'])
        # A uniform non-gray RGB frame still has distinct channel bytes.
        # Require a complete frame containing at least two different pixels.
        visible = (width > 0 and height > 0 and len(pixels) == width * height * 3
                   and pixels != pixels[:3] * (width * height))
        attempts.append(dict(witness, width=width, height=height, head=head, visible=visible))
        if visible:
            break
        if time.monotonic() >= deadline:
            raise LabError('Guest visible-output witness is blank')
        count += 1
        time.sleep(1)
    client = rfb.RFB(port)
    try:
        if (client.width, client.height) != (width, height):
            raise LabError('Guest VNC and QMP output extents disagree')
    finally:
        client.close()
    return dict(witness, width=width, height=height, head=head)


def guest_preflight(run, *, apply=False, stage='manual'):
    """Configure once, then fail closed on policy/session/output drift at handoffs."""
    # Refuse unsafe receipt destinations before writing anything. Once the
    # directory is safe, even ownership/readiness/profile failures are evidence.
    run = overlay_cleanup._no_symlinks(Path(run).absolute())
    directory = run.stat()
    if not run.is_dir() or directory.st_uid != os.getuid() or directory.st_mode & 0o022:
        raise LabError('Guest preflight receipt requires an owned, non-writable-by-others directory')
    token = 'guest-preflight-' + uuid.uuid4().hex
    report = {'schemaVersion': 1, 'stage': stage, 'apply': apply,
              'timestampUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'ok': False}
    try:
        info = verify_owned_guest(run)
        report['runId'] = info['id']
        profile = load_profile(info['profile'])
        source = HERE / 'guest_idle.py'
        report['helperSha256'] = hash_file(source)
        desktop = 'Cinnamon' if profile['desktop'] == 'X-Cinnamon' else profile['desktop']
        arguments = ['python3', '-', '--desktop', desktop, '--process', profile['session_process'],
                     '--user', info['guest_user'], '--session-type', profile['session']]
        if apply:
            arguments.append('--apply')
        result = command(ssh_args(run) + [shlex.join(arguments)], input=source.read_text(), timeout=90, check=False)
        report['policyInvocation'] = {'exitCode': result.returncode, 'stderr': getattr(result, 'stderr', '')[:1000]}
        report['policy'] = json.loads(result.stdout)
        if result.returncode or report['policy'].get('ok') is not True:
            raise LabError('Guest idle/session preflight refused: ' + report['policy'].get('error', 'no policy receipt'))
        heads = len(info.get('heads', [])) or 1
        outputs = guest_outputs(run, layout_backend(profile))
        report['outputs'] = verify_outputs(outputs, heads)
        if not report['outputs']['ok']:
            raise LabError('Guest outputs are not all connected and enabled')
        report['visuals'] = []
        report['visualAttempts'] = []
        for head in range(heads):
            # QMP supplies a complete frame; RFB damage updates are not a full-frame oracle.
            report['visuals'].append(visible_guest_output(
                run, token, head, 5900 + info['vnc_display'] + head, apply, report['visualAttempts']))
        # Recheck after output collection, using the real desktop homes again.
        checked = command(ssh_args(run) + [shlex.join([a for a in arguments if a != '--apply'])],
                          input=source.read_text(), timeout=90, check=False)
        report['afterVisualInvocation'] = {'exitCode': checked.returncode, 'stderr': checked.stderr[:1000]}
        report['afterVisuals'] = json.loads(checked.stdout)
        if checked.returncode or report['afterVisuals'].get('ok') is not True:
            raise LabError('Guest policy/session changed while collecting visible output evidence')
        report['ok'] = True
        return report
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired, LabError,
            overlay_cleanup.CleanupError) as error:
        report['error'] = str(error)
        raise LabError('Guest preflight failed; retained ' + token + '.json: ' + str(error)) from error
    finally:
        write_json(run / (token + '.json'), report)


def acceptance_preflight(run, *, apply=False, stage='manual'):
    # physical.py deliberately shares scenario/start logic, but its separately
    # leased user desktop must never receive disposable-guest policy changes.
    if state(run).get('acceleration') == 'physical':
        return {'scope': 'physical desktop policy unchanged; managed by desktop lease'}
    return guest_preflight(run, apply=apply, stage=stage)


@lifecycle_operation
def _launch_machine(args, profile, golden, run, golden_sha256, golden_identity, firmware=None, heads=1, layout=None):
    current = golden.stat()
    if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != golden_identity:
        raise LabError('Golden changed before launch')
    run.mkdir(parents=True, mode=0o700, exist_ok=False)
    os.chmod(run, 0o700)
    overlay = run / 'disk.qcow2'
    command(['qemu-img', 'create', '-f', 'qcow2', '-F', 'qcow2', '-b', golden, overlay])
    if firmware is not None:
        # The shared OVMF code image stays read-only; UEFI variables live in a
        # private copy that cleanup preserves with the rest of the run's state.
        vars_copy = run / ('vars.qcow2' if firmware['format'] == 'qcow2' else 'vars.fd')
        shutil.copyfile(firmware['vars_template'], vars_copy)
        os.chmod(vars_copy, 0o600)
        firmware = {**firmware, 'vars': str(vars_copy),
                    'code_sha256': hash_file(firmware['code']),
                    'vars_template_sha256': hash_file(firmware['vars_template'])}
    command(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', run / 'identity'])
    known_hosts = run / 'known_hosts'
    known_hosts.write_bytes(Path(args.known_hosts).read_bytes())
    info = {'schemaVersion': 1, 'id': str(uuid.uuid4()), 'profile': args.profile,
            'guest_user': profile['guest_user'], 'ssh_port': args.ssh_port,
            'vnc_display': args.vnc_display, 'acceleration': args.acceleration,
            'golden': str(golden), 'golden_sha256': golden_sha256,
            'bootstrap_key': str(Path(args.bootstrap_key).resolve()),
            'known_hosts': str(known_hosts), 'overlay': str(overlay)}
    if firmware is not None:
        info['firmware'] = firmware
    if heads > 1 or layout:
        info['heads'] = head_ports(args.vnc_display, heads)
    if layout:
        info['head_layout'] = format_layout(layout)
    write_json(run / 'state.json', info)
    result = command(qemu_command(profile, run, overlay, args.ssh_port,
                                  args.vnc_display, args.acceleration, firmware, heads), timeout=15, check=False)
    (run / 'qemu.log').write_text(result.stderr)
    if result.returncode:
        raise LabError('QEMU startup failed; private qemu.log preserved')
    info['pid'] = int((run / 'qemu.pid').read_text().strip())
    write_json(run / 'state.json', info)
    return info


def launch(args):
    # Match guarded retirement's direct-child, symlink-free layout before any
    # bootstrap, hashing, key generation, mkdir or overlay allocation.
    root = overlay_cleanup.ROOT / 'work/lab-runs'
    try:
        run = overlay_cleanup._no_symlinks(Path(args.run).absolute())
    except overlay_cleanup.CleanupError as error:
        raise LabError(str(error)) from error
    if run.parent != root:
        raise LabError(f'Run must be a direct child of canonical {root}; use --run {root}/c1. '
                       'Keep the run name short; if needed use a checkout at a shorter real path '
                       'and check resolved Unix socket paths against the 107-byte limit.')
    sockets = [run / name for name in ('qmp.sock', 'vnc.sock')]
    checked = command([sys.executable, HERE.parent / 'diagnostics/check_socket_paths.py',
                       '--socket', *sockets], check=False)
    if checked.returncode:
        raise LabError('Run Unix socket paths exceed the 107-byte limit: ' + checked.stdout.strip())
    profile = load_profile(args.profile)
    golden = Path(args.golden).resolve()
    if not golden.is_file():
        raise LabError('Install and shut down a golden guest first; see docs/vm-lab.md')
    if args.acceleration == 'kvm' and not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise LabError('KVM unavailable; request explicit --acceleration tcg for functional-only evidence')
    # A provisioning key pinned to another forwarded port cannot authenticate
    # this endpoint. Reject it before allocating an overlay or booting a guest.
    endpoint = '127.0.0.1' if args.ssh_port == 22 else f'[127.0.0.1]:{args.ssh_port}'
    pinned = command(['ssh-keygen', '-F', endpoint, '-f', args.known_hosts], check=False)
    if pinned.returncode or not pinned.stdout.strip():
        raise LabError(f'{endpoint} is missing from --known-hosts; see docs/vm-lab.md for authenticated port remapping')
    firmware = uefi_firmware(profile)
    heads, layout = head_plan(profile, getattr(args, 'heads', None), getattr(args, 'layout', None))
    before = golden.stat()
    golden_identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
    golden_sha256 = hash_file(golden)
    info = _launch_machine(args, profile, golden, run, golden_sha256, golden_identity, firmware, heads, layout)
    try:
        if heads > 1 or layout:
            # Before the guest's driver probes, so it boots with every head connected.
            info['head_requests'] = enable_heads(args.vnc_display, heads, layout)
            write_json(run / 'state.json', info)
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
        # Keep the run key usable when a later failed preflight stops QEMU.
        # A guest page-cache-only append can otherwise return as a NUL-filled
        # extent on restart. Separate it from any unterminated existing line.
        ssh(run, 'umask 077\nmkdir -p ~/.ssh\nprintf "\\n%s\\n" ' + shlex.quote(public_key)
            + ' >> ~/.ssh/authorized_keys\nsync ~/.ssh/authorized_keys\n', bootstrap=True)
        with QMP(run / 'qmp.sock') as qmp:
            features = qmp.execute('query-commands')
            acceleration_state = qmp.execute('query-kvm')
            write_json(run / 'qmp-features.json', {'version': qmp.version, 'commands': features, 'kvm': acceleration_state})
        result = {'run': str(run), 'id': info['id'], 'display': f'127.0.0.1:{5900 + args.vnc_display}',
                  'status': 'booted-not-accepted'}
        if 'heads' in info:
            result['heads'] = info['heads']
        result['guestPreflight'] = guest_preflight(run, apply=True, stage='launch')
        return result
    except BaseException:
        stop_failed_launch(run)
        raise


def stop_failed_launch(run):
    # Flush first-boot writes before QMP quit. A broken connection or inability
    # to save the receipt must still stop the owned guest.
    try:
        shutdown = {'guestSync': 'completed'}
        try:
            ssh(run, 'sync\n', timeout=10, bootstrap=True)
        except (LabError, OSError, subprocess.TimeoutExpired) as error:
            shutdown = {'guestSync': 'failed', 'error': str(error)}
        write_json(Path(run) / 'failure-shutdown.json', shutdown)
    finally:
        reset(run)


def hash_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


@lifecycle_operation
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


def input_events(run, events, head=None):
    arguments = {'events': events}
    if head is not None:
        arguments.update(device='video0', head=head)
    with QMP(Path(run) / 'qmp.sock') as qmp:
        qmp.execute('input-send-event', arguments)


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


def pointer(run, x, y, width, height, down=None, button='left'):
    # The inclusive tablet range has 32768 positions. Using extent-1
    # crosses Qt's half-pixel rounding boundary (680 becomes 680.5078 at 1280).
    # Choose the first tick at or above each pixel edge; floor and qRound
    # then agree for the tested native viewport sizes.
    events = [{'type': 'abs', 'data': {'axis': 'x', 'value': min(32767, math.ceil(x * 32768 / width))}},
              {'type': 'abs', 'data': {'axis': 'y', 'value': min(32767, math.ceil(y * 32768 / height))}}]
    if down is not None:
        events.append({'type': 'btn', 'data': {'button': button, 'down': down}})
    input_events(run, events)


def screenshot(run, name='witness', head=0):
    path = (Path(run) / f'{name}-head{head}.ppm').resolve()
    if path.parent != Path(run).resolve():
        raise LabError('Witness filename must stay in run directory')
    with QMP(Path(run) / 'qmp.sock') as qmp:
        qmp.execute('screendump', {'filename': str(path), 'device': 'video0', 'head': head})
    return {'path': str(path), 'sha256': hash_file(path)}


XRANDR_OUTPUT = re.compile(r'(\S+) (connected|disconnected)( primary)?(?: ([0-9]+)x([0-9]+)([+-][0-9]+)([+-][0-9]+))?')
XRANDR_MONITOR = re.compile(r'\s*[0-9]+: [+]?[*]?(\S+) [0-9]+/[0-9]+x[0-9]+/[0-9]+[+-][0-9]+[+-][0-9]+\s+(.*)')


def parse_xrandr(query, monitors):
    """Outputs in connector order plus the RandR monitors, from `xrandr --query` and `--listmonitors`."""
    outputs = []
    for line in query.splitlines():
        if line[:1].isspace():
            if outputs and line.split():
                outputs[-1]['modes'].append(line.split()[0])
            continue
        match = XRANDR_OUTPUT.match(line)
        if not match:
            continue
        name, state, primary, width, height, x, y = match.groups()
        output = {'name': name, 'connected': state == 'connected', 'enabled': width is not None,
                  'primary': bool(primary), 'modes': []}
        if width is not None:
            output.update(width=int(width), height=int(height), x=int(x), y=int(y))
        outputs.append(output)
    listed = []
    for line in monitors.splitlines():
        match = XRANDR_MONITOR.match(line)
        if match:
            listed.append({'name': match[1], 'outputs': match[2].split()})
    return {'outputs': outputs, 'monitors': listed}


def parse_kscreen(document):
    outputs = []
    for value in sorted(document.get('outputs', []), key=lambda output: output.get('name', '')):
        output = {'name': value['name'], 'connected': bool(value.get('connected')),
                  'enabled': bool(value.get('enabled')), 'primary': value.get('priority') == 1,
                  # kscreen lists the preferred mode first; keep the first mode of each size.
                  'modes': {}}
        for mode in value.get('modes', []):
            output['modes'].setdefault(mode['name'].split('@')[0], mode['id'])
        if output['enabled']:
            output.update(width=value['size']['width'], height=value['size']['height'],
                          x=value['pos']['x'], y=value['pos']['y'])
        outputs.append(output)
    return {'outputs': outputs}


def verify_outputs(observed, heads, layout=None):
    """Require `heads` real, enabled outputs (not RandR monitors split from one output)."""
    outputs = observed['outputs'][:heads]
    errors = []
    if len(outputs) < heads or not all(output['connected'] and output['enabled'] for output in outputs):
        errors.append(f'expected {heads} connected, enabled outputs')
    enabled = [output for output in observed['outputs'] if output['enabled']]
    if len(enabled) != heads:
        errors.append(f'{len(enabled)} outputs enabled, expected {heads}')
    if 'monitors' in observed:
        # A real output is its own RandR monitor; `xrandr --setmonitor` splits are not.
        names = sorted(output['name'] for output in enabled)
        if sorted(monitor['name'] for monitor in observed['monitors']) != names or any(
                monitor['outputs'] != [monitor['name']] for monitor in observed['monitors']):
            errors.append('RandR monitors do not match the real outputs one to one')
    if layout and not errors:
        # X11, Mutter and KWin place the whole desktop at 0,0, so compare the arrangement
        # relative to each side's top-left corner. Head 0 must be the primary output.
        spec_x, spec_y = min(head['x'] for head in layout), min(head['y'] for head in layout)
        real_x, real_y = min(output['x'] for output in outputs), min(output['y'] for output in outputs)
        for index, (head, output) in enumerate(zip(layout, outputs)):
            wanted = (head['width'], head['height'], head['x'] - spec_x, head['y'] - spec_y)
            actual = (output['width'], output['height'], output['x'] - real_x, output['y'] - real_y)
            if wanted != actual:
                errors.append(f'head {index} is {actual}, expected {wanted}')
        if not outputs[0]['primary']:
            errors.append('head 0 is not the primary output')
    return {'ok': not errors, 'errors': errors,
            'outputs': [{key: output[key] for key in ('name', 'enabled', 'primary', 'width', 'height', 'x', 'y')
                         if key in output} for output in observed['outputs']]}


MUTTER_STATE = r'''
import json, sys
from gi.repository import Gio, GLib
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
def call(method, arguments=None):
    return bus.call_sync('org.gnome.Mutter.DisplayConfig', '/org/gnome/Mutter/DisplayConfig',
                         'org.gnome.Mutter.DisplayConfig', method, arguments, None, 0, 10000, None).unpack()
serial, monitors, logical, properties = call('GetCurrentState')
placed = {}
for x, y, scale, transform, primary, members, _ in logical:
    for connector, *_ in members:
        placed[connector] = (x, y, primary)
outputs = []
for (connector, *_), modes, _ in sorted(monitors, key=lambda monitor: monitor[0][0]):
    current = [mode for mode in modes if mode[6].get('is-current')]
    output = {'name': connector, 'connected': True, 'enabled': connector in placed and bool(current),
              'primary': placed.get(connector, (0, 0, False))[2],
              'modes': {f'{mode[1]}x{mode[2]}': mode[0] for mode in sorted(modes, key=lambda mode: mode[3])}}
    if output['enabled']:
        output.update(width=current[0][1], height=current[0][2], x=placed[connector][0], y=placed[connector][1])
    outputs.append(output)
wanted = json.loads(sys.argv[1]) if len(sys.argv) > 1 else None
if wanted:
    config = [(head['x'], head['y'], 1.0, 0, index == 0, [(head['name'], head['mode'], {})])
              for index, head in enumerate(wanted)]
    call('ApplyMonitorsConfig', GLib.Variant('(uua(iiduba(ssa{sv}))a{sv})', (serial, 1, config, {})))
print(json.dumps({'outputs': outputs}))
'''


def guest_outputs(run, backend):
    if backend == 'xrandr':
        text = guest_session(run, 'xrandr --query\nprintf "\\n@@\\n"\nxrandr --listmonitors\n')
        query, monitors = text.split('\n@@\n', 1)
        return parse_xrandr(query, monitors)
    if backend == 'kscreen':
        return parse_kscreen(json.loads(guest_session(run, 'kscreen-doctor -j\n')))
    return json.loads(guest_session(run, 'python3 -c ' + shlex.quote(MUTTER_STATE) + '\n'))


def layout_backend(profile):
    if profile['session'] == 'x11':
        return 'xrandr'
    return 'kscreen' if profile['desktop'] == 'KDE' else 'mutter'


def map_tablets_x11(run, outputs):
    """Bind each head's QEMU tablet to that head's output, so VNC input lands on the right screen."""
    pairs = ' '.join(f'{head}:{shlex.quote(output["name"])}' for head, output in enumerate(outputs)
                     if output.get('enabled', True))
    return guest_session(run, f'''command -v xinput >/dev/null || {{ echo unavailable; exit 0; }}
for id in $(xinput list --id-only); do
 node=$(xinput list-props "$id" 2>/dev/null | sed -n 's/.*Device Node ([0-9]*):[[:space:]]*"\\(.*\\)"/\\1/p')
 [ -n "$node" ] || continue
 # The USB device above the event node carries QEMU's per-head serial.
 path=$(readlink -f "/sys/class/input/${{node##*/}}/device")
 while [ "$path" != /sys ] && [ "$path" != / ] && [ ! -f "$path/serial" ]; do path=${{path%/*}}; done
 serial=$(cat "$path/serial" 2>/dev/null || true)
 for pair in {pairs}; do
  if [ "$serial" = "putgd-head${{pair%%:*}}" ]; then
   xinput map-to-output "$id" "${{pair#*:}}"
   echo "mapped $id ${{pair#*:}}"
  fi
 done
done
''').split()[2::3]


def layout(run, spec=None, settle=30):
    """Size, arrange and verify a multi-head guest's outputs in its native session."""
    run = Path(run)
    info = state(run)
    profile = load_profile(info['profile'])
    heads = info.get('heads') or head_ports(info['vnc_display'], 1)
    wanted = parse_layout(spec) if spec else parse_layout(info['head_layout']) if info.get('head_layout') else None
    if wanted and len(wanted) != len(heads):
        raise LabError(f'This run has {len(heads)} heads; the layout names {len(wanted)}')
    if wanted:
        enable_heads(info['vnc_display'], len(heads), wanted)
    backend = layout_backend(profile)
    deadline = time.monotonic() + settle
    attempts = 0
    while True:
        observed = guest_outputs(run, backend)
        result = verify_outputs(observed, len(heads), wanted)
        if result['ok'] or time.monotonic() >= deadline:
            break
        names = [output['name'] for output in observed['outputs'][:len(heads)]]
        if wanted and len(names) == len(heads) and all(output['connected'] for output in observed['outputs'][:len(heads)]):
            attempts += 1
            apply_layout(run, backend, observed, wanted)
        time.sleep(2)
    if result['ok'] and backend == 'xrandr':
        result['tablets'] = map_tablets_x11(run, observed['outputs'][:len(heads)])
    # Independent of the guest: each head's QMP screendump must have that head's size.
    result['screendumps'] = []
    for index, head in enumerate(heads):
        width, height, _ = ppm(screenshot(run, 'layout', index)['path'])
        result['screendumps'].append({'head': index, 'width': width, 'height': height})
        if wanted and (width, height) != (wanted[index]['width'], wanted[index]['height']):
            result['ok'] = False
            result['errors'].append(f'head {index} screendump is {width}x{height}')
    result.update(backend=backend, heads=heads, layout=format_layout(wanted) if wanted else None, attempts=attempts)
    write_json(run / 'layout.json', result)
    return result


def apply_layout(run, backend, observed, wanted):
    outputs = observed['outputs'][:len(wanted)]
    left, top = min(head['x'] for head in wanted), min(head['y'] for head in wanted)
    if backend == 'xrandr':
        arguments = ['xrandr']
        for index, (head, output) in enumerate(zip(wanted, outputs)):
            arguments += ['--output', output['name'], '--mode', f"{head['width']}x{head['height']}",
                          '--pos', f"{head['x'] - left}x{head['y'] - top}"] + (['--primary'] if index == 0 else [])
        guest_session(run, shlex.join(arguments) + '\n')
    elif backend == 'kscreen':
        arguments = ['kscreen-doctor']
        for index, (head, output) in enumerate(zip(wanted, outputs)):
            mode = output['modes'].get(f"{head['width']}x{head['height']}")
            if mode is None:
                raise LabError(f"{output['name']} offers no {head['width']}x{head['height']} mode")
            prefix = f"output.{output['name']}."
            arguments += [prefix + 'enable', prefix + f'mode.{mode}',
                          prefix + f"position.{head['x'] - left},{head['y'] - top}", prefix + f'priority.{index + 1}']
        guest_session(run, shlex.join(arguments) + '\n')
    else:
        config = []
        for head, output in zip(wanted, outputs):
            mode = output['modes'].get(f"{head['width']}x{head['height']}")
            if mode is None:
                raise LabError(f"{output['name']} offers no {head['width']}x{head['height']} mode")
            config.append({'name': output['name'], 'mode': mode, 'x': head['x'] - left, 'y': head['y'] - top})
        guest_session(run, 'python3 -c ' + shlex.quote(MUTTER_STATE) + ' ' + shlex.quote(json.dumps(config)) + '\n')


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
    witnesses = []
    before_ids = None
    for stale in ('parameters.json', 'ready.json', 'gtk-ready.json', 'paste.json',
                  'history-paste.json', 'paste.ppm', 'artifact.ppm', 'versions.json', 'diagnostics.json'):
        (run / stale).unlink(missing_ok=True)
    for pattern in ('focus-*-head*.ppm', 'selector-head*.ppm', 'drag-head*.ppm', 'before-head*.ppm', 'final-head*.ppm',
                    'artifact-[0-9]*', 'public-[0-9]*'):
        for stale in run.glob(pattern):
            stale.unlink()

    def get_witness(remote, target):
        target.unlink(missing_ok=True)
        witnesses.append(target)
        return get(run, remote, target)

    def capture_witness(name):
        witnesses.append(run / f'{name}-head0.ppm')
        return screenshot(run, name)
    test_event(run, {'event': 'started', 'testId': test_id, 'profile': state(run)['profile'],
        'scenario': args.scenario, 'clipboardClient': args.clipboard_client, 'timeoutSeconds': args.timeout})
    result = {'schemaVersion': 1, 'testId': test_id, 'profile': state(run)['profile'],
              'scenario': args.scenario + '-independent-paste', 'ok': False,
              'acceptanceScope': 'single-head named action; no full profile certification'}
    try:
        result['guestPreflightBefore'] = acceptance_preflight(run, stage='scenario-before')
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
                get_witness('putgd-lab/fixture/ready.json', run / 'ready.json')
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
                    get_witness('putgd-lab/fixture/gtk-ready.json', run / 'gtk-ready.json')
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
        region_contract = read_json(HERE.parent.parent / 'tests/desktop/common.yaml')
        selection = region_contract['input']['drag']
        selection_width, selection_height = selection['size']
        parameters = {'schemaVersion': 1, 'profile': state(run)['profile'],
            'scenario': args.scenario, 'clipboardClient': args.clipboard_client,
            'destination': 'upload' if args.settings else 'local',
            'runningApp': result['runningApp'],
            'runnerSha256': hash_file(Path(__file__)), 'hotkeys': hotkeys,
            'displayLayout': ready['display_layout'], 'qtVersion': ready['qt_version'],
            'acceleration': state(run)['acceleration'],
            'goldenSha256': state(run).get('golden_sha256'),
            'timingSeconds': {'keyHold': .05, 'focusPoll': .1, 'focusAttemptLimit': 2,
                'clipboardSourceSettle': .3, 'selectorSettle': .5, 'dragPhaseSettle': .1, 'operationPoll': .2},
            'tabletCoordinates': {'rangeSteps': 32768, 'maximumTick': 32767,
                'mapping': 'ceil(pixel * 32768 / extent), clamped to maximumTick'},
            'selection': {'fixtureOffset': dict(zip(('x', 'y'), selection['paintOffset'])),
                'width': selection_width, 'height': selection_height},
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
            focus_poll = 0
            while True:
                witness = capture_witness(f'focus-{attempt}-{focus_poll}')
                focus_poll += 1
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
                visible = compare_ppm(witness['path'], fixture_oracle(screen, geometry))
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
        if args.scenario == 'window' and kwin_wayland_session(ready):
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
        if args.scenario.startswith('region'):
            x = geometry['x'] + selection['paintOffset'][0]
            y = geometry['y'] + selection['paintOffset'][1]
        parameters['selection'].update(start={'x': x, 'y': y},
            end={'x': x + selection_width, 'y': y + selection_height}, inputViewport={'width': width, 'height': height})
        parameters['independentFixtureGeometry'] = geometry
        if 'independentWindowFrame' in result:
            parameters['independentWindowFrame'] = result['independentWindowFrame']
        write_json(run / 'parameters.json', parameters)
        result['parameters'] = 'parameters.json'
        capture_witness('before')
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
                capture_witness('selector')
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
                try:
                    pointer(run, x, y, width, height, True)
                    time.sleep(parameters['timingSeconds']['dragPhaseSettle'])
                    pointer(run, x + selection_width, y + selection_height, width, height)
                    time.sleep(parameters['timingSeconds']['dragPhaseSettle'])
                    capture_witness('drag')
                    pointer(run, x + selection_width, y + selection_height, width, height, False)
                except BaseException:
                    # Any input may reach QEMU even if its reply is lost.
                    # Request cancellation and release at the start point; the
                    # operation observation below retains delivered outcomes.
                    result['dragAbortRequested'] = True
                    for stage, cleanup in (
                            ('cancel', lambda: key(run, ['esc'])),
                            ('release', lambda: pointer(run, x, y, width, height, False))):
                        try:
                            cleanup()
                        except Exception as cleanup_error:
                            result.setdefault('dragCleanupErrors', []).append(
                                {'stage': stage, 'error': str(cleanup_error)})
                    raise
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
            get_witness('putgd-lab/fixture/paste.json', run / 'paste.json')
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
            expected = region_contract['oracle']
        elif scenario == 'clipboard-image':
            expected = {'width': 96, 'height': 64, 'areas': [
                {'x': 0, 'y': 0, 'width': 96, 'height': 64, 'rgb': [220, 30, 40]}]}
        elif scenario in ('window', 'desktop'):
            rectangle = ready['frame'] if scenario == 'window' else screen
            expected = fixture_oracle(rectangle, geometry)
        parameters['expectedArtifact'] = expected or {'syntheticBytes': scenario}
        write_json(run / 'parameters.json', parameters)
        for index, item in enumerate(operation['items']):
            if item['local']['status'] != 'saved':
                raise LabError('Preserved local result missing')
            artifact = run / f'artifact-{index}'
            get_witness(item['local']['path'], artifact)
            artifacts.append(artifact)
            if hash_file(artifact) != item['artifact']['sha256']:
                raise LabError('Stored artifact hash mismatch')
        if expected:
            item = operation['items'][0]
            guest_session(run, 'putgd-desktop-fixture --convert ' + shlex.quote(item['local']['path']) + ' "$HOME/putgd-lab/fixture/artifact.ppm"\n')
            get_witness('putgd-lab/fixture/artifact.ppm', run / 'artifact.ppm')
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
        get_witness('putgd-lab/fixture/paste.json', run / 'paste.json')
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
                public.unlink(missing_ok=True)
                witnesses.append(public)
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
                get_witness('putgd-lab/fixture/paste.json', run / 'history-paste.json')
                result['history']['ok'] &= read_json(run / 'history-paste.json').get('text') == details[0]['receipt']['directUrl']
                # Preparation is read-only. Exact IDs/effects are retained for review;
                # deletion is a separate explicit production CLI acknowledgement.
                result['deletionPreparation'] = cli(run, 'delete.prepare', {'ids': history_ids})
        elif expected:
            if args.clipboard_client == 'gtk':
                guest_session(run, 'putgd-desktop-fixture --convert "$HOME/putgd-lab/fixture/paste.png" "$HOME/putgd-lab/fixture/paste.ppm"\n')
            get_witness('putgd-lab/fixture/paste.ppm', run / 'paste.ppm')
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
        if args.scenario == 'region' and before_ids is not None:
            # Input delivery can precede a failed QMP reply. Retain identities
            # even when cleanup cannot confirm cancellation; never claim that
            # a failed driver means the application had no side effects.
            try:
                result['operationsAfterFailedDrag'] = [
                    {field: operation.get(field) for field in ('id', 'state', 'copyStatus')}
                    for operation in cli(run, 'operation.recent')['operations']
                    if operation['id'] not in before_ids]
            except (LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired):
                result['dragOperationObservationIncomplete'] = True
    finally:
        if result['ok']:
            try:
                result['guestPreflightAfter'] = acceptance_preflight(run, stage='scenario-after')
                expected_process = result.get('runningAppAfterRestart', result['runningApp'])
                if verify_running_app(run) != expected_process:
                    raise LabError('Resident identity changed during the graphical scenario')
            except (LabError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
                result['ok'] = False
                result['error'] = str(error)
        try:
            result['witness'] = capture_witness('final')
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
        metadata = [run / name for name in ('parameters.json', 'results.json', 'results.xml',
                    'versions.json', 'package.json', 'diagnostics.json')]
        for source in dict.fromkeys(metadata + witnesses):
            if source.exists():
                shutil.copyfile(source, evidence / source.name)
                files[source.name] = hash_file(evidence / source.name)
        write_json(evidence / 'manifest.json', {'schemaVersion': 1, 'testId': test_id,
            'profile': result['profile'], 'scenario': args.scenario, 'files': files})
        test_event(run, {'event': 'finished', 'testId': test_id, 'profile': result['profile'],
            'scenario': args.scenario, 'ok': result['ok'], 'elapsedSeconds': result['elapsedSeconds'],
            'collectionIncomplete': bool(result.get('collectionIncomplete')),
            'operationId': result.get('operationId'), 'error': result.get('error'),
            'evidenceDirectory': result['evidenceDirectory']})
        if not args.keep:
            try:
                reset(run)
                if result['ok'] and not result.get('collectionIncomplete'):
                    cleanup_status = cleanup_overlay(run)
                else:
                    cleanup_status = {'status': 'retained', 'reason': 'failed-or-incomplete-scenario'}
            except (LabError, OSError, ValueError, subprocess.TimeoutExpired) as error:
                cleanup_status = {'status': 'retained', 'error': str(error)}
            write_json(run / 'overlay-cleanup-status.json', cleanup_status)
    return result


def verify_installed_binaries(run, deb):
    # Development rebuilds retain their version. Confirm actual guest bytes rather
    # than recording only the requested archive's hash.
    import tempfile
    with tempfile.TemporaryDirectory(prefix='putgd-package-') as directory:
        if str(deb).endswith('.rpm'):
            archive = subprocess.Popen(['rpm2cpio', str(Path(deb).resolve())], stdout=subprocess.PIPE)
            unpacked = subprocess.run(['cpio', '-idm', '--quiet'], stdin=archive.stdout, cwd=directory)
            archive.stdout.close()
            if archive.wait() or unpacked.returncode:
                raise LabError('Could not extract the requested RPM')
        else:
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


def stage_install_helper(run, remote, helper, digest):
    # Old immutable goldens grant apt-get, not this new helper. Use that existing
    # authority only in the disposable overlay. Staging uses no repositories
    # and disables list cleanup; normal apt work belongs to the supervisor.
    hook = (f'set -eu; install -d -m 755 /usr/local/lib/putgd-lab; '
            f'install -o root -g root -m 755 "SOURCE" {helper}; '
            f'echo "{digest}  {helper}" | sha256sum -c -; '
            'printf "lab ALL=(root) NOPASSWD: /usr/local/lib/putgd-lab/package_install.py\\n" '
            '> /etc/sudoers.d/putgd-lab-install; '
            'chmod 440 /etc/sudoers.d/putgd-lab-install; '
            'visudo -cf /etc/sudoers.d/putgd-lab-install; exit 1')
    # sudo resets HOME on some guests, so expand the caller's path before sudo.
    before, after = hook.split('SOURCE')
    script = (f'set -eu\nsource="$HOME/{remote}/package_install.py"\n'
              f'echo "{digest}  $source" | sha256sum -c -\n'
              f'hook={shlex.quote(before)}"$source"{shlex.quote(after)}\n'
              'LC_ALL=C sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null '
              '-o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 '
              '-o "APT::Update::Pre-Invoke::=$hook" update '
              '> ~/putgd-lab/' + remote.split('/')[-1] + '/staging.log 2>&1 || true\n'
              # Return only controlled lock metadata: ssh deliberately suppresses
              # guest stderr, which may contain private data. Apt's own failed
              # acquisition identifies the actual holder without a precheck race.
              f'if grep -Fq "Could not get lock /var/lib/apt/lists/lock" ~/{remote}/staging.log; then\n'
              f'python3 - "$HOME/{remote}/staging.log" <<\'PYAPTLOCK\'\n'
              'import json, re, sys\n'
              'with open(sys.argv[1]) as stream: log = stream.read(32768)\n'
              'holder = re.search(r"held by process ([0-9]+)(?: \\(([a-zA-Z0-9_.+-]+)\\))?", log)\n'
              'print("putgd-staging-apt-lock " + json.dumps({"pid": holder[1] if holder else "unknown",\n'
              '    "process": holder[2] if holder and holder[2] else "unknown"}))\n'
              'PYAPTLOCK\nexit 0\nfi\n'
              f'test -x {helper}\n'
              f'echo "{digest}  {helper}" | sha256sum -c -\n'
              f'test "$(stat -c %u:%a {helper})" = 0:755\n'
              f'sudo -n -l {helper} /tmp/check 1 1 >/dev/null\n')
    lifetime = (HERE / 'package_staging.py').read_text()
    invocation = 'python3 -c ' + shlex.quote(lifetime) + ' run "$HOME/' + remote + '" ' + shlex.quote(script)
    # The detached subreaper outlives SSH; only its terminal receipt proves all
    # staging descendants exited. Keep the original output/exit status separate.
    output = ssh(run, f'nohup {invocation} > ~/{remote}/staging-output.log 2>&1 < /dev/null &\n'
                      'staging_pid=$!\nwait "$staging_pid"\ncode=$?\n'
                      f'cat ~/{remote}/staging-output.log\nexit "$code"\n')
    for line in output.splitlines():
        if line.startswith('putgd-staging-apt-lock '):
            holder = json.loads(line.split(' ', 1)[1])
            raise LabError(f'Guest helper staging blocked by apt lists lock /var/lib/apt/lists/lock: '
                           f'holder PID {holder["pid"]} ({holder["process"]}); '
                           f'apt-daily or Mint Update may be active. Wait for the holder to finish; '
                           f'inspect ~/{remote}/staging.log.')


def reconcile_staging(run, remote, wait=30):
    lifetime = (HERE / 'package_staging.py').read_text()
    script = ('python3 -c ' + shlex.quote(lifetime) + ' reconcile "$HOME/' + remote + '" ' + str(wait) + '\n')
    report = json.loads(ssh(run, script, timeout=wait + 10))
    if not isinstance(report, dict) or type(report.get('finished')) is not bool:
        raise LabError('Invalid staging reconciliation receipt')
    return report


def recover_staging(run, job):
    if not re.fullmatch(r'install-[0-9a-f]{32}', job):
        raise LabError('Recovery requires the exact held install job ID')
    remote = 'putgd-lab/' + job
    if not (Path(run) / job / 'staging-failed.json').is_file():
        raise LabError('No controller staging-failure receipt; reconcile the active controller before recovery')
    verify_owned_guest(run)
    # Recovery is exclusively for pre-supervisor staging. Never clear an
    # install supervisor's lock or a replacement job's token.
    ssh(run, f'set -eu\ntest "$(cat ~/putgd-lab/install-active/job)" = {job}\n'
             f'test ! -e ~/{remote}/progress.json && test ! -e ~/{remote}/result.json\n')
    report = reconcile_staging(run, remote)
    if report['finished'] is not True:
        raise LabError(f'Staging {job} remains held ({report["state"]}); install-active blocks retries. '
                       f'Inspect ~/{remote}/staging-process.json and staging-result.json; '
                       'do not remove the lock while staging may still be running.')
    ssh(run, f'set -eu\ntest "$(cat ~/putgd-lab/install-active/job)" = {job}\n'
             f'test ! -e ~/{remote}/progress.json && test ! -e ~/{remote}/result.json\n'
             'rm ~/putgd-lab/install-active/job\nrmdir ~/putgd-lab/install-active\n')
    return report


def install_package(run, deb, install_timeout=None, install_recovery_timeout=None):
    if Path(deb).suffix.lower() != '.deb':
        raise LabError('Guest apt installation requires a .deb package; RPM/dnf installation uses physical.py')
    profile = load_profile(state(run)['profile'])
    timeout = install_timeout if install_timeout is not None else profile.get('install_timeout', 900)
    recovery = (install_recovery_timeout if install_recovery_timeout is not None
                else profile.get('install_recovery_timeout', 600))
    if any(type(value) is not int or value <= 0 for value in (timeout, recovery)):
        raise LabError('Install and recovery timeouts must be positive integer seconds')
    helper = '/usr/local/lib/putgd-lab/package_install.py'
    helper_sha256 = hash_file(HERE / 'package_install.py')
    token = 'install-' + uuid.uuid4().hex
    def release_lock():
        # Do not clear a replacement job if an operator reconciled this one.
        ssh(run, f'if [ "$(cat ~/putgd-lab/install-active/job)" = {token} ]; then\n'
                 'rm ~/putgd-lab/install-active/job\nrmdir ~/putgd-lab/install-active\nfi\n')
    remote = 'putgd-lab/' + token
    evidence = Path(run) / token
    # Acquire before staging or creating host evidence. A refused retry reports
    # the held job's durable state and leaves no empty host install directory.
    held = ssh(run, f'set -eu\numask 077\nmkdir -p ~/putgd-lab\n'
        f'if mkdir ~/putgd-lab/install-active 2>/dev/null; then\n'
        f'printf %s {token} > ~/putgd-lab/install-active/job\nmkdir ~/{remote}\n'
        "else\npython3 - <<'PYLOCK'\n"
        'import datetime, json, pathlib, re\n'
        'root = pathlib.Path.home() / "putgd-lab"\n'
        'try: job = (root / "install-active/job").read_text().strip()\n'
        'except OSError: job = "unknown"\n'
        'status, started, result = "staging", "unknown", "unknown"\n'
        'try: started = datetime.datetime.fromtimestamp((root / "install-active/job").stat().st_mtime, datetime.timezone.utc).isoformat()\n'
        'except OSError: pass\n'
        'if re.fullmatch(r"install-[0-9a-f]{32}", job):\n'
        '    result = str(root / job / "result.json")\n'
        '    for name in ("result.json", "progress.json"):\n'
        '        try:\n'
        '            report = json.loads((root / job / name).read_text())\n'
        '            if not isinstance(report, dict): continue\n'
        '            status = "running" if name == "progress.json" else (\n'
        '                "aborted" if report.get("aborted") else "succeeded" if report.get("ok") else "failed")\n'
        '            break\n'
        '        except (OSError, ValueError): pass\n'
        'print(json.dumps({"heldJob": job, "state": status, "startedAt": started, "resultPath": result}))\n'
        'PYLOCK\nfi\n')
    if held.strip():
        raise LabError('Install retry refused: ' + held.strip() + '; inspect this job before recovery.')
    staging_attempted = False
    staging_finished = False
    try:
        evidence.mkdir(mode=0o700)
        write_json(evidence / 'request.json', {'timeoutSeconds': timeout, 'recoverySeconds': recovery,
                                             'remoteDirectory': remote, 'helperSha256': helper_sha256})
        put(run, str(HERE / 'package_install.py'), remote + '/package_install.py')
        staging_attempted = True
        stage_install_helper(run, remote, helper, helper_sha256)
        staging_finished = True
        write_json(evidence / 'helper.json', {'sha256': helper_sha256, 'verified': True,
                   'source': remote + '/package_install.py', 'installedPath': helper,
                   'route': 'existing passwordless apt-get Pre-Invoke; no sources or list cleanup'})
        put(run, str(Path(deb).resolve()), remote + '/package.deb')
    except BaseException as staging_error:
        try:
            write_json(evidence / 'staging-failed.json', {'error': type(staging_error).__name__})
        except OSError:
            pass  # Evidence IO must not bypass lifetime reconciliation.
        # Log collection is independent of lifetime proof, including its own
        # cancellation. An uncertain guest outcome must retain the job token.
        try:
            get(run, remote + '/staging.log', evidence / 'staging.log', timeout=10)
        except (LabError, OSError, subprocess.TimeoutExpired):
            pass
        finally:
            interrupted = sys.exc_info()[1]
            try:
                report = ({'finished': True, 'state': 'staging-not-started'} if not staging_attempted
                          else {'finished': True, 'state': 'staging-finished'} if staging_finished
                          else reconcile_staging(run, remote))
                try:
                    write_json(evidence / 'staging-reconciliation.json', report)
                except OSError:
                    pass
                if report['finished'] is not True:
                    raise LabError('Staging exit is unconfirmed (' + report['state'] + ')')
                release_lock()
            except (LabError, OSError, ValueError, subprocess.TimeoutExpired) as error:
                message = (f'Install staging job {token} retains install-active; retries are blocked. '
                           f'Inspect ~/{remote}/staging-process.json and staging-result.json. '
                           f'When reachable, run tools/lab/lab.py recover-staging --run {run} --job {token}. '
                           f'Reconciliation: {type(error).__name__}.')
                if isinstance(interrupted, (KeyboardInterrupt, SystemExit)):
                    print(message, file=sys.stderr)
                else:
                    raise LabError(message) from interrupted
        raise
    try:
        ssh(run, f'nohup sudo -n {helper} "$HOME/{remote}" {timeout} {recovery} '
                 f'> ~/{remote}/supervisor.log 2>&1 < /dev/null &\n')
    except (LabError, subprocess.TimeoutExpired):
        # The remote launch may have happened even when its SSH reply was lost.
        # Reconcile the same job below; never launch a replacement.
        pass
    # The detached guest watchdog survives a controller/SSH timeout. Never start
    # a replacement. Its own pidfds bound package recovery independently of SSH.
    startup_error = None
    startup_deadline = time.monotonic() + 15
    deadline = time.monotonic() + timeout + recovery + 120
    while True:
        try:
            output = ssh(run, f'if test -f ~/{remote}/result.json; then cat ~/{remote}/result.json; '
                         f'elif ! test -f ~/{remote}/progress.json; then '
                         "printf '%s\\n' '{\"startupPending\":true}'; fi\n", timeout=10)
            if output.strip():
                report = json.loads(output)
                if not report.get('startupPending'):
                    break
                if time.monotonic() >= startup_deadline:
                    try:
                        get(run, remote + '/supervisor.log', evidence / 'supervisor.log')
                    except (LabError, subprocess.TimeoutExpired):
                        pass
                    startup_error = (f'Install supervisor did not start: no progress.json after 15 seconds; '
                                   f'inspect {evidence}/supervisor.log (may be missing); '
                                   f'job {token} retains install-active pending reconciliation.')
                    break
        except (LabError, subprocess.TimeoutExpired):
            pass
        if time.monotonic() >= deadline:
            raise LabError(f'Install outcome unavailable; guest watchdog owns recovery in ~/{remote}; '
                           'install-active blocks retries. Inspect result.json and dpkg state before recovery.')
        time.sleep(1)
    if startup_error:
        raise LabError(startup_error)
    write_json(evidence / 'result.json', report)
    try:
        for name in ('apt.log', 'progress.json', 'supervisor.log', 'staging.log',
                     'staging-process.json', 'staging-result.json', 'staging-output.log'):
            get(run, remote + '/' + name, evidence / name)
        if not report.get('ok'):
            raise LabError(f'Install failed; dpkg state and owned PIDs retained in {evidence}/result.json; '
                           'install-active blocks retries pending recovery.')
        installed = verify_installed_binaries(run, deb)
    finally:
        # A successful report proves apt stopped and dpkg is clean. Hold the
        # lock through binary verification to prevent a concurrent replacement,
        # but release it even if collection/verification fails afterward.
        if report.get('ok'):
            release_lock()
    return {'sha256': hash_file(deb), 'filename': Path(deb).name, 'installedBinarySha256': installed,
            'installEvidence': str(evidence), 'timedOut': report['timedOut']}


APPIMAGE_SHIMS = {
    # AppRun registers the KWin permission entry for the mounted resident and
    # forwards every argument to the bundled putgd-launcher.
    'putgd-launcher': '#!/bin/sh\nexec "$HOME/{name}" "$@"\n',
    # The CLI has no AppRun route; run the extracted copy with the image's own
    # libraries, exactly as AppRun does for the resident.
    'putgd-cli': '#!/bin/sh\nroot="$HOME/putgd-lab/appimage/squashfs-root"\n'
                 'export LD_LIBRARY_PATH="$root/usr/lib:$root/usr/lib/putgd${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"\n'
                 'export QT_PLUGIN_PATH="$root/usr/plugins:$root/usr/lib/qt6/plugins${{QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}}"\n'
                 'exec "$root/usr/bin/putgd-cli" "$@"\n',
}


def install_appimage(run, appimage):
    """Place the AppImage in the guest home, verify its bytes and record the bundled binaries.

    Image-based guests (Aurora) have no package manager: the AppImage is the
    installed artifact. The receipt keeps the package layout (`/usr/bin/putgd`
    keys) because the resident runs from the image's usr/bin and
    verify_running_app hashes that executable.
    """
    source = Path(appimage).resolve()
    name = source.name
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*\.AppImage', name):
        raise LabError('AppImage filename must be a plain name ending in .AppImage')
    expected_sha256 = hash_file(source)
    quoted = shlex.quote(name)
    put(run, str(source), name)  # relative to the guest home (/var/home/lab on Aurora)
    shims = ''.join(
        f'printf %s {shlex.quote(body.format(name=name))} > ~/putgd-lab/bin/{shim}\nchmod 755 ~/putgd-lab/bin/{shim}\n'
        for shim, body in APPIMAGE_SHIMS.items())
    script = ('set -eu\ncd ~\n'
              f'chmod +x {quoted}\n'
              f'actual=$(sha256sum {quoted} | cut -d" " -f1)\n'
              f'[ "$actual" = {expected_sha256} ] || exit 65\n'
              'mkdir -p ~/putgd-lab/bin\nrm -rf ~/putgd-lab/appimage\nmkdir -p ~/putgd-lab/appimage\n'
              f'(cd ~/putgd-lab/appimage && "$HOME/"{quoted} --appimage-extract >/dev/null)\n'
              + shims +
              'printf \'{"home":"%s","version":\' "$HOME"\n'
              f'"$HOME/"{quoted} --version-json\n'
              'printf \',"binaries":\'\n'
              'sha256sum ~/putgd-lab/appimage/squashfs-root/usr/bin/putgd* | python3 -c '
              '\'import json,sys; print(json.dumps(dict(line.split()[::-1] for line in sys.stdin.read().splitlines())), end="")\'\n'
              'printf \'}\\n\'\n')
    report = json.loads(ssh(run, script, timeout=300))
    installed = {}
    for path, digest in report['binaries'].items():
        binary = Path(path).name
        if not binary.startswith('putgd') or not re.fullmatch(r'[0-9a-f]{64}', digest):
            raise LabError('Unexpected bundled binary inventory')
        installed['/usr/bin/' + binary] = digest
    if '/usr/bin/putgd' not in installed:
        raise LabError('AppImage has no putgd application binary')
    version = report['version']
    if not isinstance(version, dict) or not version.get('version'):
        raise LabError('AppImage --version-json did not report an identity')
    return {'sha256': expected_sha256, 'filename': name, 'format': 'appimage',
            'installedPath': report['home'] + '/' + name, 'version': version,
            'installedBinarySha256': installed}


def install(run, deb=None, appimage=None, install_timeout=None, install_recovery_timeout=None):
    # A failed new install must not certify the previous package.
    (Path(run) / 'package.json').unlink(missing_ok=True)
    wanted = install_format(load_profile(state(run)['profile']))
    if appimage is not None and wanted != 'appimage':
        raise LabError('This profile installs a distro package; pass --deb')
    if deb is not None and wanted != 'deb':
        raise LabError('This profile installs an AppImage; pass --appimage')
    receipt = (install_appimage(run, appimage) if appimage is not None else
               install_package(run, deb, install_timeout, install_recovery_timeout))
    write_json(Path(run) / 'package.json', receipt)
    return {'status': 'installed', 'format': receipt.get('format', 'deb'), 'sha256': receipt['sha256'],
            **{key: receipt[key] for key in ('installEvidence', 'timedOut') if key in receipt}}


def start(run, timeout=30):
    """Start the installed resident in the native session and prove it is the verified build."""
    acceptance_preflight(run, apply=True, stage='start')
    guest_session(run, 'mkdir -p ~/putgd-lab\nnohup putgd-launcher --background > ~/putgd-lab/app.log 2>&1 < /dev/null &\n')
    deadline = time.monotonic() + timeout
    while True:
        try:
            capabilities = cli(run, 'capabilities')
            break
        except (LabError, ValueError, subprocess.TimeoutExpired):
            if time.monotonic() >= deadline:
                raise LabError('Resident did not answer the production CLI in time')
            time.sleep(1)
    resident = verify_running_app(run)
    return {'status': 'started', 'resident': resident,
            'source': capabilities.get('source', {}).get('adapter')}


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
    launch_parser.add_argument('--heads', type=int, help=f'Virtual monitors (1-{MAX_HEADS}); overrides the profile')
    launch_parser.add_argument('--layout', help='Per-head WxH+X+Y list in head order, e.g. 2560x1440+0+0,1920x1080-1920+0')
    for name in ('recover-staging', 'reset', 'cleanup', 'collect', 'screenshot', 'key', 'input', 'install', 'start', 'run', 'cli', 'layout', 'preflight'):
        child = sub.add_parser(name)
        child.add_argument('--run', required=True)
        if name == 'preflight':
            child.add_argument('--apply', action='store_true', help='Apply disposable guest idle policy before verifying it')
            child.add_argument('--stage', default='manual', help='Receipt label for before/after a measurement stage')
        if name == 'cleanup':
            child.add_argument('--evidence-manifest', help='Verified completion manifest for a custom or reviewed failed run')
            child.add_argument('--dry-run', action='store_true', help='Validate retirement without removing the disk')
            child.add_argument('--images-root', help="Absolute work/lab-images holding the run's golden, when it is not this checkout's")
        if name == 'cli':
            child.add_argument('--method', required=True)
            child.add_argument('--params', default='{}', help='Production JSON parameters (credential references only)')
        if name == 'screenshot':
            child.add_argument('--head', type=int, default=0)
        if name == 'key':
            child.add_argument('keys', nargs='+')
        if name == 'input':
            child.add_argument('events', help='JSON QMP input-send-event events array')
            child.add_argument('--head', type=int, help="Send to this head's tablet (multi-head runs)")
        if name == 'layout':
            child.add_argument('--layout', help="WxH+X+Y list in head order; defaults to the run's launch layout")
            child.add_argument('--settle', type=int, default=30, help='Seconds to wait for the guest to report the outputs')
        if name == 'recover-staging':
            child.add_argument('--job', required=True, help='Exact held staging job ID')
        if name == 'install':
            child.add_argument('--install-timeout', type=int, help='Apt budget in seconds (profile install_timeout, default 900)')
            child.add_argument('--install-recovery-timeout', type=int, help='Wait for owned processes after timeout (profile install_recovery_timeout, default 600)')
            package = child.add_mutually_exclusive_group(required=True)
            package.add_argument('--deb', help='Distro package for apt profiles')
            package.add_argument('--appimage', help='AppImage for image-based guests (profile install: appimage)')
        if name == 'start':
            child.add_argument('--timeout', type=int, default=30)
        if name == 'run':
            child.add_argument('--timeout', type=int, default=45)
            child.add_argument('--keep', action='store_true')
            child.add_argument('--scenario', choices=SCENARIOS, default='region')
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
        elif args.action == 'cleanup':
            value = cleanup_overlay(args.run, evidence_manifest=args.evidence_manifest, dry_run=args.dry_run,
                                    images_root=args.images_root)
        elif args.action == 'collect':
            value = collect(args.run)
        elif args.action == 'screenshot':
            value = screenshot(args.run, head=args.head)
        elif args.action == 'key':
            key(args.run, args.keys)
            value = {'status': 'input-sent'}
        elif args.action == 'input':
            input_events(args.run, json.loads(args.events), args.head)
            value = {'status': 'input-sent'}
        elif args.action == 'layout':
            value = layout(args.run, args.layout, args.settle)
        elif args.action == 'cli':
            value = cli(args.run, args.method, json.loads(args.params))
        elif args.action == 'recover-staging':
            value = recover_staging(args.run, args.job)
        elif args.action == 'install':
            value = install(args.run, deb=args.deb, appimage=args.appimage,
                            install_timeout=args.install_timeout, install_recovery_timeout=args.install_recovery_timeout)
        elif args.action == 'start':
            value = start(args.run, timeout=args.timeout)
        elif args.action == 'preflight':
            value = guest_preflight(args.run, apply=args.apply, stage=args.stage)
        else:
            value = fixture_run(args)
        print(json.dumps(value))
        return 1 if isinstance(value, dict) and value.get('ok') is False else 0
    except (LabError, OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 1


if __name__ == '__main__':
    sys.exit(main())
