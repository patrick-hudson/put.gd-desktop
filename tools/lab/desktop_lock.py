#!/usr/bin/env python3
"""Exclusive locks for the builders' physical desktops, Proton sessions and quiet leases (stdlib only).

Each builder's GNOME or Plasma desktop serves one session at a time. Its lock
lives on that builder, in ~/putgd-lab/locks/, so every session that can reach
the desktop over pinned SSH sees the same lock, whatever its app, worktree or
session family. The builder's clock decides expiry and staleness. The rules are
in docs/lab/remote-builders.md, "Sharing the lab between concurrent sessions".

`--resource proton` locks the builder's put.gd Proton session instead: one user
at a time (the lab profile's app, a VM borrowing it through
tools/lab/proton_lab.py lend, or a refresh), because every refresh rotates its
token. It is independent of the desktop lock and has no cleanup inventory.

A GNOME builder's desktop is locked only while its tray host is serving: the
AppIndicator extension enabled and active, and a StatusNotifierWatcher with a
registered host on the desktop session bus (tools/lab/gnome_tray_host.py, the
check the nightly's Fedora GNOME guest runs). A test of the no-tray path on
purpose passes `--no-tray-host REASON`, which the lock's purpose records.

`--resource quiet` is the builder's quiet lease: "don't load this host's CPU".
A timing run takes it when it is ready to start, never booked ahead, heartbeats
it and releases it the moment the timing ends. Acquire records the host's load.
The lab's build, replay and benchmark helpers call refuse_while_quiet() and
don't start while another session holds it (`guard` checks it from a shell).
It is independent of the desktop lock: a timing run may need both.

  desktop_lock.py status [HOST ...] [--check] [--resource R]
  desktop_lock.py acquire HOST --owner NAME --session ID --purpose TEXT --minutes N [--resource R]
                           [--no-tray-host REASON]
  desktop_lock.py heartbeat HOST --session ID [--blocked REASON] [--resource R]
  desktop_lock.py extend HOST --session ID --minutes N [--resource R]
  desktop_lock.py release HOST --session ID [--verified-clean] [--not-mine ID ...] [--resource R]
  desktop_lock.py break HOST --session ID --reason TEXT [--resource R]
  desktop_lock.py guard [HOST] [--session ID]   exit 1 while another session holds HOST's quiet lease
                                                (no HOST: this machine's own lock directory)
  desktop_lock.py held [HOST] [--session ID] [--resource R]
                                                exit 0 only while session ID holds HOST's lock and it is
                                                current (no HOST: this machine's own lock directory)
  desktop_lock.py run HOST --owner NAME --session ID --purpose TEXT --minutes N [--resource desktop|quiet]
                       [--no-tray-host REASON] [--release] -- CMD ...
                                                take the lock (or confirm session ID holds it), then run
                                                CMD on HOST, renewing the lock while it runs

`run` is the safe form of "acquire, then work": CMD never starts unless the lock
is ours. CMD runs on the builder over the same pinned BatchMode SSH, its words
quoted as given (a shell expression goes in `sh -c '...'`), with
$PUTGD_LAB_SESSION set to ID, so lab helpers' held and quiet checks know it.
The lock is renewed every 5 minutes, or every third of the lease if that is
shorter. A refused renewal stops CMD's SSH, and so does one that can't reach
the host when the lease would lapse before the next try.
The lock stays held after CMD unless --release is given (which releases it as
`release --verified-clean` would, still refusing if CMD left something new).
Lab helpers that install packages or drive a desktop call refuse_unless_held()
before they change anything.

The script sends itself to the builder (`ssh ... python3 -`) and runs the lock
operation there. Exit codes: 0 done, 1 refused, 2 usage, 3 host unreachable.
"""
import argparse
import concurrent.futures
import contextlib
import datetime
import fcntl
import json
import os
import re
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import time

HOSTS = {
    'f44': ('192.168.68.70', 'Fedora 44 GNOME Wayland'),
    'd69': ('192.168.68.69', 'Debian 13 Plasma Wayland'),
    'k72': ('192.168.68.72', 'Kubuntu 26.04 Plasma Wayland'),
    'm74': ('192.168.68.74', 'Linux Mint 22.3 Cinnamon X11'),
    'aurora-amd': ('192.168.68.71', 'Aurora Fedora 45 Plasma Wayland'),
}
GNOME_HOSTS = frozenset(host for host, (_, desktop) in HOSTS.items() if 'GNOME' in desktop)
USER = 'patrick'
IDENTITY = Path.home() / '.ssh/awskey_110724'
PINS = Path.home() / 'work/put.gd/work/remote-builders'  # <host>/known_hosts
LOCK_DIR = '~/putgd-lab/locks'
RECEIPT_NAME = re.compile(r'(\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z)-(?:(?:proton|quiet)-)?(?:release|break)(?:-([1-9][0-9]?))?\.json')  # receipt()
RESOURCES = {'desktop': 'desktop', 'proton': 'Proton session', 'quiet': 'quiet lease'}  # lock file <name>.lock
SESSION_VARIABLE = 'PUTGD_LAB_SESSION'  # the session a lab helper runs for; its own quiet lease doesn't stop it
RENEW_SECONDS = 300  # how often `run` renews its lock while CMD runs (at most); well inside STALE_AFTER
monotonic = time.monotonic  # `run`'s clock for its own lease deadline
MAX_MINUTES = 45
STALE_AFTER = datetime.timedelta(minutes=15)
FIELDS = ('owner', 'session', 'host', 'purpose', 'acquired', 'expires', 'heartbeat')
PROCESS_PATTERN = ('putgd|host_input|videotestsrc|cover_player'
                   '|Xvfb .*-auth /tmp/pg[^/ ]*/xvfb-run\\.[^/ ]+/Xauthority( |$)')
ALWAYS_REPORTED = ('host_input', 'videotestsrc')  # input agents and test patterns, with or without a display
# The magenta test cover (or its surface-driver copy) run as a script, with or without a display (#1925);
# not a command that only names it, such as a unittest run of test_cover_player.py.
COVER_PLAYER = re.compile(r'(?:^|[ /])cover_player\.py(?: |$)')
# A test X server whose xvfb-run auth file sits in a lab tool's private /tmp/pg* TMPDIR (stress_suite's
# /tmp/pgst.*, for one). A user's own xvfb-run keeps it in /tmp/xvfb-run.*, which doesn't match.
LAB_XVFB = re.compile(r'(?:\S*/)?Xvfb .*-auth /tmp/pg[^/ ]*/xvfb-run\.[^/ ]+/Xauthority(?: |$)')
# /tmp on the builders is a tmpfs, so what is left there takes RAM until a reboot: scratch left by
# campaigns starved the nightly guests on k72 and f44 for two nights (#1978). Each top-level /tmp
# entry of ours this large is an item; all of ours together this large gets a note.
TMP_ROOT = '/tmp'
TMP_ITEM_BYTES = 256 * 2**20
TMP_NOTE_BYTES = 2**30
TMP_SCAN_SECONDS = 10
# The builder's own standing services, as systemd user units: what runs in their cgroups is host
# activity, never something a session left. d69's package signer runs every 10 minutes from
# putgd-signer.timer; its process inherits the user manager's DISPLAY and names putgd (#2015).
HOST_SERVICES = ('putgd-signer.service',)
LEFTOVER_PATHS = ('.local/share/applications/*putgd*.desktop',
                  '.local/share/gnome-shell/extensions/putgd@put.gd',
                  '.local/share/putgd', '.cache/putgd', '.config/putgd')


