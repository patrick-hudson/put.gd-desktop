#!/usr/bin/env python3
"""Exclusive locks for the builders' physical desktops and Proton sessions (stdlib only).

Each builder's GNOME or Plasma desktop serves one session at a time. Its lock
lives on that builder, in ~/putgd-lab/locks/, so every session that can reach
the desktop over pinned SSH sees the same lock, whatever its app, worktree or
session family. The builder's clock decides expiry and staleness. The rules are
in docs/remote-builders.md, "Sharing the lab between concurrent sessions".

`--resource proton` locks the builder's put.gd Proton session instead: one user
at a time (the lab profile's app, a VM borrowing it through
tools/lab/proton_lab.py lend, or a refresh), because every refresh rotates its
token. It is independent of the desktop lock and has no cleanup inventory.

  desktop_lock.py status [HOST ...] [--check] [--resource R]
  desktop_lock.py acquire HOST --owner NAME --session ID --purpose TEXT --minutes N [--resource R]
  desktop_lock.py heartbeat HOST --session ID [--blocked REASON] [--resource R]
  desktop_lock.py extend HOST --session ID --minutes N [--resource R]
  desktop_lock.py release HOST --session ID [--verified-clean] [--not-mine ID ...] [--resource R]
  desktop_lock.py break HOST --session ID --reason TEXT [--resource R]

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
from pathlib import Path
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
USER = 'patrick'
IDENTITY = Path.home() / '.ssh/awskey_110724'
PINS = Path.home() / 'work/put.gd/work/remote-builders'  # <host>/known_hosts
LOCK_DIR = '~/putgd-lab/locks'
RESOURCES = {'desktop': 'desktop', 'proton': 'Proton session'}  # lock file <name>.lock
MAX_MINUTES = 45
STALE_AFTER = datetime.timedelta(minutes=15)
FIELDS = ('owner', 'session', 'host', 'purpose', 'acquired', 'expires', 'heartbeat')
PROCESS_PATTERN = 'putgd|host_input|videotestsrc'
ALWAYS_REPORTED = ('host_input', 'videotestsrc')  # input agents and test patterns, with or without a display
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
    keys = FIELDS + tuple(key for key in ('resource', 'blocked') if record.get(key))
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


def compare(baseline, current, not_mine=()):
    """Split the host's items into ones present at acquire and ones that appeared since."""
    known = {item['key'] for item in baseline}
    new = [item for item in current if item['key'] not in known]
    claimed = set(not_mine)
    return {'left': [item for item in new if item['id'] not in claimed],
            'notMine': [item for item in new if item['id'] in claimed],
            'preexisting': [item for item in current if item['key'] in known],
            'unmatchedNotMine': sorted(claimed - {item['id'] for item in new})}


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


