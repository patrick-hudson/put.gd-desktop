#!/usr/bin/env python3
"""Maintained native acceptance primitives; run on the desktop host under its lab lock.

These commands do not install packages, log out, unlock desktops, change keyrings,
or launch residents. Keep product package and driver source identities separate.
Use --help on each command. JSON receipts contain facts, never config or secrets.
"""
import argparse
from contextlib import closing, contextmanager
import fcntl
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import sqlite3
import stat
import struct
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit


class Refused(RuntimeError):
    """A safe-to-report unmet prerequisite."""
    def __init__(self, message, recovery=None):
        super().__init__(message)
        self.recovery = recovery


def require(condition, message):
    if not condition:
        raise Refused(message)


def command(argv, env=None, timeout=20):
    return subprocess.run(argv, env=env, capture_output=True, text=True, timeout=timeout)


def private(path, directory=False):
    path = Path(path)
    info = path.lstat()
    require((stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode))
            and info.st_uid == os.getuid() and info.st_mode & 0o077 == 0,
            'Expected an owned private regular file/directory')
    return path


def socket_path(runtime):
    path = Path(runtime).resolve() / 'putgd/session.sock'
    require(len(str(path).encode('utf-8')) <= 107, 'Unix socket path exceeds 107 UTF-8 bytes; choose a new short runtime')
    return path


def process(pid, proc=Path('/proc')):
    path = proc / str(int(pid))
    require(path.stat().st_uid == os.getuid(), 'Process belongs to another user')
    fields = (path / 'stat').read_text().rsplit(') ', 1)[1].split()
    return {'pid': int(pid), 'exe': os.readlink(path / 'exe'), 'startTicks': int(fields[19])}


def environment(pid):
    # Never return the unrestricted process environment in a receipt.
    return dict(x.split('=', 1) for x in Path(f'/proc/{pid}/environ').read_bytes().decode().split('\0') if '=' in x)


def same_process(identity):
    try:
        return process(identity['pid']) == identity
    except (OSError, Refused):
        return False


def bus(env, *args):
    reply = command(['busctl', '--user', '--json=short', *map(str, args)], env)
    require(reply.returncode == 0, 'D-Bus call failed: ' + str(args[0]) + ' (no raw diagnostic retained)')
    return json.loads(reply.stdout).get('data') if reply.stdout.strip() else []


def owner(env, service):
    unique = bus(env, 'call', 'org.freedesktop.DBus', '/org/freedesktop/DBus',
                 'org.freedesktop.DBus', 'GetNameOwner', 's', service)[0]
    pid = bus(env, 'call', 'org.freedesktop.DBus', '/org/freedesktop/DBus',
              'org.freedesktop.DBus', 'GetConnectionUnixProcessID', 's', unique)[0]
    return unique, int(pid)


SESSION_KEYS = ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'XDG_CURRENT_DESKTOP',
                'XDG_SESSION_TYPE', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION',
                'XDG_DATA_DIRS', 'XDG_CONFIG_DIRS', 'XDG_MENU_PREFIX', 'XDG_CONFIG_HOME',
                'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'KDE_FULL_SESSION', 'KDE_SESSION_VERSION')