class LockError(Exception):
    """A refused operation. The message says why; details travel back as JSON."""

    def __init__(self, message, **details):
        super().__init__(message)
        self.details = details


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)


def stamp(moment):
    return moment.strftime('%Y-%m-%dT%H:%M:%SZ')


def parse_time(text):
    return datetime.datetime.strptime(text, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)


def one_line(name, value, limit=400):
    value = (value or '').strip()
    if not value or not value.isprintable() or len(value) > limit:
        raise LockError(f'--{name} must be one printable line of 1-{limit} characters')
    return value


def checked_minutes(minutes):
    if type(minutes) is not int or not 1 <= minutes <= MAX_MINUTES:
        raise LockError(f'--minutes must be 1-{MAX_MINUTES}; extend the lock before it expires instead')
    return minutes


def parse_lock(text):
    record = {}
    for line in text.splitlines():
        key, separator, value = line.partition(': ')
        if separator:
            record[key.strip()] = value.strip()
    missing = [field for field in FIELDS if not record.get(field)]
    if missing:
        raise ValueError('lock lacks ' + ', '.join(missing))
    for field in ('acquired', 'expires', 'heartbeat'):
        parse_time(record[field])
    return record


def render_lock(record):
    keys = FIELDS + tuple(key for key in ('resource', 'load', 'blocked') if record.get(key))
    return ''.join(f'{key}: {record[key]}\n' for key in keys)


def assess(record, now):
    """Expired once `expires` has passed; stale once the heartbeat is over 15 minutes old."""
    expires, heartbeat = parse_time(record['expires']), parse_time(record['heartbeat'])
    expired, stale = now >= expires, now - heartbeat > STALE_AFTER
    blocked = record.get('blocked') or None
    return {'state': 'expired' if expired else 'stale' if stale else 'blocked' if blocked else 'held',
            'breakable': expired or stale, 'blocked': blocked,
            'expiresInSeconds': int((expires - now).total_seconds()),
            'heartbeatAgeSeconds': int((now - heartbeat).total_seconds())}


def package_source(item):
    return item['key'].split(':')[1]  # package:deb:<version> -> deb


def compare(baseline, current, not_mine=()):
    """Split the host's items into ones present at acquire and ones that appeared since. A package
    whose source (deb, rpm) has no package at all any more is listed as removed."""
    known = {item['key'] for item in baseline}
    sources = {package_source(item) for item in current if item['kind'] == 'package'}
    new = [item for item in current if item['key'] not in known]
    claimed = set(not_mine)
    excused = {item['key'] for item in new if claimed & {item['id'], item['key']}}
    return {'left': [item for item in new if item['key'] not in excused],
            'notMine': [item for item in new if item['key'] in excused],
            'preexisting': [item for item in current if item['key'] in known],
            'removed': [item for item in baseline if item['kind'] == 'package' and package_source(item) not in sources],
            'unmatchedNotMine': sorted(claimed - {identity for item in new for identity in (item['id'], item['key'])})}


def execute(argv):
    """Return (exit code, stdout), or (None, '') when the command is not installed."""
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
    except FileNotFoundError:
        return None, ''
    except subprocess.TimeoutExpired:
        raise LockError('Cleanup check timed out: ' + ' '.join(argv)) from None
    return result.returncode, result.stdout


def process_start(pid):
    try:
        return Path(f'/proc/{pid}/stat').read_text().rpartition(')')[2].split()[19]
    except (OSError, IndexError):
        return ''


def process_environment(pid):
    """Return the process's environment names, None if unreadable, or False once it has exited."""
    try:
        data = Path(f'/proc/{pid}/environ').read_bytes()
    except (FileNotFoundError, ProcessLookupError):
        return False
    except OSError:
        return None
    return {entry.partition(b'=')[0].decode(errors='replace') for entry in data.split(b'\0') if entry}


def process_parent(pid):
    """Return PID's parent PID, or None when it can't be read."""
    try:
        return Path(f'/proc/{pid}/stat').read_bytes().rpartition(b')')[2].split()[1].decode()
    except (OSError, IndexError):
        return None


def process_orphaned(pid):
    """True once the process that started PID has exited: PID now belongs to init or to a
    systemd subreaper. False while its parent runs, or when PID can't be read."""
    parent = process_parent(pid)
    try:
        return parent is not None and (parent == '1' or Path(f'/proc/{parent}/comm').read_text().strip() == 'systemd')
    except OSError:
        return False


def process_cgroup(pid):
    """Return PID's cgroup v2 path, or None when it can't be read (or the process has exited)."""
    try:
        lines = Path(f'/proc/{pid}/cgroup').read_text().splitlines()
    except OSError:
        return None
    return next((line[3:] for line in lines if line.startswith('0::')), None)


def host_service(path, uid=None):
    """The HOST_SERVICES unit whose cgroup PATH is, when it is one of this user's own systemd
    user units (under user@UID.service, in any slice), else None. A unit of that name run in
    a session scope or another user's manager is not the host's service."""
    uid = os.getuid() if uid is None else uid
    match = re.fullmatch(rf'/user\.slice/user-{uid}\.slice/user@{uid}\.service/(?:[^/]+\.slice/)*([^/]+)', path or '')
    return match.group(1) if match and match.group(1) in HOST_SERVICES else None


def started_environment(pid, environment=process_environment, parent=process_parent, depth=8):
    """The environment names of the nearest process up PID's parent chain whose environment can
    be read, None when none can (or that process has exited) before init. A confined helper such as
    putgd-ocr makes itself non-dumpable, so its own environment is root's to read; whoever
    started it decides whether it is another session's SSH work (#1938)."""
    for _ in range(depth):
        pid = parent(pid)
        if pid is None or pid in ('0', '1'):
            return None
        names = environment(pid)
        if names is not None:
            return None if names is False else names
    return None


def size_text(count):
    """A byte count as KB, MB or GB (1024 based)."""
    for unit, scale in (('GB', 2**30), ('MB', 2**20), ('KB', 2**10)):
        if count >= scale:
            return f'{count / scale:.1f} {unit}'
    return f'{count} bytes'


def tmpfs_mounted(root, mounts='/proc/self/mounts'):
    """True when ROOT is a mount point whose (topmost) file system is a tmpfs."""
    kind = None
    try:
        for fields in (line.split() for line in Path(mounts).read_text().splitlines()):
            if len(fields) >= 3 and fields[1] == root:
                kind = fields[2]
    except OSError:
        return False
    return kind == 'tmpfs'


