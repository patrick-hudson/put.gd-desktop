#!/usr/bin/env python3
"""Fail-closed idle policy for an owned disposable QEMU guest, never a host.

The host-side lab preflight authenticates the run before sending this helper.
Use the real desktop user's configuration, not put.gd's isolated XDG homes.
"""
import argparse
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import subprocess
import time


class PolicyError(RuntimeError):
    pass


class DesktopNotReady(PolicyError):
    pass


def command(*args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30)
    if result.returncode:
        # This helper's arguments are policy keys/values, never credentials.
        raise PolicyError('Required guest policy command failed: ' + shlex.join(args)
                          + ': ' + result.stderr.strip()[:1000])
    return result.stdout.strip()


def settings(desktop):
    if desktop not in ('GNOME', 'Cinnamon'):
        raise PolicyError('Unsupported GSettings desktop')
    prefix = 'org.gnome' if desktop == 'GNOME' else 'org.cinnamon'
    policy = [(prefix + '.desktop.session', 'idle-delay', 'uint32 0'),
              (prefix + '.desktop.screensaver', 'lock-enabled', 'false'),
              (prefix + '.desktop.screensaver', 'idle-activation-enabled', 'false')]
    power = prefix + '.settings-daemon.plugins.power'
    for supply in ('ac', 'battery'):
        policy += [(power, 'sleep-inactive-' + supply + '-timeout', '0'),
                   (power, 'sleep-inactive-' + supply + '-type', "'nothing'")]
    if desktop == 'GNOME':
        policy.append((power, 'idle-dim', 'false'))
    else:
        for supply in ('ac', 'battery'):
            policy += [(power, 'idle-dim-' + supply, 'false'),
                       (power, 'sleep-display-' + supply, '0')]
        policy.append((power, 'lock-on-suspend', 'false'))
    return policy


def desktop_environment(process, user, proc=Path('/proc')):
    """Recover desktop homes as well as the bus/display; reject ambiguous shells."""
    account = pwd.getpwnam(user)
    if os.getuid() != account.pw_uid or os.getuid() == 0:
        raise PolicyError('Run as the disposable guest user')
    if process not in ('gnome-shell', 'cinnamon', 'plasmashell'):
        raise PolicyError('Unsupported desktop session process')
    matches = []
    for path in proc.iterdir():
        if not path.name.isdecimal():
            continue
        try:
            if path.stat().st_uid == account.pw_uid and (path / 'comm').read_text().strip() == process:
                matches.append(dict(item.split('=', 1) for item in
                                    (path / 'environ').read_text().split('\0') if '=' in item))
        except (OSError, ProcessLookupError):
            continue
    if not matches:
        raise DesktopNotReady('Actual desktop shell is not ready')
    if len(matches) != 1:
        raise PolicyError('Need exactly one actual desktop shell')
    session = matches[0]
    env = dict(os.environ, HOME=account.pw_dir, LC_ALL='C')
    # Unset inherited app sandbox homes when absent from the desktop environment.
    for name in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_STATE_HOME', 'XDG_CACHE_HOME',
                 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS', 'DISPLAY', 'XAUTHORITY', 'WAYLAND_DISPLAY'):
        env.pop(name, None)
        if session.get(name):
            env[name] = session[name]
    env['XDG_RUNTIME_DIR'] = '/run/user/' + str(account.pw_uid)
    env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + env['XDG_RUNTIME_DIR'] + '/bus'
    return env


def active_session(expected_type):
    matches = []
    for line in command('loginctl', 'list-sessions', '--no-legend').splitlines():
        name = line.split()[0]
        values = dict(line.split('=', 1) for line in command(
            'loginctl', 'show-session', name, '-p', 'User', '-p', 'Type', '-p', 'Active',
            '-p', 'LockedHint', '-p', 'Seat', '-p', 'State').splitlines() if '=' in line)
        if values.get('User') == str(os.getuid()) and values.get('Seat') == 'seat0' and values.get('Type') in ('x11', 'wayland'):
            matches.append(dict(values, id=name))
    if len(matches) != 1:
        raise PolicyError('Need one graphical seat0 session')
    session = matches[0]
    if session.get('Type') != expected_type or session.get('Active') != 'yes' or session.get('LockedHint') != 'no':
        raise PolicyError('Guest session is locked, inactive, or the wrong native session type')
    return session