def collect_inventory(home, run=execute, own=None, environment=process_environment):
    """List what a desktop session can leave on this host: matching processes in the desktop
    session (or that can't be told apart), input agents and test patterns, Flatpak instances,
    user desktop entries, extension copies and data, and a putgd package. Matching processes
    without DISPLAY or WAYLAND_DISPLAY are shared SSH work (builds, syncs, collectors)."""
    own = {os.getpid(), os.getppid()} if own is None else own
    items, notes = [], []

    def add(kind, ident, key, detail):
        items.append({'kind': kind, 'id': ident, 'key': key,
                      'detail': detail if len(detail) <= 160 else detail[:157] + '...'})

    code, output = run(['pgrep', '-u', str(os.getuid()), '-af', PROCESS_PATTERN])
    if code is None or code > 1:
        raise LockError('Cleanup check could not run pgrep on this host')
    shared = []
    for line in output.splitlines():
        pid, _, command = line.strip().partition(' ')
        if not pid.isdigit() or int(pid) in own:
            continue
        names = environment(pid)
        if names is False:
            continue
        if names is not None and not names & {'DISPLAY', 'WAYLAND_DISPLAY'} and \
                not any(word in command for word in ALWAYS_REPORTED):
            shared.append(pid)
            continue
        add('process', pid, f'process:{pid}:{process_start(pid)}', command)
    if shared:
        notes.append(f'{len(shared)} process(es) outside the desktop session (no DISPLAY or '
                     'WAYLAND_DISPLAY; shared SSH work): ' + ' '.join(shared[:8]) +
                     (' ...' if len(shared) > 8 else ''))
    code, output = run(['flatpak', 'ps', '--columns=instance,pid,application'])
    if code is None:
        notes.append('flatpak is not installed')
    for fields in (line.split() for line in output.splitlines()):
        if len(fields) >= 2 and fields[0].isdigit():  # not a header
            add('flatpak', fields[0], 'flatpak:' + fields[0], 'PID ' + ' '.join(fields[1:]))
    for pattern in LEFTOVER_PATHS:
        for path in sorted(home.glob(pattern)):
            add('file', str(path), f'file:{path}', '')
    code, output = run(['rpm', '-q', '--qf', '%{VERSION}-%{RELEASE}', 'putgd'])
    if code == 0:
        add('package', 'putgd', 'package:rpm:' + output.strip(), 'rpm putgd ' + output.strip())
    code, output = run(['dpkg-query', '-W', '-f', '${db:Status-Abbrev} ${Version}', 'putgd'])
    status, _, version = output.partition(' ')
    if code == 0 and status[1:2] not in ('', 'n', 'c'):
        add('package', 'putgd', 'package:deb:' + version.strip(), 'deb putgd ' + version.strip())
    return items, notes


class Desktop:
    """One lock (the desktop, or the Proton session) in a builder's lock directory.
    Every change happens under an flock shared by all of the directory's locks."""

    def __init__(self, directory, host, clock=now_utc, inventory=None, resource='desktop'):
        if resource not in RESOURCES:
            raise LockError('Unknown resource ' + str(resource))
        self.directory, self.host, self.clock, self.resource = Path(directory), host, clock, resource
        # Only the desktop has a cleanup inventory; a Proton session leaves nothing on screen.
        self.inventory = inventory or (lambda: collect_inventory(Path.home()) if resource == 'desktop' else ([], []))
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
        receipts = sorted(self.history.glob('*.json')) if self.history.is_dir() else []
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
        items, notes = self.inventory()
        with self.mutex():
            now = self.clock()
            current = self.state(now)
            if current['state'] != 'free':
                raise LockError(f'{self.name} is already locked', **current)
            record = {'owner': owner, 'session': session, 'host': self.host, 'purpose': purpose,
                      'acquired': stamp(now), 'expires': stamp(now + datetime.timedelta(minutes=minutes)),
                      'heartbeat': stamp(now)}
            if self.resource != 'desktop':
                record['resource'] = self.resource
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
        return {'lock': record, 'baseline': items, 'notes': notes}

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

    def release(self, session, verified_clean=False, not_mine=()):
        items, notes = self.inventory()
        with self.mutex():
            now = self.clock()
            record = self.held_by(session, now)
            check = compare(self.read_baseline(), items, not_mine)
            if check['left']:
                raise LockError(f'Not released: {len(check["left"])} item(s) appeared on {self.host} since you '
                                'acquired it. Stop or remove yours (exact PIDs only) and check again; name '
                                "another session's item with --not-mine ID", check=check, notes=notes)
            if not verified_clean and self.resource == 'proton':
                raise LockError('Confirm that no lent copy is outstanding (every carry-back confirmed, every '
                                'putgd-lab guest entry cleaned up), then rerun with --verified-clean',
                                check=check, notes=notes)
            if not verified_clean:
                raise LockError('Nothing new is left on the host. Finish the manual steps (workstation '
                                'forwards, re-login after an extension copy), then rerun with --verified-clean',
                                check=check, notes=notes)
            receipt = self.receipt('release', {'lock': record, 'releasedBy': session, 'check': check,
                                               'notes': notes})
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