def tmp_usage(root=TMP_ROOT, uid=None, mounts='/proc/self/mounts', seconds=TMP_SCAN_SECONDS, clock=time.monotonic):
    """None when ROOT is not a tmpfs. Otherwise the bytes our files take there ('total'), our
    top-level entries as (path, bytes of our files in it, inode) ('entries'), and whether the walk
    finished inside SECONDS ('complete'). Symbolic links and other file systems are not followed;
    what can't be read is skipped."""
    if not tmpfs_mounted(root, mounts):
        return None
    uid = os.getuid() if uid is None else uid
    deadline = clock() + seconds
    try:
        device, top = os.stat(root).st_dev, os.scandir(root)
    except OSError:
        return {'total': 0, 'entries': [], 'complete': False}
    try:
        with top:
            total, entries, complete = _tmp_entries(top, uid, device, deadline, clock)
    except OSError:  # listing /tmp itself failed part way: report nothing rather than stop the check
        return {'total': 0, 'entries': [], 'complete': False}
    return {'total': total, 'entries': entries, 'complete': complete}


def _tmp_entries(top, uid, device, deadline, clock):
    total, entries, complete = 0, [], True
    for entry in top:  # streamed: the budget holds while /tmp itself is listed
        if clock() > deadline:
            complete = False
            break
        try:
            info = entry.stat(follow_symlinks=False)
        except OSError:
            continue
        size, pending = 0, [entry.path] if entry.is_dir(follow_symlinks=False) and info.st_dev == device else []
        if not pending and info.st_uid == uid and not entry.is_symlink():
            size = info.st_blocks * 512
        while pending and complete:
            try:
                children = os.scandir(pending.pop())
            except OSError:
                continue
            with children:
                for child in children:  # the deadline is checked per entry: one directory can be huge
                    if clock() > deadline:
                        complete = False
                        break
                    try:
                        status = child.stat(follow_symlinks=False)
                    except OSError:
                        continue
                    if child.is_dir(follow_symlinks=False):
                        if status.st_dev == device:
                            pending.append(child.path)
                    elif status.st_uid == uid and not child.is_symlink():
                        size += status.st_blocks * 512
        total += size
        if info.st_uid == uid:
            entries.append((entry.path, size, info.st_ino))
        if not complete:
            break
    return total, entries, complete


def mem_available():
    """MemAvailable in bytes, or None when /proc/meminfo can't be read."""
    try:
        return next(int(line.split()[1]) * 1024 for line in Path('/proc/meminfo').read_text().splitlines()
                    if line.startswith('MemAvailable:'))
    except (OSError, StopIteration, ValueError, IndexError):
        return None


def collect_inventory(home, run=execute, own=None, environment=process_environment, orphaned=process_orphaned,
                      parent=process_parent, tmp=tmp_usage, memory=mem_available, cgroup=process_cgroup):
    """List what a desktop session can leave on this host: matching processes in the desktop
    session (or that can't be told apart, unless the nearest readable parent shows an SSH
    login), input agents, test patterns and cover players, orphaned lab test X servers, Flatpak instances, user desktop entries, extension copies and
    data, a putgd package, and our large entries in a RAM-backed /tmp. Other matching processes
    without DISPLAY or WAYLAND_DISPLAY are shared SSH work (builds, syncs, collectors), and so is a
    lab test X server whose test still runs. Processes in a HOST_SERVICES unit's cgroup are the
    host's standing activity and are only noted."""
    own = {os.getpid(), os.getppid()} if own is None else own
    items, notes = [], []

    def add(kind, ident, key, detail):
        items.append({'kind': kind, 'id': ident, 'key': key,
                      'detail': detail if len(detail) <= 160 else detail[:157] + '...'})

    code, output = run(['pgrep', '-u', str(os.getuid()), '-af', PROCESS_PATTERN])
    if code is None or code > 1:
        raise LockError('Cleanup check could not run pgrep on this host')
    shared, services = [], {}
    for line in output.splitlines():
        pid, _, command = line.strip().partition(' ')
        if not pid.isdigit() or int(pid) in own:
            continue
        service = host_service(cgroup(pid))
        if service:
            services.setdefault(service, []).append(pid)
            continue
        if LAB_XVFB.match(command):
            if not orphaned(pid):
                shared.append(pid)
                continue
            add('process', pid, f'process:{pid}:{process_start(pid)}', command)
            continue
        names = environment(pid)
        if names is False:
            continue
        if names is None:
            # Exempt only with positive evidence of SSH work: an adoptive parent (a subreaper)
            # without display variables is not the desktop process that started it.
            started = started_environment(pid, environment, parent)
            names = started if started and started & {'SSH_CONNECTION', 'SSH_CLIENT'} else None
        if names is not None and not names & {'DISPLAY', 'WAYLAND_DISPLAY'} and \
                not any(word in command for word in ALWAYS_REPORTED) and not COVER_PLAYER.search(command):
            shared.append(pid)
            continue
        add('process', pid, f'process:{pid}:{process_start(pid)}', command)
    if shared:
        notes.append(f'{len(shared)} process(es) outside the desktop session (no DISPLAY or '
                     'WAYLAND_DISPLAY; shared SSH work): ' + ' '.join(shared[:8]) +
                     (' ...' if len(shared) > 8 else ''))
    for service, pids in services.items():
        notes.append(f'{len(pids)} process(es) of the host service {service} (standing host activity): ' +
                     ' '.join(pids[:8]) + (' ...' if len(pids) > 8 else ''))
    code, output = run(['flatpak', 'ps', '--columns=instance,pid,application'])
    if code is None:
        notes.append('flatpak is not installed')
    for fields in (line.split() for line in output.splitlines()):
        if len(fields) >= 2 and fields[0].isdigit():  # not a header
            add('flatpak', fields[0], 'flatpak:' + fields[0], 'PID ' + ' '.join(fields[1:]))
    for pattern in LEFTOVER_PATHS:
        for path in sorted(home.glob(pattern)):
            add('file', str(path), f'file:{path}', '')
    packages = len(items)
    code, output = run(['rpm', '-q', '--qf', '%{VERSION}-%{RELEASE}', 'putgd'])
    if code == 0:
        add('package', 'putgd', 'package:rpm:' + output.strip(), 'rpm putgd ' + output.strip())
    code, output = run(['dpkg-query', '-W', '-f', '${db:Status-Abbrev} ${Version}', 'putgd'])
    status, _, version = output.partition(' ')
    if code == 0 and status[1:2] not in ('', 'n', 'c'):
        add('package', 'putgd', 'package:deb:' + version.strip(), 'deb putgd ' + version.strip())
    if len(items) == packages:
        notes.append('no putgd package installed' + (f' (config files of deb {version.strip()} only)'
                                                      if code == 0 and status[1:2] == 'c' else ''))
    usage = tmp()
    if usage:
        for path, size, inode in usage['entries']:
            if size >= TMP_ITEM_BYTES:
                add('tmp', path, f'tmp:{path}:{inode}', size_text(size) + ' in RAM-backed /tmp')
        if usage['total'] >= TMP_NOTE_BYTES:
            available = memory()
            notes.append(f'/tmp is a tmpfs (RAM): your files there take {size_text(usage["total"])}'
                         + (f', with {size_text(available)} of memory available' if available is not None else '')
                         + '. Keep scratch in ~/work/tmp (tools/lab/scratch.py) and remove what your runs are done with')
        if not usage['complete']:
            notes.append(f'the /tmp size check stopped after {TMP_SCAN_SECONDS} s; its sizes are lower bounds')
    return items, notes