def apply_gsettings(desktop, apply):
    result = []
    for schema, key, wanted in settings(desktop):
        if apply:
            command('gsettings', 'set', schema, key, wanted)
        actual = command('gsettings', 'get', schema, key)
        if actual != wanted:
            raise PolicyError('Idle policy readback mismatch: ' + schema + ' ' + key)
        result.append({'schema': schema, 'key': key, 'value': actual})
    return result


def screensaver_inactive(desktop):
    service = 'org.gnome.ScreenSaver' if desktop == 'GNOME' else 'org.cinnamon.ScreenSaver'
    path = '/' + service.replace('.', '/')
    if command('busctl', '--user', 'call', service, path, service, 'GetActive') != 'b false':
        raise PolicyError('Guest screensaver remains active; restore the visible console first')


def desktop_services(desktop, apply):
    if desktop == 'Cinnamon':
        # This service is demand-activated and may legitimately have no owner.
        # The required GetActive call below activates it and verifies its state.
        return
    services = (['org.kde.Solid.PowerManagement', 'org.freedesktop.ScreenSaver']
                if desktop == 'KDE' else
                ['org.gnome.ScreenSaver' if desktop == 'GNOME' else 'org.cinnamon.ScreenSaver'])
    deadline = time.monotonic() + (30 if apply else 0)
    for service in services:
        while command('busctl', '--user', 'call', 'org.freedesktop.DBus',
                      '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                      'NameHasOwner', 's', service) != 'b true':
            if time.monotonic() >= deadline:
                raise PolicyError('Required desktop policy service is not ready: ' + service)
            time.sleep(.25)


def kde_policy(apply):
    version = command('plasmashell', '--version')
    match = re.search(r'\b([56])\.\d+', version)
    if not match:
        raise PolicyError('Unsupported Plasma policy version')
    major = match[1]
    write, read = 'kwriteconfig' + major, 'kreadconfig' + major
    policy = [('kscreenlockerrc', ['Daemon'], 'Autolock', 'false'),
              ('kscreenlockerrc', ['Daemon'], 'LockOnResume', 'false')]
    for supply in ('AC', 'Battery', 'LowBattery'):
        if major == '6':
            policy += [('powerdevilrc', [supply, 'Display'], 'DimDisplayWhenIdle', 'false'),
                       ('powerdevilrc', [supply, 'Display'], 'TurnOffDisplayWhenIdle', 'false'),
                       ('powerdevilrc', [supply, 'SuspendAndShutdown'], 'AutoSuspendAction', '0')]
        else:
            policy += [('powermanagementprofilesrc', [supply, 'DimDisplay'], 'idleTime', '0'),
                       ('powermanagementprofilesrc', [supply, 'DPMSControl'], 'idleTime', '0'),
                       ('powermanagementprofilesrc', [supply, 'SuspendSession'], 'idleTime', '0'),
                       ('powermanagementprofilesrc', [supply, 'SuspendSession'], 'suspendType', '0')]
    result = []
    for file, groups, key, wanted in policy:
        arguments = ['--file', file]
        for group in groups:
            arguments += ['--group', group]
        arguments += ['--key', key]
        if apply:
            command(write, *arguments, wanted)
        actual = command(read, *arguments, '--default', '__missing__')
        if actual != wanted:
            raise PolicyError('Plasma idle policy readback mismatch: ' + file + '/' + key)
        result.append({'file': file, 'groups': groups, 'key': key, 'value': actual})
    if apply:
        command('busctl', '--user', 'call', 'org.kde.Solid.PowerManagement',
                '/org/kde/Solid/PowerManagement', 'org.kde.Solid.PowerManagement', 'reparseConfiguration')
        command('busctl', '--user', 'call', 'org.freedesktop.ScreenSaver', '/ScreenSaver',
                'org.kde.screensaver', 'configure')
    if command('busctl', '--user', 'call', 'org.freedesktop.ScreenSaver', '/ScreenSaver',
               'org.freedesktop.ScreenSaver', 'GetActive') != 'b false':
        raise PolicyError('Plasma screensaver remains active')
    return result


UNIT = '''[Unit]
Description=Keep an owned disposable put.gd lab guest visible
[Service]
ExecStart=/usr/bin/systemd-inhibit --what=idle:sleep --who=putgd-lab-guest --why=visible-acceptance --mode=block /usr/bin/sleep infinity
Restart=on-failure
[Install]
WantedBy=default.target
'''


def inhibitor(apply):
    name = 'putgd-lab-visible.service'
    config = Path(os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')))
    unit = config / 'systemd/user' / name
    if apply:
        if unit.is_symlink() or (unit.exists() and unit.read_text() != UNIT):
            raise PolicyError('Guest visibility unit ownership collision')
        unit.parent.mkdir(parents=True, exist_ok=True)
        unit.write_text(UNIT)
        command('systemctl', '--user', 'daemon-reload')
        command('systemctl', '--user', 'enable', '--now', name)
    if command('systemctl', '--user', 'is-active', name) != 'active':
        raise PolicyError('Guest visibility inhibitor is inactive')
    if command('systemctl', '--user', 'is-enabled', name) != 'enabled':
        raise PolicyError('Guest visibility inhibitor is not enabled for future logins')
    # Type=simple becomes active before systemd-inhibit acquires its logind FD.
    # Only initial application may wait; boundary checks must observe it now.
    deadline = time.monotonic() + (5 if apply else 0)
    while True:
        document = json.loads(command('busctl', '--system', '--json=short', 'call',
                                  'org.freedesktop.login1', '/org/freedesktop/login1',
                                  'org.freedesktop.login1.Manager', 'ListInhibitors'))
        inhibitors = document['data'][0]
        found = [row for row in inhibitors if row[1] == 'putgd-lab-guest' and row[3] == 'block'
                 and row[4] == os.getuid() and {'idle', 'sleep'} <= set(row[0].split(':'))]
        if len(found) == 1:
            break
        if len(found) > 1 or time.monotonic() >= deadline:
            raise PolicyError('Idle/sleep inhibitor was not acquired uniquely')
        time.sleep(.1)
    return {'what': found[0][0], 'who': found[0][1], 'mode': found[0][3], 'uid': found[0][4], 'pid': found[0][5]}


def preflight(desktop, process, user, session_type, apply=False):
    if command('systemd-detect-virt', '--vm') not in ('qemu', 'kvm'):
        raise PolicyError('Refusing to change a non-QEMU/KVM machine')
    deadline = time.monotonic() + (30 if apply else 0)
    while True:
        try:
            environment = desktop_environment(process, user)
            break
        except DesktopNotReady:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.25)
    os.environ.clear()
    os.environ.update(environment)
    desktop_services(desktop, apply)
    policies = kde_policy(apply) if desktop == 'KDE' else apply_gsettings(desktop, apply)
    power = inhibitor(apply)
    x11 = None
    if session_type == 'x11':
        if apply:
            command('xset', 's', 'off')
            command('xset', 's', 'noblank')
            command('xset', '-dpms')
        x11 = command('xset', 'q')
        if not re.search(r'timeout:\s+0\b', x11) or 'DPMS is Disabled' not in x11:
            raise PolicyError('X11 blanking/DPMS policy is not disabled')
    if desktop in ('GNOME', 'Cinnamon'):
        screensaver_inactive(desktop)
    return {'ok': True, 'desktop': desktop, 'session': active_session(session_type),
            'configurationHome': os.environ.get('XDG_CONFIG_HOME', str(Path.home() / '.config')),
            'policy': policies, 'inhibitor': power, 'x11': x11}


def install_autostart(user):
    """Provision future disposable logins; launch/start still require live proof."""
    if command('systemd-detect-virt', '--vm') not in ('qemu', 'kvm'):
        raise PolicyError('Refusing to provision a non-QEMU/KVM machine')
    account = pwd.getpwnam(user)
    if account.pw_uid == 0 or os.getuid() not in (0, account.pw_uid):
        raise PolicyError('Only root or the disposable user may provision its startup policy')
    home = Path(account.pw_dir)
    target = home / '.local/lib/putgd-lab/guest_idle.py'
    entry = home / '.config/autostart/putgd-lab-visible.desktop'
    contents = '[Desktop Entry]\nType=Application\nName=Disposable lab visibility preflight\nExec=/usr/bin/python3 "' + str(target) + '" --user ' + user + ' --autodetect --apply\nNoDisplay=true\n'
    if not re.fullmatch(r'[a-z_][a-z0-9_-]*', user) or '"' in str(home) or '\n' in str(home):
        raise PolicyError('Unsupported disposable account path')
    files = [(target, Path(__file__).read_bytes()), (entry, contents.encode())]
    for path, data in files:
        if path.is_symlink() or (path.exists() and path.read_bytes() != data):
            raise PolicyError('Guest startup policy path collision')
    for path, data in files:
        # Do not create root-owned intermediate directories in the guest home.
        missing = []
        parent = path.parent
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for directory in reversed(missing):
            directory.mkdir(mode=0o700)
            if os.getuid() == 0:
                os.chown(directory, account.pw_uid, account.pw_gid)
        path.write_bytes(data)
        path.chmod(0o600)
        if os.getuid() == 0:
            os.chown(path, account.pw_uid, account.pw_gid)
    return {'ok': True, 'status': 'autostart-provisioned-not-accepted', 'user': user}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', choices=['GNOME', 'Cinnamon', 'KDE'])
    parser.add_argument('--process')
    parser.add_argument('--user', required=True)
    parser.add_argument('--session-type', choices=['x11', 'wayland'])
    parser.add_argument('--apply', action='store_true')
    startup = parser.add_mutually_exclusive_group()
    startup.add_argument('--install-autostart', action='store_true')
    startup.add_argument('--autodetect', action='store_true')
    args = parser.parse_args()
    startup_guest_verified = False
    try:
        if args.install_autostart:
            result = install_autostart(args.user)
        else:
            if args.autodetect:
                if command('systemd-detect-virt', '--vm') not in ('qemu', 'kvm'):
                    raise PolicyError('Refusing startup policy on a non-QEMU/KVM machine')
                startup_guest_verified = True
                desktop = os.environ.get('XDG_CURRENT_DESKTOP', '').lower()
                if 'cinnamon' in desktop:
                    args.desktop, args.process = 'Cinnamon', 'cinnamon'
                elif 'gnome' in desktop or 'ubuntu' in desktop:
                    args.desktop, args.process = 'GNOME', 'gnome-shell'
                elif 'kde' in desktop:
                    args.desktop, args.process = 'KDE', 'plasmashell'
                else:
                    raise PolicyError('Unsupported startup desktop')
                args.session_type = os.environ.get('XDG_SESSION_TYPE')
            if not args.desktop or not args.process or args.session_type not in ('x11', 'wayland'):
                raise PolicyError('An explicit native desktop/session is required')
            result = preflight(args.desktop, args.process, args.user, args.session_type, args.apply)
    except (PolicyError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        result = {'ok': False, 'error': str(error)}
    if args.autodetect and startup_guest_verified:
        directory = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'putgd-lab'
        directory.mkdir(parents=True, exist_ok=True)
        receipt = directory / 'guest-idle-startup.json'
        receipt.write_text(json.dumps(result, indent=2) + '\n')
        receipt.chmod(0o600)
    print(json.dumps(result))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
