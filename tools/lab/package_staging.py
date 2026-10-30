#!/usr/bin/env python3
"""Unprivileged lifetime fence for helper staging in a disposable overlay."""
import ctypes
import errno
import fcntl
import json
import os
import select
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


def save(name, value):
    temporary = Path(name + '.tmp')
    temporary.write_text(json.dumps(value) + '\n')
    temporary.replace(name)


CGROUP_ROOT = Path('/sys/fs/cgroup')
BOOT_ID = Path('/proc/sys/kernel/random/boot_id')


def unified_mount():
    if not any(line.split()[1:3] == ['/sys/fs/cgroup', 'cgroup2']
               for line in Path('/proc/mounts').read_text().splitlines()):
        raise ValueError('A cgroup v2 mount at /sys/fs/cgroup is required')
    return CGROUP_ROOT.stat()


def populated(directory):
    # One read of the kernel's hierarchical, atomic emptiness flag. Never infer
    # emptiness from cgroup.procs (which also omits delegated child cgroups).
    values = [line.split()[1:] for line in (directory / 'cgroup.events').read_text().splitlines()
              if line.split()[:1] == ['populated']]
    if values not in ([['0']], [['1']]):
        raise ValueError('Invalid cgroup.events populated flag')
    return values == [['1']]


def scope_identity(unit):
    root = unified_mount()
    paths = [line[3:] for line in Path('/proc/self/cgroup').read_text().splitlines()
             if line.startswith('0::')]
    if len(paths) != 1 or Path(paths[0]).name != unit:
        raise ValueError('Staging did not enter its dedicated scope')
    directory = CGROUP_ROOT / paths[0].lstrip('/')
    info = directory.stat()
    if not populated(directory):
        raise ValueError('Running scope is not populated')
    return {'path': paths[0], 'unit': unit, 'device': info.st_dev, 'inode': info.st_ino,
            'mountDevice': root.st_dev, 'mountInode': root.st_ino,
            'bootId': BOOT_ID.read_text().strip()}


def launch(source, directory, script, supervisor=False):
    """Choose once, before work. A failed scope launch is never retried unscoped."""
    environment = {**os.environ, 'XDG_RUNTIME_DIR': f'/run/user/{os.getuid()}',
                   'DBUS_SESSION_BUS_ADDRESS': f'unix:path=/run/user/{os.getuid()}/bus'}
    reason = 'cgroup v2 or the systemd user manager is unavailable'
    try:
        unified_mount()
        available = shutil.which('systemd-run') and subprocess.run(
            ['systemctl', '--user', 'show-environment'], env=environment,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5).returncode == 0
    except (OSError, ValueError, subprocess.TimeoutExpired):
        available = False
    arguments = [sys.executable, '-c', source, 'supervise' if supervisor else 'run', directory, script]
    if available:
        job = Path(directory).name
        if not re.fullmatch(r'install-[0-9a-f]{32}', job):
            raise ValueError('Scope requires an install job ID')
        unit = ('putgd-supervisor-' if supervisor else 'putgd-staging-') + job + '.scope'
        arguments += ['--scope', unit]
        arguments = ['systemd-run', '--user', '--scope', '--quiet', '--property=Delegate=yes',
                     '--unit=' + unit, '--expand-environment=no', '--', *arguments]
    else:
        arguments += ['--session-fallback', reason]
    os.execvpe(arguments[0], arguments, environment)


def run(script, unit=None, fallback='direct session-only invocation', supervisor=False):
    # Every staging descendant must remain identifiable after wrapper death.
    if os.getsid(0) != os.getpid():
        os.setsid()
    # nohup protects this process from the SSH session disappearing. A terminal
    # receipt requires ECHILD, including adopted sudo/apt/hook descendants.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'Cannot become staging subreaper')
    with open('staging.lock', 'a') as fence:
        fcntl.flock(fence, fcntl.LOCK_EX)
        if Path('staging-cancelled').exists():
            return 125
        identity = {'pid': os.getpid(), 'sessionId': os.getsid(0), 'startTicks': int(
            Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19])}
        if unit:
            identity.update(tracking='cgroup-v2', cgroup=scope_identity(unit))
        else:
            identity.update(tracking='session', fallbackReason=fallback)
        prefix = 'supervisor' if supervisor else 'staging'
        save(prefix + '-process.json', identity)
        process = subprocess.Popen(['sh', '-c', script], stdin=subprocess.DEVNULL)
        code = process.wait()
        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                break
        save(prefix + '-result.json', {**identity, 'exitCode': code, 'finished': True})
        return code