def probe_tray_host(source, timeout=8):
    """Run the tray host probe (gnome_tray_host.probe_source) on the desktop session bus."""
    environment = dict(os.environ)
    environment.setdefault('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}')
    environment.setdefault('DBUS_SESSION_BUS_ADDRESS', f'unix:path={environment["XDG_RUNTIME_DIR"]}/bus')
    try:
        result = subprocess.run(['python3', '-c', source], capture_output=True, text=True,
                                timeout=timeout, env=environment)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {'ready': False, 'error': type(error).__name__}
    try:
        report = json.loads(result.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError):
        detail = (result.stderr.strip().splitlines() or [f'exit {result.returncode}, no output'])[-1]
        return {'ready': False, 'error': detail[:200]}
    return dict(report, ready=result.returncode == 0)


class Desktop:
    """One lock (the desktop, or the Proton session) in a builder's lock directory.
    Every change happens under an flock shared by all of the directory's locks."""

    def __init__(self, directory, host, clock=now_utc, inventory=None, resource='desktop', tray_host=None):
        if resource not in RESOURCES:
            raise LockError('Unknown resource ' + str(resource))
        self.directory, self.host, self.clock, self.resource = Path(directory), host, clock, resource
        # Only the desktop has a cleanup inventory; a Proton session leaves nothing on screen.
        self.inventory = inventory or (lambda: collect_inventory(Path.home()) if resource == 'desktop' else ([], []))
        # A GNOME desktop's tray host probe: returns a report whose 'ready' must be true to lock.
        self.tray_host = tray_host if resource == 'desktop' else None
        self.lock = self.directory / f'{resource}.lock'
        self.baseline = self.directory / 'desktop.baseline.json'
        self.history = self.directory / 'history'
        self.name = self.host if resource == 'desktop' else f'{self.host} {RESOURCES[resource]}'

    @contextlib.contextmanager
    def mutex(self):
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        with open(self.directory / '.mutex', 'a') as handle:
            deadline = time.monotonic() + 10
            while True:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() > deadline:
                        raise LockError('Another lock operation is still running on this host; retry') from None
                    time.sleep(.05)
            try:
                yield
            finally:
                fcntl.flock(handle, fcntl.LOCK_UN)

    def state(self, now):
        try:
            text = self.lock.read_text()
        except FileNotFoundError:
            return {'state': 'free', 'breakable': False}
        try:
            record = parse_lock(text)
        except ValueError as error:
            written = datetime.datetime.fromtimestamp(self.lock.stat().st_mtime, datetime.timezone.utc)
            return {'state': 'invalid', 'error': str(error), 'text': text,
                    'breakable': now - written > STALE_AFTER}
        return dict(assess(record, now), lock=record)

    def held_by(self, session, now):
        current = self.state(now)
        if current['state'] == 'free':
            raise LockError(f'{self.name} is not locked')
        if current['state'] == 'invalid':
            raise LockError(f'{self.name} has an unreadable lock ({current["error"]})', **current)
        if current['lock']['session'] != session:
            raise LockError(f'{self.name} is held by {current["lock"]["owner"]} '
                            f'({current["lock"]["session"]}), not {session}', **current)
        return current['lock']

    def held(self, session):
        """The lock's state if `session` holds it and it is current (not expired or stale); else LockError."""
        now = self.clock()
        record = self.held_by(session, now)
        current = dict(assess(record, now), lock=record)
        if current['breakable']:
            raise LockError(f'{self.name} is {current["state"]} under {session}: anyone may break it now. '
                            'Extend it before more work', **current)
        return current

    def replace(self, path, text):
        temporary = path.with_name(f'.{path.name}.{os.getpid()}')
        temporary.write_text(text)
        os.replace(temporary, path)

    def read_baseline(self):
        if self.resource != 'desktop':
            return []
        try:
            return json.loads(self.baseline.read_text())['items']
        except (OSError, ValueError, KeyError):
            return []

    def receipt(self, event, body):
        self.history.mkdir(mode=0o700, exist_ok=True)
        tag = event if self.resource == 'desktop' else f'{self.resource}-{event}'
        name = stamp(self.clock()).replace(':', '-') + '-' + tag
        if self.resource != 'desktop':
            body = dict(body, resource=self.resource)
        for suffix in [''] + [f'-{number}' for number in range(1, 100)]:
            try:
                with open(self.history / (name + suffix + '.json'), 'x') as stream:
                    stream.write(json.dumps(dict(body, event=event, host=self.host,
                                                 at=stamp(self.clock())), indent=2) + '\n')
                return str(self.history / (name + suffix + '.json'))
            except FileExistsError:
                continue
        raise LockError('Could not write a unique receipt name')

    def last_event(self):
        # Only this tool's own receipts, which start with their UTC stamp and so sort by time;
        # other files kept in history (retired scratch notes) must not stand in for the last event.
        # Ordered by stamp, then by write time (receipts are created once), then by the -1..-99 suffix:
        # suffixes count per event, so only write time orders a break and a release of one second.
        def written(path):
            try:
                return path.stat().st_mtime_ns
            except OSError:
                return 0
        named = [(RECEIPT_NAME.fullmatch(path.name), path) for path in self.history.glob('*.json')] \
            if self.history.is_dir() else []
        receipts = [path for match, path in sorted(((m, p) for m, p in named if m),
                                                   key=lambda item: (item[0][1], written(item[1]), int(item[0][2] or 0)))]
        for receipt in reversed(receipts):
            try:
                body = json.loads(receipt.read_text())
            except ValueError:
                if self.resource == 'desktop':
                    return {'receipt': str(receipt)}
                continue
            if body.get('resource', 'desktop') != self.resource:
                continue
            lock = body.get('lock') or {}
            return {'event': body.get('event'), 'at': body.get('at'), 'owner': lock.get('owner'),
                    'by': body.get('releasedBy') or body.get('brokenBy'), 'receipt': str(receipt)}
        return None

    def status(self, check=False):
        result = dict(self.state(self.clock()), last=self.last_event())
        if check:
            items, result['notes'] = self.inventory()
            if result['state'] == 'free':
                result['present'] = items
            else:
                result['check'] = compare(self.read_baseline(), items)
        return result

    def acquire(self, owner, session, purpose, minutes):
        owner, session, purpose = one_line('owner', owner), one_line('session', session), one_line('purpose', purpose)
        minutes = checked_minutes(minutes)
        with self.mutex():
            now = self.clock()
            current = self.state(now)
            if current['state'] != 'free':
                raise LockError(f'{self.name} is already locked' + (
                    '. Do other work and retry acquire later; never sleep holding nothing'
                    if self.resource == 'quiet' else ''), **current)
            # The baseline is taken once the desktop is free, so the last holder's cleanup is in it.
            items, notes = self.inventory()
            # Under the mutex, so a holder releasing just now can't leave the tray down unseen.
            tray = self.tray_host() if self.tray_host else None
            if tray is not None and not tray.get('ready'):
                raise LockError(f'{self.name} has no tray host', trayHost=tray)
            now = self.clock()  # the lease starts after the probe
            record = {'owner': owner, 'session': session, 'host': self.host, 'purpose': purpose,
                      'acquired': stamp(now), 'expires': stamp(now + datetime.timedelta(minutes=minutes)),
                      'heartbeat': stamp(now)}
            if self.resource != 'desktop':
                record['resource'] = self.resource
            if self.resource == 'quiet':
                record['load'], note = host_load()
                notes = notes + ([note] if note else [])
            temporary = self.directory / f'.{self.lock.name}.{os.getpid()}'
            temporary.write_text(render_lock(record))
            try:
                os.link(temporary, self.lock)  # link(2) never replaces an existing lock
            except FileExistsError:
                raise LockError(f'{self.name} was locked by someone else just now', **self.state(now)) from None
            finally:
                temporary.unlink()
            if self.resource == 'desktop':
                self.replace(self.baseline, json.dumps({'acquired': record['acquired'], 'items': items}) + '\n')
        reply = {'lock': record, 'baseline': items, 'notes': notes}
        return dict(reply, trayHost=tray) if tray is not None else reply

    def heartbeat(self, session, blocked=None):
        blocked = one_line('blocked', blocked) if blocked is not None else None
        with self.mutex():
            now = self.clock()
            record = self.held_by(session, now)
            record['heartbeat'] = stamp(now)
            record.pop('blocked', None)
            if blocked:
                record['blocked'] = blocked
            self.replace(self.lock, render_lock(record))
        return dict(assess(record, now), lock=record)

    def extend(self, session, minutes):
        minutes = checked_minutes(minutes)
        with self.mutex():
            now = self.clock()
            record = self.held_by(session, now)
            was_expired = now >= parse_time(record['expires'])
            record['expires'] = stamp(now + datetime.timedelta(minutes=minutes))
            record['heartbeat'] = stamp(now)
            self.replace(self.lock, render_lock(record))
        return dict(assess(record, now), lock=record, wasExpired=was_expired)

    def release(self, session, verified_clean=False, not_mine=(), removed_ok=None, tmp_blocks=True):
        """With tmp_blocks false (the nightly's own lease), a new large /tmp entry is reported but
        doesn't hold the lock: it can be another session's, and the nightly can't move it."""
        items, notes = self.inventory()
        with self.mutex():
            now = self.clock()
            record = self.held_by(session, now)
            # Resolve dynamic ownership after inventory so concurrent spawns in
            # the inventory cannot fall between the proof and cleanup snapshots.
            claimed = not_mine() if callable(not_mine) else not_mine
            check = compare(self.read_baseline(), items, claimed)
            blocking = [item for item in check['left'] if tmp_blocks or item['kind'] != 'tmp']
            if blocking:
                raise LockError(f'Not released: {len(blocking)} item(s) appeared on {self.host} since you '
                                'acquired it. Stop or remove yours (exact PIDs only) and check again; name '
                                "another session's item with --not-mine ID" +
                                ('. Move /tmp scratch you still need to ~/work/tmp (tools/lab/scratch.py): '
                                 '/tmp here is RAM' if any(item['kind'] == 'tmp' for item in blocking) else ''),
                                check=check, notes=notes)
            if check['removed'] and not removed_ok:
                raise LockError(f'Not released: {len(check["removed"])} putgd package(s) installed when you acquired '
                                f'the lock are gone from {self.host}. Reinstall the version listed, or say why it '
                                'stays removed with --removed-ok REASON', check=check, notes=notes)
            if not verified_clean and self.resource == 'proton':
                raise LockError('Confirm that no lent copy is outstanding (every carry-back confirmed, every '
                                'putgd-lab guest entry cleaned up), then rerun with --verified-clean',
                                check=check, notes=notes)
            if not verified_clean and self.resource == 'desktop':
                raise LockError('Nothing new is left on the host. Finish the manual steps (workstation '
                                'forwards, re-login after an extension copy), then rerun with --verified-clean',
                                check=check, notes=notes)
            body = {'lock': record, 'releasedBy': session, 'check': check, 'notes': notes}
            if check['removed']:
                body['removedOk'] = one_line('removed-ok', removed_ok)
            receipt = self.receipt('release', body)
            if self.resource == 'desktop':
                self.baseline.unlink(missing_ok=True)
            self.lock.unlink()
        return {'lock': record, 'check': check, 'notes': notes, 'receipt': receipt}

    def break_lock(self, session, reason):
        session, reason = one_line('session', session), one_line('reason', reason)
        items, notes = self.inventory()
        with self.mutex():
            now = self.clock()
            current = self.state(now)
            if current['state'] == 'free':
                raise LockError(f'{self.name} is not locked')
            if not current['breakable']:
                raise LockError(f'{self.name} has a live lock; only an expired or stale one can be broken. '
                                'Message its holder instead', **current)
            check = compare(self.read_baseline(), items)
            receipt = self.receipt('break', {'lock': current.get('lock'), 'lockText': current.get('text'),
                                             'state': current['state'], 'brokenBy': session, 'reason': reason,
                                             'check': check, 'notes': notes})
            if self.resource == 'desktop':
                self.baseline.unlink(missing_ok=True)
            self.lock.unlink()
        return {'broken': current, 'check': check, 'notes': notes, 'receipt': receipt}