def remote(payload):
    """Run one lock operation on this host and print its JSON reply."""
    desktop = Desktop(Path(payload.get('directory') or LOCK_DIR).expanduser(), payload['host'],
                      resource=payload.get('resource', 'desktop'))
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
                                    payload.get('not_mine', ()))
        elif action == 'break':
            reply = desktop.break_lock(payload['session'], payload['reason'])
        else:
            raise LockError('Unknown action ' + action)
        reply = dict(reply, ok=True)
    except LockError as error:
        reply = dict(error.details, ok=False, message=str(error))
    print(json.dumps(dict(reply, host=desktop.host, action=action, resource=desktop.resource,
                          now=stamp(desktop.clock()))))
    return 0


def ssh_command(host, identity=IDENTITY, pins=PINS):
    return ['ssh', '-i', str(identity), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=8',
            '-o', 'UserKnownHostsFile=' + str(Path(pins) / host / 'known_hosts'),
            f'{USER}@{HOSTS[host][0]}', 'python3', '-']


def remote_script(payload):
    return Path(__file__).read_text() + '\nraise SystemExit(remote(json.loads(%r)))\n' % json.dumps(payload)


def call(host, payload, identity=IDENTITY, pins=PINS, run=subprocess.run, command=None):
    """Send this script to HOST over pinned SSH and return its reply."""
    failed = {'ok': False, 'unreachable': True, 'host': host, 'action': payload['action']}
    known_hosts = Path(pins) / host / 'known_hosts'
    if command is None and not known_hosts.is_file():
        return dict(failed, message=f'No pinned host key at {known_hosts}. Recover the verified pin '
                                    '(docs/remote-builders.md); never trust on first use')
    try:
        result = run(command or ssh_command(host, identity, pins), input=remote_script(dict(payload, host=host)),
                     capture_output=True, text=True, timeout=90)
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


DONE = {'acquire': 'locked', 'heartbeat': 'heartbeat recorded', 'extend': 'expiry moved',
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
    if action == 'acquire' and not reply['ok'] and lock:
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
        if check['preexisting'] and action == 'release' and reply['ok']:
            lines.append(f'  {len(check["preexisting"])} item(s) that were there before the lock stay')
        elif check['preexisting']:
            lines += describe_items('already there when the lock was taken', check['preexisting'])
        if check['unmatchedNotMine']:
            lines.append('  --not-mine matched nothing new: ' + ', '.join(check['unmatchedNotMine']))
    for note in reply.get('notes') or ():
        lines.append('  note: ' + note)
    if reply.get('receipt'):
        lines.append('  receipt on the host: ' + reply['receipt'])
    if action == 'release' and reply['ok']:
        lines.append('  Tell the next session in the queue and your coordinator')
    return lines


def main(argv=None, run=subprocess.run):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--json', action='store_true', help='print the raw replies')
    common.add_argument('--resource', choices=sorted(RESOURCES), default='desktop',
                        help="what to lock: the desktop (default) or the builder's put.gd Proton session")
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
    for command in (acquire, heartbeat, extend, release, breaking):
        command.add_argument('host', choices=sorted(HOSTS))
        command.add_argument('--session', required=True, help='your session id, how others reach you')
    acquire.add_argument('--owner', required=True, help='short session name')
    acquire.add_argument('--purpose', required=True)
    for command in (acquire, extend):
        command.add_argument('--minutes', type=int, required=True, help=f'1-{MAX_MINUTES}')
    heartbeat.add_argument('--blocked', metavar='REASON', help='say you are blocked (cleared by the next heartbeat)')
    release.add_argument('--verified-clean', action='store_true',
                         help='you stopped and removed everything you started there')
    release.add_argument('--not-mine', action='append', default=[], metavar='ID',
                         help="another session's item that appeared during your lock (repeat)")
    breaking.add_argument('--reason', required=True)
    args = parser.parse_args(argv)

    payload = {key: value for key, value in vars(args).items()
               if key not in ('json', 'identity', 'pins', 'host', 'hosts') and value is not None}
    hosts = (args.hosts or sorted(HOSTS)) if args.action == 'status' else [args.host]
    unknown = sorted(set(hosts) - set(HOSTS))
    if unknown:
        parser.error('unknown host ' + ', '.join(unknown) + '; choose from ' + ', '.join(sorted(HOSTS)))
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        replies = list(pool.map(lambda host: call(host, payload, args.identity, args.pins, run), hosts))
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