# A wrapper that reached its terminal receipt also waited, as subreaper, for
# descendants that called setsid(). After the wrapper is killed, only the session
# scan remains on session-only guests, and it cannot see those: the in-tree hook and apt-get update with
# no sources start none, but a guest's own APT Pre-Invoke hook could.
ABANDONED_LIMIT = ('Abandonment proves only that no process remains in the staging session. A '
                   'descendant that called setsid() (for example a daemonizing guest APT Pre-Invoke '
                   'hook) left that session and is not checked, and the /proc scan is not atomic: '
                   'a member that keeps forking and exiting could evade both passes. Inspect the '
                   'guest process list if its hooks can daemonize.')


def cgroup_status(identity):
    if identity.get('tracking', 'session') == 'session' and 'cgroup' not in identity:
        return {'tracking': 'session', 'limit': ABANDONED_LIMIT,
                'fallbackReason': identity.get('fallbackReason', 'legacy session-only identity')}
    status = {'tracking': 'cgroup-v2', 'cgroupPopulated': None, 'cgroupGone': False}
    try:
        group = identity['cgroup']
        path = group['path']
        unit = group['unit']
        if (identity.get('tracking') != 'cgroup-v2' or not isinstance(path, str)
                or not re.fullmatch(r'putgd-staging-install-[0-9a-f]{32}\.scope', unit)
                or not path.startswith('/user.slice/') or Path(path).name != unit
                or any(part in ('', '.', '..') for part in path.split('/')[1:])
                or any(type(group[k]) is not int or group[k] <= 0
                       for k in ('device', 'inode', 'mountDevice', 'mountInode'))):
            raise ValueError('Invalid cgroup identity')
        root = unified_mount()
        if ((root.st_dev, root.st_ino) != (group['mountDevice'], group['mountInode'])
                or BOOT_ID.read_text().strip() != group['bootId']):
            raise ValueError('Cgroup mount or boot identity changed')
        # Pin components without following symlinks. ENOENT here, with the same
        # boot and mount, proves the recorded cgroup gone. ENOENT for EVENTS
        # does not: a missing/unreadable events file always retains the lock.
        fd = os.open(CGROUP_ROOT, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            try:
                for part in path.split('/')[1:]:
                    child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
                    os.close(fd)
                    fd = child
            except FileNotFoundError:
                return {**status, 'cgroupGone': True, 'cgroupPopulated': False}
            info = os.fstat(fd)
            if (info.st_dev, info.st_ino) != (group['device'], group['inode']):
                raise ValueError('Recorded cgroup was replaced')
            status['cgroupPopulated'] = populated(Path(f'/proc/self/fd/{fd}'))
        finally:
            os.close(fd)
        return status
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {**status, 'inspectionError': 'Cannot inspect staging cgroup: ' + type(error).__name__}


def proc_fields(pid):
    # stat field 6 is the session ID, even for root-owned descendants. The
    # command name can itself contain spaces and parentheses.
    return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()


def session_status(session):
    try:
        # hidepid can silently omit other users' processes from enumeration.
        for line in Path('/proc/mounts').read_text().splitlines():
            fields = line.split()
            if fields[1] == '/proc' and any(option.startswith('hidepid=') and
                    option != 'hidepid=0' for option in fields[3].split(',')):
                raise PermissionError('procfs hidepid prevents complete session enumeration')
        def scan():
            live = []
            for entry in Path('/proc').iterdir():
                if not entry.name.isdecimal():
                    continue
                try:
                    fields = proc_fields(entry.name)
                    if int(fields[3]) == session and fields[0] not in ('Z', 'X'):
                        live.append(int(entry.name))
                except (FileNotFoundError, ProcessLookupError):
                    continue  # A process that exited during the scan is harmless.
            return sorted(live)
        # A member can fork and exit while one pass runs; after a PID wrap its
        # child can land behind the scan position and be missed. Any live member
        # refuses at once; an empty session must be empty on two passes. Without
        # freezing the session (signals, or a cgroup: #780) no scan is atomic.
        live = scan() or scan()
        return {'sessionAlive': bool(live), 'sessionPids': live}
    except (OSError, ValueError, IndexError) as error:
        return {'sessionAlive': None,
                'inspectionError': 'Cannot enumerate staging session: ' + type(error).__name__}


def process_status():
    handle = None
    status = {'exactProcessAlive': None, 'sessionAlive': None}
    try:
        identity = json.loads(Path('staging-process.json').read_text())
        pid = identity['pid']
        if (type(pid) is not int or pid <= 0 or
                type(identity['startTicks']) is not int or identity['startTicks'] <= 0 or
                type(identity['sessionId']) is not int or identity['sessionId'] != pid):
            raise ValueError('Invalid staging session leader identity')
        status['process'] = identity
        try:
            # Missing API and ENOSYS (new Python on an older kernel) both use
            # /proc. EINVAL means the PID is still referenced (as this session's
            # ID) but no process has it: the leader is dead and the PID cannot
            # be reused, so the session scan decides. All other failures leave
            # the leader's identity uncertain.
            pidfd_open = getattr(os, 'pidfd_open', None)
            if pidfd_open is not None:
                try:
                    handle = pidfd_open(pid)
                except OSError as error:
                    if error.errno == errno.EINVAL:
                        raise ProcessLookupError(error.errno, 'Session leader exited') from None
                    if error.errno != errno.ENOSYS:
                        raise
            fields = proc_fields(pid)
            alive = int(fields[19]) == identity['startTicks'] and fields[0] not in ('Z', 'X')
            if handle is not None:
                alive = alive and not select.select([handle], [], [], 0)[0]
            status['exactProcessAlive'] = alive
        except (ProcessLookupError, FileNotFoundError):
            status['exactProcessAlive'] = False
        return {**status, **session_status(identity['sessionId']), **cgroup_status(identity)}
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        return {**status, 'inspectionError':
                'Cannot establish staging session leader identity: ' + type(error).__name__}
    finally:
        if handle is not None:
            os.close(handle)


def reconcile(seconds, abandoned=False):
    deadline = time.monotonic() + seconds
    with open('staging.lock', 'a') as fence:
        while True:
            try:
                fcntl.flock(fence, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    return {'finished': False, 'state': 'staging-running', **process_status()}
                time.sleep(.1)
        # Fence a delayed launch, too: lack of a PID file alone cannot prove
        # that an SSH command has not started (or will not start later).
        Path('staging-cancelled').touch(mode=0o600, exist_ok=True)
        if not Path('staging-process.json').exists() and not abandoned:
            return {'finished': True, 'state': 'staging-not-started'}
        status = process_status()
        # The wrapper releases the flock just before it exits, and session
        # members may still be finishing. Wait out confirmed liveness within the
        # deadline; never wait out uncertain (None) inspection.
        while ((status['exactProcessAlive'] is True or status['sessionAlive'] is True
                or status.get('cgroupPopulated') is True)
               and time.monotonic() < deadline):
            time.sleep(.1)
            status = process_status()
        # A free flock or terminal receipt does not establish that a killed
        # wrapper's privileged descendants exited. Both paths require a complete
        # session scan and a verifiable, no-longer-live session leader.
        if (status['exactProcessAlive'] is not False or status['sessionAlive'] is not False
                or (status.get('tracking') == 'cgroup-v2' and status.get('cgroupPopulated') is not False)):
            return {'finished': False, 'state': 'staging-outcome-unknown', **status}
        if abandoned:
            return {'finished': True, 'state': 'staging-abandoned', **status}
        if Path('staging-result.json').exists():
            result = json.loads(Path('staging-result.json').read_text())
            identity = status['process']
            if (result.get('finished') is True and all(result.get(k) == identity[k]
                    for k in ('pid', 'startTicks', 'sessionId'))):
                return {**result, **status, 'state': 'staging-finished'}
        # No terminal receipt: abandonment requires explicit operator recovery.
        return {'finished': False, 'state': 'staging-outcome-unknown', **status}


def main():
    os.umask(0o077)
    os.chdir(sys.argv[2])
    if sys.argv[1] in ('launch', 'launch-supervisor'):
        return launch(sys.argv[4], sys.argv[2], sys.argv[3], sys.argv[1] == 'launch-supervisor')
    if sys.argv[1] in ('run', 'supervise'):
        return run(sys.argv[3], unit=sys.argv[5] if sys.argv[4:5] == ['--scope'] else None,
                   fallback=sys.argv[5] if sys.argv[4:5] == ['--session-fallback'] else
                   'direct session-only invocation', supervisor=sys.argv[1] == 'supervise')
    print(json.dumps(reconcile(float(sys.argv[3]), abandoned='--abandoned' in sys.argv[4:])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