def host_load():
    """Return this host's load averages and CPU count as one line, and a note when the load is above the CPU count."""
    loads, cpus = os.getloadavg(), os.cpu_count() or 1
    text = ' '.join(f'{value:.2f}' for value in loads) + f' ({cpus} CPUs)'
    note = (f'load {loads[0]:.2f} is above the {cpus} CPUs: heavy work is still running here; '
            'find it before you time anything') if loads[0] > cpus else None
    return text, note


def quiet_holder(directory=None, session=None, clock=None):
    """Return why heavy work may not start on this host now, or None.

    Heavy work waits for another session's quiet lease while it is held, blocked or stale (a stale
    lease can be broken; until then its timing run may still be going), and for a recent
    unreadable one. A free or expired lease, or the caller's own (`session`, by default
    $PUTGD_LAB_SESSION), lets it start."""
    session = os.environ.get(SESSION_VARIABLE) if session is None else session
    directory, clock = Path(directory or LOCK_DIR).expanduser(), clock or now_utc
    lease = Desktop(directory, socket.gethostname(), clock, resource='quiet')
    current = lease.state(clock())
    if current['state'] == 'free' or current['state'] == 'expired' or \
            (current['state'] == 'invalid' and current['breakable']):
        return None
    if current['state'] == 'invalid':
        return f'this host\'s quiet lease is unreadable ({current["error"]}) and recent'
    record = current['lock']
    if session and record['session'] == session:
        return None
    expiry = minutes_text(current['expiresInSeconds'])
    return (f'{record["owner"]} ({record["session"]}) holds this host\'s quiet lease for "{record["purpose"]}" '
            f'({current["state"]}, expires in {expiry}). Do other work and retry later; never sleep waiting '
            f'for it. Your own lease: set {SESSION_VARIABLE} to its session')