def verified_wayland(identity, exported, runtime):
    """A user-manager export is only a hint: bind it to this shell's socket peer.

    GNOME's compositor often predates WAYLAND_DISPLAY being exported. Never
    accept an unverified user-manager display from a previous graphical login.
    """
    name = exported.get('WAYLAND_DISPLAY')
    require(name, 'Current shell has no display and the user manager has no Wayland hint')
    path = Path(name) if Path(name).is_absolute() else Path(runtime) / name
    path = path.resolve()
    require(path.is_relative_to(Path(runtime).resolve()) and len(str(path).encode()) <= 107,
            'Wayland hint is outside this user runtime or exceeds the Unix socket limit')
    info = path.lstat()
    require(stat.S_ISSOCK(info.st_mode) and info.st_uid == os.getuid(), 'Wayland hint is not an owned socket')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
        probe.settimeout(3)
        probe.connect(str(path))
        peer_pid, peer_uid, _ = struct.unpack('3i', probe.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    require(peer_pid == identity['pid'] and peer_uid == os.getuid() and same_process(identity),
            'User-manager Wayland hint does not belong to the current graphical shell')
    return str(path)


def choose_session(sessions, requested=None):
    matches = [s for s in sessions if s.get('User') == str(os.getuid()) and s.get('Active') == 'yes'
               and s.get('Remote') == 'no' and s.get('Type') in ('x11', 'wayland')
               and s.get('Seat') == 'seat0' and (requested is None or s['Id'] == requested)]
    require(len(matches) == 1, 'Expected exactly one active local seat0 graphical session')
    return matches[0]


def session(requested=None):
    listed = command(['loginctl', 'list-sessions', '--no-legend', '--no-pager'])
    require(listed.returncode == 0, 'Cannot enumerate logind sessions')
    sessions = []
    for line in listed.stdout.splitlines():
        sid = line.split()[0]
        reply = command(['loginctl', 'show-session', sid, '--no-pager'])
        if reply.returncode == 0:
            sessions.append(dict(l.split('=', 1) for l in reply.stdout.splitlines() if '=' in l))
    found = choose_session(sessions, requested)
    env = {'HOME': str(Path.home()), 'USER': os.environ.get('USER', ''), 'PATH': '/usr/local/bin:/usr/bin:/bin',
           'LANG': 'C.UTF-8', 'XDG_RUNTIME_DIR': f'/run/user/{os.getuid()}',
           'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{os.getuid()}/bus'}
    shells = []
    for service, name in (('org.kde.plasmashell', 'plasmashell'), ('org.gnome.Shell', 'gnome-shell'), ('org.Cinnamon', 'cinnamon')):
        try:
            _, pid = owner(env, service)
            identity = process(pid)
            if Path(identity['exe']).name == name:
                shells.append((identity, environment(pid)))
        except (Refused, OSError):
            pass
    start = int(found['TimestampMonotonic'])
    hz = os.sysconf('SC_CLK_TCK')
    shells = [(p, e) for p, e in shells if p['startTicks'] * 1_000_000 // hz >= start - 2_000_000
              and (not e.get('XDG_SESSION_ID') or e['XDG_SESSION_ID'] == found['Id'])]
    require(len(shells) == 1, 'No unique current-session shell; refusing a prior shell/user-manager environment')
    identity, shell_env = shells[0]
    env.update({k: shell_env[k] for k in SESSION_KEYS if shell_env.get(k)})
    env.update({k: shell_env[k] for k in ('LANG', 'LANGUAGE', 'LC_ALL', 'LC_MESSAGES', 'KDESYCOCA') if shell_env.get(k)})
    env.setdefault('XDG_SESSION_TYPE', found['Type'])
    if not env.get('WAYLAND_DISPLAY') and found['Type'] == 'wayland':
        shown = command(['systemctl', '--user', 'show-environment'], env)
        require(shown.returncode == 0, 'Cannot inspect the user-manager Wayland hint')
        exported = dict(l.split('=', 1) for l in shown.stdout.splitlines() if '=' in l)
        env['WAYLAND_DISPLAY'] = verified_wayland(identity, exported, env['XDG_RUNTIME_DIR'])
    require(env.get('DISPLAY') or env.get('WAYLAND_DISPLAY'), 'Current shell has no display environment')
    facts = {k: found.get(k) for k in ('Id', 'Type', 'Seat', 'Desktop', 'Timestamp', 'TimestampMonotonic', 'Leader')}
    facts.update({'shell': identity, 'bootId': Path('/proc/sys/kernel/random/boot_id').read_text().strip()})
    return facts, env


def cli(exe, runtime, method, env, params=None):
    path = socket_path(runtime)
    if not Path(exe).is_file():
        return {'ok': False, 'error': {'code': 'cli_missing'}}
    try:
        result = command([str(exe), '--socket', str(path), method, json.dumps(params or {})], env, timeout=35)
        return json.loads(result.stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return {'ok': False, 'error': {'code': 'cli_unavailable'}}


def checked_resident(pid, exe, runtime=None):
    identity = process(pid)
    require(identity['exe'] == str(Path(exe).resolve()), 'Resident executable does not match the explicitly selected build')
    if runtime is not None:
        require(Path(environment(pid).get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')).resolve() == Path(runtime).resolve(),
                'Resident runtime does not match the selected profile')
    return identity


def tray_target(env, identity):
    require(same_process(identity), 'Resident generation changed')
    items = bus(env, 'get-property', 'org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher',
                'org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems')
    targets = []
    for item in items:
        service, sep, suffix = item.partition('/')
        pid = None
        try:
            unique, pid = owner(env, service)
            if pid != identity['pid']:
                continue
            path = '/' + suffix if sep else '/StatusNotifierItem'
            title = bus(env, 'get-property', unique, path, 'org.kde.StatusNotifierItem', 'Title')
            title = title[0] if isinstance(title, list) else title
            require(isinstance(title, str) and title.lower().startswith(('put.gd', 'putgd')), 'SNI title is not put.gd')
            menu = bus(env, 'get-property', unique, path, 'org.kde.StatusNotifierItem', 'Menu')
            menu = menu[0] if isinstance(menu, list) else menu
            targets.append({'service': unique, 'path': path, 'menu': menu, 'title': title, 'resident': identity})
        except Refused:
            # Unrelated items may disappear between enumeration and lookup.
            if pid == identity['pid']:
                raise
    require(len(targets) == 1, 'Expected one SNI owned by the selected put.gd resident; no coordinate fallback')
    require(same_process(identity), 'Resident generation changed during SNI lookup')
    return targets[0]


def quit_action(layout):
    matches = []
    def walk(node, enabled=True):
        if isinstance(node, dict):
            node = node['data']
        action, props, children = node
        values = {k: v.get('data') if isinstance(v, dict) else v for k, v in props.items()}
        usable = enabled and values.get('visible', True) and values.get('enabled', True)
        if usable and values.get('label', '').replace('&', '').replace('_', '').strip() == 'Quit':
            matches.append(action)
        for child in children:
            walk(child, usable)
    walk(layout)
    require(len(matches) == 1, 'Expected exactly one visible enabled Quit action in the owned tray menu')
    return matches[0]


def tray(args, env):
    require(not os.environ.get('TRAY_XY'), 'TRAY_XY is unsafe and unsupported; unset it')
    identity = checked_resident(args.pid, args.exe)
    target = tray_target(env, identity)
    if args.action == 'menu':
        layout = bus(env, 'call', target['service'], target['menu'], 'com.canonical.dbusmenu', 'GetLayout', 'iias', '--', '0', '-1', '0')
        return {'target': target, 'action': 'menu', 'quitAction': quit_action(layout[1]),
                'pointerInputSent': False, 'menuRendered': False}
    elif args.action == 'activate':
        # Addresses the authenticated tray item. No hit-testing of panels,
        # launchers or desktop background, and no pointer coordinates accepted.
        require(tray_target(env, identity) == target, 'Tray ownership changed before activation')
        bus(env, 'call', target['service'], target['path'], 'org.kde.StatusNotifierItem', 'Activate', 'ii', '0', '0')
    elif args.action == 'quit':
        layout = bus(env, 'call', target['service'], target['menu'], 'com.canonical.dbusmenu', 'GetLayout', 'iias', '--', '0', '-1', '0')
        action = quit_action(layout[1])
        require(tray_target(env, identity) == target, 'Tray ownership changed before activation')
        bus(env, 'call', target['service'], target['menu'], 'com.canonical.dbusmenu', 'Event', 'isvu',
            action, 'clicked', 's', '', '0')
        deadline = time.monotonic() + 20
        while same_process(identity) and time.monotonic() < deadline:
            time.sleep(.1)
        require(not same_process(identity), 'Tray Quit did not stop the selected resident')
    return {'target': target, 'action': args.action, 'pointerInputSent': False}


def plasma_registration(env, repair=False, probe=None):
    require('KDE' in env.get('XDG_CURRENT_DESKTOP', '').upper(), 'Plasma registration check requires the current KDE session')
    # KService queries sycoca, rather than trusting desktop files on disk.
    # Missing tooling is a blocked check, never an empty cache.
    require(probe and Path(probe).is_file(), 'Build the distro-linked tools/lab/sycoca_probe helper and pass --probe')
    def inspect():
        reply = command([str(probe)], env)
        require(reply.returncode == 0, 'KService probe failed')
        return json.loads(reply.stdout)
    before = inspect()
    missing = not before['applicationCount'] or (Path('/usr/share/applications/putgd.desktop').exists() and not before['putgd'])
    if missing and repair:
        reply = command(['kbuildsycoca6', '--noincremental'], env, timeout=60)
        require(reply.returncode == 0, 'Current-session sycoca rebuild failed')
    after = inspect() if missing and repair else before
    return {'before': before, 'after': after, 'repaired': bool(missing and repair),
            'healthy': bool(after['applicationCount']) and (not Path('/usr/share/applications/putgd.desktop').exists() or after['putgd'])}


def resident_processes(runtime, data_root=None):
    result = []
    for folder in Path('/proc').iterdir():
        if not folder.name.isdigit():
            continue
        try:
            identity = process(int(folder.name))
            name = Path(identity['exe'].removesuffix(' (deleted)')).name
            if name != 'putgd' and not name.startswith('putgd-'):
                continue
            try:
                env = environment(identity['pid'])
            except FileNotFoundError:
                continue
            except (OSError, UnicodeError):
                raise Refused('Cannot inspect an owned product process; absence is unproven') from None
            home = Path(env.get('XDG_DATA_HOME') or Path.home() / '.local/share').resolve()
            same_data = data_root is not None and Path(data_root).resolve() in (home / 'putgd', home / 'putgd/putgd')
            if data_root is not None and not same_data:
                db_info = (Path(data_root) / 'history.sqlite').stat()
                try:
                    for fd in (folder / 'fd').iterdir():
                        try: opened = fd.stat()
                        except FileNotFoundError: continue
                        if (opened.st_dev, opened.st_ino) == (db_info.st_dev, db_info.st_ino):
                            same_data = True
                            break
                except FileNotFoundError:
                    continue
                except OSError:
                    raise Refused('Cannot inspect owned product database descriptors; absence is unproven') from None
            if same_data or Path(env.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')).resolve() == Path(runtime).resolve():
                result.append(identity)
        except (FileNotFoundError, ProcessLookupError):
            continue
        except PermissionError:
            # A disappearing/other-user process is harmless. An unreadable owned
            # putgd helper is not evidence of its absence.
            if folder.stat().st_uid == os.getuid() and (folder / 'comm').read_text().startswith('putgd'):
                raise Refused('Cannot identify an owned product helper; absence is unproven') from None
        except Refused as error:
            if str(error) != 'Process belongs to another user':
                raise
    return result


def prepare_login(args, facts, env):
    identity = checked_resident(args.pid, args.exe, args.runtime)
    path = socket_path(args.runtime)
    require(stat.S_ISSOCK(path.lstat().st_mode) and path.lstat().st_uid == os.getuid(), 'Expected an owned resident socket')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
        probe.settimeout(3)
        probe.connect(str(path))
        peer_pid, peer_uid, _ = struct.unpack('3i', probe.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    require(peer_pid == identity['pid'] and peer_uid == os.getuid(), 'Socket peer is not the selected resident')
    reply = cli(args.cli, args.runtime, 'ui.quit', env)
    require(reply.get('ok'), 'Cannot quit the lab resident before logout')
    deadline = time.monotonic() + 25
    while same_process(identity) and time.monotonic() < deadline:
        time.sleep(.1)
    remaining = resident_processes(args.runtime)
    require(not remaining, 'Resident/helper survived ui.quit; logout test would be invalid')
    return {'prepared': True, 'oldSession': facts, 'stopped': identity, 'runtime': str(Path(args.runtime).resolve()),
            'preparedAt': datetime.now(timezone.utc).isoformat()}


def check_login(before, facts, processes, expected='present'):
    """Verify post-login state, not the launch origin of a new resident.

    A new SSH/systemd user-scope start can share the profile and executable.
    Neither the runtime nor start time can establish desktop autostart.
    """
    old = before['oldSession']
    changed = (old['bootId'], old['Id'], old['TimestampMonotonic']) != (facts['bootId'], facts['Id'], facts['TimestampMonotonic'])
    require(before.get('prepared') and changed, 'No verified new graphical session after a prepared logout')
    require(facts['shell'] != old['shell'], 'Prior graphical shell still selected')
    result = {'newSession': facts, 'processes': processes, 'newSessionVerified': True,
              'postLoginStateVerified': True, 'expected': expected, 'autostartProven': False,
              'scope': 'Post-login process presence/absence only; resident launch origin is unverified'}
    if expected == 'absent':
        require(not processes, 'Expected no resident after removal/login')
        return result
    require(processes, 'No resident in the selected profile after login')
    expected_exe = before.get('stopped', {}).get('exe')
    if expected_exe:
        require(any(p['exe'] == expected_exe for p in processes), 'New resident is not the selected package executable')
    hz = os.sysconf('SC_CLK_TCK')
    require(all(p['startTicks'] * 1_000_000 // hz >= int(facts['TimestampMonotonic']) for p in processes),
            'A pre-login resident/helper survived; invalid login evidence')
    return result


def database_snapshot(db):
    if not Path(db).is_file():
        return {'available': False}
    with closing(sqlite3.connect(Path(db).resolve().as_uri() + '?mode=ro', uri=True)) as con:
        tables = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        counts = {t: con.execute('SELECT count(*) FROM ' + quote(t)).fetchone()[0] for t in tables}
        result = {'available': True, 'userVersion': con.execute('PRAGMA user_version').fetchone()[0],
                  'integrity': con.execute('PRAGMA integrity_check').fetchone()[0], 'tables': counts}
        if 'history' in tables:
            result['independentOperations'] = con.execute('SELECT count(DISTINCT operation_id) FROM history').fetchone()[0]
        return result


def snapshot(args, facts, env):
    procs = resident_processes(args.runtime)
    reply = cli(args.cli, args.runtime, 'ui.snapshot', env) if procs else {'ok': False, 'error': {'code': 'resident_absent'}}
    return {'session': facts, 'processes': procs, 'cliExists': Path(args.cli).is_file(),
            'cliStatus': 'ok' if reply.get('ok') else (reply.get('error') or {}).get('code'),
            'build': (reply.get('result') or {}).get('build'), 'history': database_snapshot(args.db) if args.db else None}


def quote(name):
    return '"' + name.replace('"', '""') + '"'


def insert(con, table, row):
    con.execute('INSERT INTO ' + quote(table) + '(' + ','.join(map(quote, row)) + ') VALUES (' + ','.join('?' for _ in row) + ')', list(row.values()))


def remap(value, substitutions):
    if isinstance(value, dict):
        return {k: remap(v, substitutions) for k, v in value.items()}
    if isinstance(value, list):
        return [remap(v, substitutions) for v in value]
    if isinstance(value, str):
        for old, new in substitutions.items():
            value = value.replace(old, new)
    return value


def loopback_url(value):
    try:
        parsed = urlsplit(value)
        return (parsed.scheme in ('http', 'https') and parsed.hostname in ('127.0.0.1', '::1')
                and parsed.username is None and parsed.password is None and parsed.port is not None
                and 1 <= parsed.port <= 65535)
    except (ValueError, TypeError):
        return False


TERMINAL_OPERATIONS = {'completed', 'partial', 'failed', 'cancelled', 'interrupted'}
# StorageReceipt{}.toJson() / Destination{}.toJson(), including every field.
INERT_RECEIPT = {'schemaVersion': 1,
                 'destination': {'providerId': '', 'profileId': '', 'profileRevision': '', 'settings': {}, 'credentials': {}},
                 'objectReference': {}, 'concurrencyToken': '', 'publicationId': '', 'directUrl': ''}


def isolated_receipt(receipt):
    if not isinstance(receipt, dict) or set(receipt) != set(INERT_RECEIPT): return False
    destination = receipt.get('destination')
    return (type(receipt['schemaVersion']) is int and receipt['schemaVersion'] == 1
            and isinstance(destination, dict) and set(destination) == set(INERT_RECEIPT['destination'])
            and destination['providerId'] == 's3'
            and all(isinstance(destination[k], str) and destination[k] for k in ('profileId', 'profileRevision'))
            and isinstance(destination['settings'], dict) and loopback_url(destination['settings'].get('endpoint'))
            and isinstance(receipt['objectReference'], dict) and bool(receipt['objectReference'])
            and isinstance(receipt['publicationId'], str) and bool(receipt['publicationId'])
            and isinstance(receipt['concurrencyToken'], str) and isinstance(receipt['directUrl'], str))


def fixture_network_state(value, terminal=False):
    """Reject external/unknown routes and recovery work, including old journals.

    No rewriting of an external destination into a fixture is allowed. Review
    the whole database, not just the rows chosen as cloning templates.
    """
    if isinstance(value, list):
        for item in value: fixture_network_state(item, terminal)
    elif isinstance(value, dict):
        if 'items' in value and 'state' in value:
            terminal = value['state'] in TERMINAL_OPERATIONS
        provider = value.get('providerId', value.get('provider'))
        if provider:
            require(provider == 's3' and loopback_url(value.get('settings', {}).get('endpoint')),
                    'Fixture contains an external or unknown persisted provider destination')
        for key, item in value.items():
            normalized = key.replace('_', '').lower()
            if key == 'credentials' and item:
                require(isinstance(item, dict) and set(item) <= {'mode', 'reference'}
                        and item.get('mode') == 'session' and not item.get('reference'),
                        'Fixture may use session-only credential references; no keychain or embedded credentials')
            require(not (item and normalized in ('accesskey', 'accesskeyid', 'secretkey', 'secretaccesskey',
                                                 'sessiontoken', 'accesstoken', 'refreshtoken', 'userkeypassword')),
                    'Fixture contains embedded credential material')
            require(not (item and (normalized in ('uploadjournal', 'resume', 'multipart', 'uploadid', 'multipartuploadid',
                                                  'deleteoperationid', 'deleteprogress', 'shareprogress', 'incompleteupload')
                                   or normalized.startswith('cleanup'))),
                    'Fixture contains persisted upload/deletion/cleanup recovery work')
            if key == 'endpoint' and item:
                require(loopback_url(item), 'Fixture contains a non-loopback endpoint')
            if key in ('receipt', 'shareReceipt'):
                inert = (key == 'receipt' and isinstance(item, dict) and type(item.get('schemaVersion')) is int
                         and item == INERT_RECEIPT and terminal
                         and value.get('uploadStatus') in ('failed', 'local_only', 'cancelled', 'skipped', 'conflict')
                         and value.get('remoteStatus', 'not_requested') == 'not_requested'
                         and value.get('shareStatus', 'not_requested') == 'not_requested')
                require(inert or isolated_receipt(item), 'Fixture receipt is neither inert nonpublished state nor a complete isolated receipt')
            fixture_network_state(item, terminal)
    elif isinstance(value, str) and '://' in value:
        require(loopback_url(value), 'Fixture contains a non-loopback persisted URL')


def validate_fixture_database(con):
    allowed = {'history', 'operations', 'item_state', 'deletions', 'deletion_targets', 'cleared_history', 'sharing_operations'}
    for (table,) in con.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
        require(table in allowed, 'Unknown fixture table; persisted state has not been reviewed')
        if table in ('deletions', 'deletion_targets', 'sharing_operations'):
            require(not con.execute('SELECT 1 FROM ' + quote(table) + ' LIMIT 1').fetchone(),
                    'Fixture contains deletion/sharing journals; use a fresh isolated seed')
        columns = {r[1] for r in con.execute('PRAGMA table_info(' + quote(table) + ')')}
        if 'data' in columns:
            if table == 'history':
                for data, state in con.execute("SELECT h.data,json_extract(o.data,'$.state') FROM history h LEFT JOIN operations o ON o.id=h.operation_id"):
                    fixture_network_state(json.loads(data), state in TERMINAL_OPERATIONS)
            else:
                for (data,) in con.execute('SELECT data FROM ' + quote(table)):
                    fixture_network_state(json.loads(data))


@contextmanager
def fixture_data_lock(db):
    """Hold the application's own resident.lock inode through commit.

    Qt QLockFile uses O_EXCL plus flock on Unix, including when removing a stale
    file. Refuse every preexisting lock; never break one or remove a foreign
    inode. Our native flock also prevents a startup racing the initial checks.
    """
    db = Path(db)
    path = db.parent / 'resident.lock'
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    identity = os.fstat(fd)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fields = (str(os.getpid()), Path(os.readlink('/proc/self/exe')).name, socket.gethostname(),
                  Path('/etc/machine-id').read_text().strip(), Path('/proc/sys/kernel/random/boot_id').read_text().strip())
        os.write(fd, ('\n'.join(fields) + '\n').encode()); os.fsync(fd)
        before = db.lstat()
        yield
        after = db.lstat()
        require((before.st_dev, before.st_ino) == (after.st_dev, after.st_ino), 'Fixture database inode changed while locked')
    finally:
        try:
            current = path.lstat()
            if (current.st_dev, current.st_ino) == (identity.st_dev, identity.st_ino): path.unlink()
        finally:
            os.close(fd)


def grow_database(db, total, stopped_check=lambda: None):
    """Transactional cloning of terminal fixture operations, not real uploads.

    Preserve seed payload/preview paths and statuses; each copied journal contains
    only the matching item. New identities/timestamps are consistent in all three
    tables. Original multi-item or unrelated cleared journals are not rewritten.
    """
    require(1 <= total <= 100_000, '--to must be 1..100000')
    with closing(sqlite3.connect(db)) as con, con:
        con.row_factory = sqlite3.Row
        con.execute('BEGIN IMMEDIATE')
        stopped_check()
        validate_fixture_database(con)
        version = con.execute('PRAGMA user_version').fetchone()[0]
        require(version in (5, 6, 7), 'Only schema 5..7 fixture databases are supported')
        rows = [dict(r) for r in con.execute('SELECT * FROM history')]
        require(rows and len(rows) <= total, 'Need nonempty seed history no larger than requested total')
        require(len({r['operation_id'] for r in rows}) == len(rows),
                'Seed must have independent operations; retire the old five-operation expanded fixture')
        operations = {r['id']: dict(r) for r in con.execute('SELECT * FROM operations')}
        states = {r['id']: dict(r) for r in con.execute('SELECT * FROM item_state')}
        templates = []
        for row in rows:
            require(row['operation_id'] in operations and row['id'] in states, 'Seed is missing operation/item-state journals')
            journal = json.loads(operations[row['operation_id']]['data'])
            require(journal.get('state') in ('completed', 'partial', 'failed', 'cancelled', 'interrupted'), 'Active operation cannot seed acceptance history')
            items = [i for i in journal.get('items', []) if i.get('artifact', {}).get('id') == row['item_id']]
            require(len(items) == 1, 'Seed history item must resolve to exactly one journal item')
            templates.append((row, journal, items[0]))
        earliest = min(datetime.fromisoformat(r['created'].replace('Z', '+00:00')) for r in rows)
        for index in range(total - len(rows)):
            row, journal, item = templates[index % len(templates)]
            new_item, new_op = str(uuid.uuid4()), str(uuid.uuid4())
            created = (earliest - timedelta(seconds=index + 1)).isoformat(timespec='milliseconds').replace('+00:00', 'Z')
            substitutions = {row['operation_id']: new_op, row['id']: new_item}
            if row['item_id'] != row['id']:
                substitutions[row['item_id']] = new_item
            # Identity fields and embedded references are replaced, while file
            # paths keep pointing to the small retained synthetic seed payloads.
            def clone(value):
                result = remap(value, substitutions)
                def restore_paths(old, new):
                    if isinstance(old, dict):
                        for k in old:
                            if k in ('path', 'previewPath'):
                                new[k] = old[k]
                            else:
                                restore_paths(old[k], new[k])
                    elif isinstance(old, list):
                        for a, b in zip(old, new):
                            restore_paths(a, b)
                restore_paths(value, result)
                return result
            record = clone(json.loads(row['data']))
            record.update(id=new_item, itemId=new_item, operationId=new_op, created=created)
            copied = dict(row, id=new_item, item_id=new_item, operation_id=new_op, created=created, data=json.dumps(record).encode())
            operation = clone(journal)
            operation.update(id=new_op, created=created, items=[clone(item)])
            state = dict(states[row['id']])
            state.update(id=new_item, operation_id=new_op, data=json.dumps(clone(json.loads(state['data']))).encode())
            insert(con, 'operations', dict(operations[row['operation_id']], id=new_op, data=json.dumps(operation).encode()))
            insert(con, 'history', copied)
            insert(con, 'item_state', state)
        stopped_check()
        require(con.execute('PRAGMA integrity_check').fetchone()[0] == 'ok', 'Cloned fixture failed SQLite integrity')
        shape = con.execute('SELECT count(*),count(DISTINCT operation_id),count(DISTINCT item_id) FROM history').fetchone()
        require(shape[0] == total, 'History count mismatch')
        require(shape[1] == total and shape[2] == total, 'History operations/items are not independent')
        return {'seedRows': len(rows), 'historyRows': shape[0], 'independentOperations': shape[1], 'independentItems': shape[2],
                'operationJournals': con.execute('SELECT count(*) FROM operations').fetchone()[0],
                'itemStates': con.execute('SELECT count(*) FROM item_state').fetchone()[0], 'userVersion': version,
                'syntheticClones': True, 'realUploadsPerformed': 0, 'integrity': 'ok'}


def grow(args):
    root = private(Path(args.fixture_root).resolve(), directory=True)
    db = private(args.db)
    require(db.resolve() == root / 'data/putgd/history.sqlite', 'Database must be the selected fixture data/putgd/history.sqlite')
    private(db.parent, directory=True)
    def stopped():
        require(not resident_processes(args.runtime, db.parent), 'Fixture data or runtime still has an associated resident/helper')
        require(not socket_path(args.runtime).exists(), 'Fixture socket still exists; verify stopped ownership before retrying')
    stopped()
    # Never mutate the source golden or an unlabelled default profile.
    require((root / 'config/putgd/config.json').is_file(), 'Fixture must include config/putgd/config.json')
    doc = json.loads((root / 'config/putgd/config.json').read_text())
    fixture_network_state(doc)
    with fixture_data_lock(db):
        stopped()
        return grow_database(db, args.to, stopped)


def login_input(path):
    if path is None:
        return {'available': False, 'reason': 'No protected login input supplied; inspect bootstrap metadata before requesting one'}
    require(Path(path).name != 'credentials.txt', 'credentials.txt is not a desktop login source')
    try:
        private(path)
    except (OSError, Refused):
        return {'available': False, 'reason': 'Protected login input absent or not an owned private regular file'}
    return {'available': True, 'valueRead': False}


def write_receipt(stream, result):
    if stream is not None:
        stream.seek(0)
        json.dump(result, stream, indent=2); stream.write('\n'); stream.truncate()
        stream.flush(); os.fsync(stream.fileno())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='New private JSON receipt; refuses to overwrite')
    parser.add_argument('--session', help='Expected active logind session ID')
    subs = parser.add_subparsers(dest='cmd', required=True)
    subs.add_parser('session')
    p = subs.add_parser('tray'); p.add_argument('action', choices=('inspect', 'menu', 'activate', 'quit'))
    p.add_argument('--pid', type=int, required=True); p.add_argument('--exe', required=True)
    p = subs.add_parser('plasma-registration'); p.add_argument('--repair', action='store_true'); p.add_argument('--probe', required=True)
    p = subs.add_parser('login-prepare'); p.add_argument('--pid', type=int, required=True); p.add_argument('--exe', required=True)
    p.add_argument('--cli', default='/usr/bin/putgd-cli'); p.add_argument('--runtime', required=True)
    p = subs.add_parser('login-check', help='Check post-login presence/absence; does not prove autostart',
                        description='Check a new graphical session and post-login resident state; launch origin and desktop autostart are not proved')
    p.add_argument('--before', type=Path, required=True); p.add_argument('--runtime', required=True)
    p.add_argument('--expected', choices=('present', 'absent'), default='present')
    p = subs.add_parser('snapshot'); p.add_argument('--runtime', required=True); p.add_argument('--cli', default='/usr/bin/putgd-cli'); p.add_argument('--db')
    p = subs.add_parser('grow'); p.add_argument('--fixture-root', required=True); p.add_argument('--db', required=True)
    p.add_argument('--runtime', required=True); p.add_argument('--to', type=int, default=10000)
    p = subs.add_parser('keychain'); p.add_argument('phase', choices=('status', 'store', 'check', 'cleanup'))
    p.add_argument('--unlock-lab', action='store_true', help='Request visible unlock of the existing approved blank lab collection only')
    p.add_argument('--before', type=Path); p.add_argument('--login-input'); p.add_argument('--credentials-mode', choices=('keychain', 'session', 'reference-only'), default='keychain')
    args = parser.parse_args(argv)
    # A directly executed script and its imported keychain helper must share
    # one refusal type (never print arbitrary backend exception text).
    sys.modules.setdefault('native_acceptance', sys.modules[__name__])
    os.umask(0o077)
    result = {'driverSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'host': socket.gethostname(),
              'at': datetime.now(timezone.utc).isoformat(), 'command': args.cmd}
    stream = None
    # Reserve and validate before session discovery (which can activate D-Bus
    # services), database locking, ui.quit, or any keychain write/unlock.
    try:
        mutating = (args.cmd in ('grow', 'login-prepare') or (args.cmd == 'tray' and args.action in ('activate', 'quit'))
                    or (args.cmd == 'plasma-registration' and args.repair)
                    or (args.cmd == 'keychain' and args.phase != 'status'))
        require(not mutating or args.output is not None, 'Mutating commands require a new private --output receipt')
        if args.output:
            private(args.output.parent, directory=True)
            fd = os.open(args.output, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            stream = os.fdopen(fd, 'w+')
            write_receipt(stream, dict(result, ok=False, state='reserved-before-action'))
    except (OSError, Refused) as error:
        if stream is not None: stream.close()
        result.update(ok=False, error=str(error) if isinstance(error, Refused) else type(error).__name__)
        print(json.dumps(result, indent=2))
        return 2
    try:
        if hasattr(args, 'runtime'):
            result['socket'] = {'path': str(socket_path(args.runtime)), 'utf8Bytes': len(str(socket_path(args.runtime)).encode())}
        if args.cmd == 'grow':
            result['result'] = grow(args)
        else:
            facts, env = session(args.session)
            result['session'] = facts
            if args.cmd == 'session': result['result'] = facts
            elif args.cmd == 'tray': result['result'] = tray(args, env)
            elif args.cmd == 'plasma-registration': result['result'] = plasma_registration(env, args.repair, args.probe)
            elif args.cmd == 'login-prepare': result['result'] = prepare_login(args, facts, env)
            elif args.cmd == 'login-check':
                before = json.loads(private(args.before).read_text())
                require(before.get('ok') and before.get('host') == socket.gethostname(), 'Need a successful preparation receipt from this host')
                require(before.get('result', {}).get('runtime') == str(Path(args.runtime).resolve()), 'Login receipt targets another runtime')
                result['result'] = check_login(before['result'], facts, resident_processes(args.runtime), args.expected)
            elif args.cmd == 'snapshot': result['result'] = snapshot(args, facts, env)
            elif args.cmd == 'keychain':
                from native_keychain import lifecycle
                before = json.loads(private(args.before).read_text()) if args.before else None
                require(before is None or before.get('host') == socket.gethostname(), 'Keychain receipt belongs to another host')
                recovery = before.get('recovery') if before and args.phase == 'cleanup' else None
                require(before is None or (before.get('ok') and isinstance(before.get('result'), dict)) or isinstance(recovery, dict),
                        'Prior keychain receipt has no successful store or exact recovery reference')
                def durable_recovery(value):
                    result['recovery'] = value
                    write_receipt(stream, dict(result, ok=False, state='keychain-reference-reserved'))
                result['result'] = lifecycle(args.phase, env, recovery or (before['result'] if before else None), args.unlock_lab, durable_recovery)
                result['result'].update({'protectedLoginInput': login_input(args.login_input), 'productCredentialMode': args.credentials_mode,
                                         'productKeychainRoundtripProven': False})
                if args.credentials_mode != 'keychain':
                    result['result']['productEvidence'] = 'session credentials/reference-only; no product keychain lifecycle proof'
        result['ok'] = True
    except Exception as error:
        result.update(ok=False, error=str(error) if isinstance(error, Refused) else type(error).__name__)
        if isinstance(error, Refused) and error.recovery is not None: result['recovery'] = error.recovery
    try:
        write_receipt(stream, result)
    except OSError:
        result.update(ok=False, receiptError='Cannot finalize reserved receipt; retain this stdout including recovery reference')
    finally:
        if stream is not None: stream.close()
    print(json.dumps(result, indent=2))
    return 0 if result['ok'] else 2


if __name__ == '__main__':
    sys.exit(main())