def refuse_while_quiet(what, directory=None, session=None):
    """The one check the lab's build, replay and benchmark helpers make before they start:
    exit with the reason while another session holds this host's quiet lease."""
    reason = quiet_holder(directory, session)
    if reason:
        raise SystemExit(f'{what} not started: {reason}')


def lock_problem(directory=None, session=None, clock=None, resource='desktop'):
    """Return why this host's lock is not the caller's, or None.

    The caller is `session`, by default $PUTGD_LAB_SESSION (which `run` sets). A host with no lock
    directory is not a shared builder (a VM guest, a sandbox) and has no lock to hold: None."""
    session = os.environ.get(SESSION_VARIABLE, '') if session is None else session
    directory = Path(directory or LOCK_DIR).expanduser()
    if not directory.is_dir():
        return None
    if not session:
        return f'{SESSION_VARIABLE} is not set to the session that holds this host\'s {RESOURCES[resource]} lock'
    try:
        Desktop(directory, socket.gethostname(), clock or now_utc, resource=resource).held(session)
    except LockError as error:
        return str(error)
    return None


def refuse_unless_held(what, directory=None, session=None, resource='desktop'):
    """The check the lab's package-installing and desktop-driving helpers make before they change
    anything on a builder: exit with the reason unless the caller holds this host's lock."""
    reason = lock_problem(directory, session, resource=resource)
    if reason:
        raise SystemExit(f'{what} not started: {reason}. Run it through desktop_lock.py run HOST ... -- CMD')


def held_check_script(session, resource='desktop'):
    """A shell script printing lock_problem() for `session` as JSON on the host it runs on, for a
    workstation driver to send through its own transport to the host it will change."""
    check = f'print(json.dumps({{"problem": lock_problem(session={session!r}, resource={resource!r})}}))'
    return "python3 - <<'PUTGD_LOCK'\n" + Path(__file__).read_text() + '\n' + check + '\nPUTGD_LOCK\n'


def remote(payload):
    """Run one lock operation on this host and print its JSON reply."""
    probe = payload.get('tray_probe')
    desktop = Desktop(Path(payload.get('directory') or LOCK_DIR).expanduser(), payload['host'],
                      resource=payload.get('resource', 'desktop'),
                      tray_host=(lambda: probe_tray_host(probe)) if probe else None)
    action = payload['action']
    try:
        if action == 'status':
            reply = desktop.status(payload.get('check', False))
        elif action == 'acquire':
            reply = desktop.acquire(payload['owner'], payload['session'], payload['purpose'], payload['minutes'])
        elif action == 'heartbeat':
            reply = desktop.heartbeat(payload['session'], payload.get('blocked'))
        elif action == 'extend':
            reply = desktop.extend(payload['session'], payload['minutes'])
        elif action == 'release':
            reply = desktop.release(payload['session'], payload.get('verified_clean', False),
                                    payload.get('not_mine', ()), payload.get('removed_ok'))
        elif action == 'break':
            reply = desktop.break_lock(payload['session'], payload['reason'])
        elif action == 'held':
            reply = desktop.held(payload['session'])
        elif action == 'guard':
            reason = quiet_holder(desktop.directory, payload.get('session', ''), desktop.clock)
            reply = dict(desktop.status(), quiet=reason is None)
            if reason:
                raise LockError(reason, **reply)
        else:
            raise LockError('Unknown action ' + action)
        reply = dict(reply, ok=True)
    except LockError as error:
        reply = dict(error.details, ok=False, message=str(error))
    print(json.dumps(dict(reply, host=desktop.host, action=action, resource=desktop.resource,
                          now=stamp(desktop.clock()))))
    return 0


def ssh_command(host, identity=IDENTITY, pins=PINS, remote_command=('python3', '-')):
    return ['ssh', '-i', str(identity), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=8',
            '-o', 'UserKnownHostsFile=' + str(Path(pins) / host / 'known_hosts'),
            f'{USER}@{HOSTS[host][0]}', *remote_command]


def remote_script(payload):
    return Path(__file__).read_text() + '\nraise SystemExit(remote(json.loads(%r)))\n' % json.dumps(payload)


def call(host, payload, identity=IDENTITY, pins=PINS, run=subprocess.run, command=None, timeout=90):
    """Send this script to HOST over pinned SSH and return its reply."""
    failed = {'ok': False, 'unreachable': True, 'host': host, 'action': payload['action']}
    known_hosts = Path(pins) / host / 'known_hosts'
    if command is None and not known_hosts.is_file():
        return dict(failed, message=f'No pinned host key at {known_hosts}. Recover the verified pin '
                                    '(docs/lab/remote-builders.md); never trust on first use')
    try:
        result = run(command or ssh_command(host, identity, pins), input=remote_script(dict(payload, host=host)),
                     capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return dict(failed, message=f'SSH to {host} timed out')
    try:
        return json.loads(result.stdout.strip().splitlines()[-1])
    except (IndexError, ValueError):
        detail = (result.stderr.strip().splitlines() or ['no output'])[-1]
        return dict(failed, message=f'SSH to {host} failed (exit {result.returncode}): {detail}')


def minutes_text(seconds):
    return f'{round(abs(seconds) / 60)} min'


def describe_items(title, items):
    return [f'  {title}:'] + [f'    {item["kind"]} {item["id"]}' + (f': {item["detail"]}' if item['detail'] else '')
                              for item in items]


DONE = {'guard': 'quiet lease free for heavy work', 'held': 'held by this session', 'acquire': 'locked', 'heartbeat': 'heartbeat recorded', 'extend': 'expiry moved',
        'release': 'released', 'break': 'lock broken'}


def describe(reply):
    """Human-readable lines for one host's reply."""
    host, action = reply['host'], reply['action']
    head = f'{host} ({HOSTS[host][1] if host in HOSTS else "?"})'
    if reply.get('resource', 'desktop') != 'desktop':
        head = f'{host} {RESOURCES.get(reply["resource"], reply["resource"])} ({HOSTS[host][1] if host in HOSTS else "?"})'
    if not reply['ok']:
        lines = [f'{head}: {reply["message"]}']
    else:
        lines = [f'{head}: ' + (reply['state'] if action == 'status' else DONE[action])]
    if reply.get('unreachable'):
        return lines
    broken = reply.get('broken') or {}
    lock, state = reply.get('lock') or broken.get('lock'), broken.get('state') or reply.get('state')
    if lock:
        lines.append(f'  {lock["owner"]} ({lock["session"]}), acquired {lock["acquired"]}')
        lines.append(f'  purpose: {lock["purpose"]}')
    if broken:
        lines.append(f'  the broken lock was {state}')
    if lock and 'expiresInSeconds' in reply:
        expiry = reply['expiresInSeconds']
        lines.append('  ' + (f'expires in {minutes_text(expiry)}' if expiry > 0 else
                             f'expired {minutes_text(expiry)} ago') +
                     f', heartbeat {minutes_text(reply["heartbeatAgeSeconds"])} ago')
    if reply.get('blocked'):
        lines.append('  blocked: ' + reply['blocked'])
    if state == 'invalid':
        lines.append(f'  unreadable lock ({reply.get("error")}); breakable: {reply.get("breakable")}')
    if action == 'status' and reply.get('breakable'):
        lines.append('  Message the holder. With no answer in 10 min, check the host (status --check) '
                     'and break the lock')
    if action == 'acquire' and not reply['ok'] and lock and reply.get('resource') != 'quiet':
        lines.append("  Don't poll the holder or wait idle: tell your coordinator, do other work, "
                     'and try again later')
    if action in ('heartbeat', 'extend') and reply['ok'] and state == 'expired':
        lines.append('  The lock has expired: extend it now or release it')
    if reply.get('wasExpired'):
        lines.append('  The lock had expired before this extension')
    if reply.get('last') and action == 'status':
        last = reply['last']
        lines.append(f'  last: {last.get("event")} of {last.get("owner")}\'s lock by {last.get("by")} '
                     f'at {last.get("at")}')
    if reply.get('baseline'):
        lines += describe_items('already on the host (not yours; leave them and tell your coordinator)',
                                reply['baseline'])
    if reply.get('present'):
        lines += describe_items('on the host now', reply['present'])
    check = reply.get('check')
    if check:
        if check['left']:
            lines += describe_items('appeared since the lock was taken', check['left'])
        if check['notMine']:
            lines += describe_items('named --not-mine', check['notMine'])
        if check.get('removed'):
            lines += describe_items('removed since the lock was taken', check['removed'])
        if check['preexisting'] and action == 'release' and reply['ok']:
            lines.append(f'  {len(check["preexisting"])} item(s) that were there before the lock stay')
        elif check['preexisting']:
            lines += describe_items('already there when the lock was taken', check['preexisting'])
        if check['unmatchedNotMine']:
            lines.append('  --not-mine matched nothing new: ' + ', '.join(check['unmatchedNotMine']))
    if lock and lock.get('load'):
        lines.append('  load at acquire: ' + lock['load'])
    for note in reply.get('notes') or ():
        lines.append('  note: ' + note)
    if reply.get('receipt'):
        lines.append('  receipt on the host: ' + reply['receipt'])
    tray = reply.get('trayHost')
    if tray and tray.get('ready'):
        lines.append(f'  tray host: {tray.get("extension")} {tray.get("state")}, StatusNotifierWatcher host registered')
    if action == 'release' and reply['ok']:
        lines.append('  Tell the next session in the queue and your coordinator')
    return lines


def explain_tray_host(reply):
    """Turn a refusal for a missing tray host into one line that says what is wrong and the fix."""
    tray = reply.get('trayHost')
    if reply.get('ok') or not tray or tray.get('ready'):
        return reply
    problem, fix = tray_host_module().missing(tray)
    return dict(reply, message=f'{reply["host"]} has no tray host: {problem}. Fix: {fix}. '
                               'To test the no-tray path on purpose, pass --no-tray-host "<reason>"')


def tray_host_module():
    """tools/lab/gnome_tray_host.py, loaded beside this file (only on the workstation side)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('gnome_tray_host', Path(__file__).with_name('gnome_tray_host.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def guard_command(args, run):
    session = os.environ.get(SESSION_VARIABLE, '') if args.session is None else args.session
    if args.host is None:  # on the builder itself
        reason = quiet_holder(session=session)
        reply = {'ok': reason is None, 'quiet': reason is None, 'message': reason}
        print(json.dumps(reply) if args.json else reason or 'quiet lease free for heavy work')
        return 0 if reason is None else 1
    reply = call(args.host, {'action': 'guard', 'resource': 'quiet', 'session': session}, args.identity, args.pins, run)
    print(json.dumps(reply, indent=2) if args.json else '\n'.join(describe(reply)))
    return 3 if reply.get('unreachable') else 0 if reply['ok'] else 1


def held_command(args, run):
    session = os.environ.get(SESSION_VARIABLE, '') if args.session is None else args.session
    if not session:
        print(f'held: pass --session or set {SESSION_VARIABLE}', file=sys.stderr)
        return 2
    if args.host is None:  # on the builder itself
        desktop = Desktop(Path(LOCK_DIR).expanduser(), socket.gethostname(), resource=args.resource)
        try:
            desktop.held(session)
            reason = None
        except LockError as error:
            reason = str(error)
        reply = {'ok': reason is None, 'message': reason}
        print(json.dumps(reply) if args.json else reason or 'held by this session')
        return 0 if reason is None else 1
    reply = call(args.host, {'action': 'held', 'resource': args.resource, 'session': session},
                 args.identity, args.pins, run)
    print(json.dumps(reply, indent=2) if args.json else '\n'.join(describe(reply)))
    return 3 if reply.get('unreachable') else 0 if reply['ok'] else 1


def add_tray_host(payload, args, parser):
    """An acquire of a GNOME desktop carries the tray host probe, or records why it is skipped."""
    if payload['action'] == 'acquire' and args.resource == 'desktop' and args.host in GNOME_HOSTS:
        if args.no_tray_host is None:
            payload['tray_probe'] = tray_host_module().probe_source()
        elif not args.no_tray_host.strip():
            parser.error('--no-tray-host needs a reason')
        else:
            payload['purpose'] = f'{args.purpose} [no tray host: {args.no_tray_host.strip()}]'
    elif getattr(args, 'no_tray_host', None) is not None:
        parser.error('--no-tray-host applies only to the desktop of a GNOME host: ' + ', '.join(sorted(GNOME_HOSTS)))


def run_command(args, command, parser, run=subprocess.run, popen=subprocess.Popen, every=RENEW_SECONDS):
    """`run`: CMD starts on the builder only once the lock is ours, and the lock is renewed while it runs."""
    if args.resource not in ('desktop', 'quiet'):
        parser.error('run takes --resource desktop or quiet')
    if not command:
        parser.error('run needs a command after --')
    if args.json:
        parser.error('run prints its lock replies as text on stderr; --json does not apply')
    every = min(every, args.minutes * 60 / 3)  # three renewals per lease, so one missed renewal never lapses it

    def lock_call(action, timeout=90, **fields):
        reply = call(args.host, dict(fields, action=action, resource=args.resource, session=args.session),
                     args.identity, args.pins, run, timeout=timeout)
        print('\n'.join(describe(reply)), file=sys.stderr)
        return reply

    acquire = {'action': 'acquire', 'owner': args.owner, 'purpose': args.purpose, 'minutes': args.minutes}
    add_tray_host(acquire, args, parser)
    deadline = monotonic() + args.minutes * 60  # from before the ask: no later than the lease's expiry on the host
    reply = explain_tray_host(call(args.host, dict(acquire, resource=args.resource, session=args.session),
                                   args.identity, args.pins, run))
    if not reply['ok'] and (reply.get('lock') or {}).get('session') == args.session:
        deadline = monotonic() + args.minutes * 60
        reply = lock_call('extend', minutes=args.minutes)  # already ours: confirm it under the mutex and renew it
    else:
        print('\n'.join(describe(reply)), file=sys.stderr)
    if not reply['ok']:
        print(f'{args.host}: command not run: this session does not hold the lock', file=sys.stderr)
        return 3 if reply.get('unreachable') else 1
    # CMD's words reach the remote shell as they were given; a shell expression goes in `sh -c '...'`.
    remote = [f'export {SESSION_VARIABLE}={shlex.quote(args.session)};', shlex.join(command)]
    process = popen(ssh_command(args.host, args.identity, args.pins, remote))
    while True:
        try:
            code = process.wait(timeout=every)
            break
        except subprocess.TimeoutExpired:
            asked = monotonic()
            # A renewal that hangs must not outlast the lease: it gives up 5 s before the expiry.
            renewed = lock_call('extend', timeout=max(1, min(90, deadline - asked - 5)), minutes=args.minutes)
            if renewed['ok']:
                deadline = asked + args.minutes * 60
            # Unconfirmed and too close to the expiry to wait for the next try: stop before it lapses.
            if not renewed['ok'] and (not renewed.get('unreachable') or deadline - monotonic() <= every):
                print(f'{args.host}: the lock is no longer confirmed ours; stopping the command\'s SSH (anything '
                      'it started on the host may still be running)', file=sys.stderr)
                process.terminate()
                process.wait()
                return 1
    if args.release:
        released = lock_call('release', verified_clean=True)
        if code == 0 and not released['ok']:
            return 3 if released.get('unreachable') else 1
    return code


def main(argv=None, run=subprocess.run, popen=subprocess.Popen):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true', help='print the raw replies')
    common.add_argument('--resource', choices=sorted(RESOURCES), default='desktop',
                        help="what to lock: the desktop (default), the builder's put.gd Proton session, or its "
                             'quiet lease (no heavy work on the host)')
    common.add_argument('--identity', type=Path, default=IDENTITY, help=argparse.SUPPRESS)
    common.add_argument('--pins', type=Path, default=PINS, help=argparse.SUPPRESS)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='action', required=True)
    status = commands.add_parser('status', parents=[common], help='show every desktop lock')
    status.add_argument('hosts', nargs='*', metavar='HOST')
    status.add_argument('--check', action='store_true', help="also list what's on each host")
    acquire = commands.add_parser('acquire', parents=[common], help='take a free desktop')
    heartbeat = commands.add_parser('heartbeat', parents=[common], help='show you are still active')
    extend = commands.add_parser('extend', parents=[common], help='move the expiry to N minutes from now')
    release = commands.add_parser('release', parents=[common], help='check the host and give the desktop back')
    breaking = commands.add_parser('break', parents=[common], help='remove an expired or stale lock')
    guard = commands.add_parser('guard', help="exit 1 while another session holds the host's quiet lease")
    guard.add_argument('host', nargs='?', choices=sorted(HOSTS), help="omit on a builder to check its own lock directory")
    guard.add_argument('--session', help=f'your session id; your own lease passes (default ${SESSION_VARIABLE})')
    guard.add_argument('--json', action='store_true', help='print the raw reply')
    guard.add_argument('--identity', type=Path, default=IDENTITY, help=argparse.SUPPRESS)
    guard.add_argument('--pins', type=Path, default=PINS, help=argparse.SUPPRESS)
    held = commands.add_parser('held', parents=[common], help='exit 0 only while your session holds the lock')
    held.add_argument('host', nargs='?', choices=sorted(HOSTS), help="omit on a builder to check its own lock directory")
    held.add_argument('--session', help=f'your session id (default ${SESSION_VARIABLE})')
    running = commands.add_parser('run', parents=[common], help='take the lock, then run -- CMD on the host',
                                  usage='%(prog)s HOST --owner NAME --session ID --purpose TEXT --minutes N '
                                        '[--resource desktop|quiet] [--no-tray-host REASON] [--release] -- CMD ...')
    running.add_argument('--release', action='store_true', help='release the lock after CMD (--verified-clean)')
    for command in (acquire, heartbeat, extend, release, breaking, running):
        command.add_argument('host', choices=sorted(HOSTS))
        command.add_argument('--session', required=True, help='your session id, how others reach you')
    for command in (acquire, running):
        command.add_argument('--owner', required=True, help='short session name')
        command.add_argument('--purpose', required=True)
        command.add_argument('--no-tray-host', metavar='REASON',
                             help='lock a GNOME desktop without its tray host, to test the no-tray path on purpose')
    for command in (acquire, extend, running):
        command.add_argument('--minutes', type=int, required=True, help=f'1-{MAX_MINUTES}')
    heartbeat.add_argument('--blocked', metavar='REASON', help='say you are blocked (cleared by the next heartbeat)')
    release.add_argument('--verified-clean', action='store_true',
                         help='you stopped and removed everything you started there')
    release.add_argument('--not-mine', action='append', default=[], metavar='ID',
                         help="another session's item that appeared during your lock (repeat)")
    release.add_argument('--removed-ok', metavar='REASON',
                         help='release although a putgd package there at acquire is gone; REASON goes in the receipt')
    breaking.add_argument('--reason', required=True)
    argv = sys.argv[1:] if argv is None else list(argv)
    command = []
    if argv[:1] == ['run'] and '--' in argv:
        argv, command = argv[:argv.index('--')], argv[argv.index('--') + 1:]
    args = parser.parse_args(argv)
    if args.action == 'guard':
        return guard_command(args, run)
    if args.action == 'held':
        return held_command(args, run)
    if args.action == 'run':
        return run_command(args, command, parser, run, popen)

    payload = {key: value for key, value in vars(args).items()
               if key not in ('json', 'identity', 'pins', 'host', 'hosts', 'no_tray_host') and value is not None}
    hosts = (args.hosts or sorted(HOSTS)) if args.action == 'status' else [args.host]
    add_tray_host(payload, args, parser)
    unknown = sorted(set(hosts) - set(HOSTS))
    if unknown:
        parser.error('unknown host ' + ', '.join(unknown) + '; choose from ' + ', '.join(sorted(HOSTS)))
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        replies = [explain_tray_host(reply) for reply in
                   pool.map(lambda host: call(host, payload, args.identity, args.pins, run), hosts)]
    if args.json:
        print(json.dumps(replies if args.action == 'status' else replies[0], indent=2))
    else:
        for reply in replies:
            print('\n'.join(describe(reply)))
    if any(reply.get('unreachable') for reply in replies):
        return 3
    return 0 if all(reply['ok'] for reply in replies) else 1


if __name__ == '__main__' and sys.argv[0] != '-':  # `python3 -` on a builder runs remote() instead
    raise SystemExit(main())
