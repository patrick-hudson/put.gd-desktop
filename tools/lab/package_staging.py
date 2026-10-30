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
import signal
import stat
import subprocess
import sys
import time


def save(name, value):
    temporary = Path(name + '.tmp')
    temporary.write_text(json.dumps(value) + '\n')
    temporary.replace(name)


CGROUP_ROOT = Path('/sys/fs/cgroup')
BOOT_ID = Path('/proc/sys/kernel/random/boot_id')
# Root-owned in the overlay (staged with package_install.py) and granted alone.
SCOPE_LAUNCHER = '/usr/local/lib/putgd-lab/package_scope.py'
# The root supervisor writes result.json; the lab user owns its directory.
RESULT_OWNER = 0


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
    if len(paths) != 1 or paths[0] != '/system.slice/' + unit:
        raise ValueError('Staging did not enter its dedicated system scope')
    directory = CGROUP_ROOT / paths[0].lstrip('/')
    info = directory.stat()
    if not populated(directory):
        raise ValueError('Running scope is not populated')
    return {'path': paths[0], 'unit': unit, 'device': info.st_dev, 'inode': info.st_ino,
            'mountDevice': root.st_dev, 'mountInode': root.st_ino,
            'bootId': BOOT_ID.read_text().strip()}


def system_scope_tools():
    """Probe each prerequisite without creating a unit or relying on a login."""
    try:
        unified_mount()
    except (OSError, ValueError):
        return None, 'cgroup v2 mount at /sys/fs/cgroup is unavailable'
    paths = {}
    for name in ('systemd-run', 'systemctl', 'sudo'):
        paths[name] = shutil.which(name)
        if not paths[name]:
            return None, name + ' executable is unavailable'
    try:
        info = os.stat(SCOPE_LAUNCHER)
    except OSError:
        return None, 'scope launcher ' + SCOPE_LAUNCHER + ' is not installed'
    if info.st_uid != 0 or info.st_mode & 0o022 or not stat.S_ISREG(info.st_mode):
        return None, 'scope launcher is not a root-owned, root-writable-only file'
    probes = [
        ([paths['systemd-run'], '--version'], 'systemd-run version probe'),
        ([paths['systemctl'], '--system', 'show', '--property=Version', '--value'],
         'system manager bus probe'),
        ([paths['sudo'], '-n', '--', SCOPE_LAUNCHER, '--probe'],
         'passwordless sudo for scope launcher probe'),
    ]
    for arguments, prerequisite in probes:
        try:
            result = subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                    text=True, timeout=5)
        except subprocess.TimeoutExpired:
            return None, prerequisite + ' timed out'
        except OSError:
            return None, prerequisite + ' could not execute'
        if result.returncode or (prerequisite.endswith('launcher probe')
                                 and result.stdout.strip() != 'putgd-lab-scope-ready'):
            return None, prerequisite + ' failed'
        if prerequisite == 'systemd-run version probe':
            version = re.match(r'systemd ([0-9]+)\b', result.stdout)
            if not version:
                return None, 'systemd-run version could not be established'
            if int(version[1]) < 254:
                return None, 'systemd-run version ' + version[1] + ' is below required 254'
    return paths, None


def launch(source, directory, script, supervisor=False):
    """Choose once, before work. A failed scope launch is never retried unscoped."""
    paths, reason = system_scope_tools()
    arguments = [sys.executable, '-c', source, 'supervise' if supervisor else 'run', directory, script]
    tracking = {'tracking': 'session', 'fallbackReason': reason}
    if paths:
        job = Path(directory).name
        if not re.fullmatch(r'install-[0-9a-f]{32}', job):
            raise ValueError('Scope requires an install job ID')
        kind = 'supervisor' if supervisor else 'staging'
        unit = 'putgd-' + kind + '-' + job + '.scope'
        if supervisor:
            arguments[5] += ' --require-scope'
        arguments += ['--scope', unit]
        # PID 1 owns this scope. A user@UID.service stop after logout must not
        # kill the detached watchdog. The root launcher's fixed systemd-run and
        # setpriv arguments (no PAM session) drop back to this user before any
        # wrapper/staging code runs.
        arguments = [paths['sudo'], '-n', '--', SCOPE_LAUNCHER, kind, job, *arguments]
        tracking = {'tracking': 'cgroup-v2', 'manager': 'system', 'unit': unit}
    else:
        arguments += ['--session-fallback', reason]
        print('putgd-staging-fallback ' + json.dumps(tracking), flush=True)
    save(('supervisor' if supervisor else 'staging') + '-tracking.json', tracking)
    os.execvpe(arguments[0], arguments, os.environ.copy())


LAUNCH_SIGNALS = {signal.SIGHUP, signal.SIGINT, signal.SIGQUIT, signal.SIGTERM}


def detach():
    """Leave only a waiter as the scope launch client's child; return its exit code there.

    The outer sudo stays in the SSH login's session scope as this process's
    parent. Ending that login (systemctl stop session-N.scope, loginctl
    terminate-session/terminate-user) sends it SIGTERM/SIGHUP, which sudo relays
    to its child, or to the child's process group with a pty. A scope isolates
    cgroups, not signals. Only the waiter receives them; the wrapper continues in
    its own session and scope, reparented to PID 1, and stays killable directly.
    """
    signal.pthread_sigmask(signal.SIG_BLOCK, LAUNCH_SIGNALS)
    child = os.fork()
    if child:
        signal.pthread_sigmask(signal.SIG_UNBLOCK, LAUNCH_SIGNALS)
        code = os.waitstatus_to_exitcode(os.waitpid(child, 0)[1])
        return code if code >= 0 else 128 - code
    os.setsid()
    # A signal relayed to the launch group before setsid() was aimed at the
    # waiter: ignoring a pending signal discards it. Later signals act normally.
    previous = {number: signal.signal(number, signal.SIG_IGN) for number in LAUNCH_SIGNALS}
    signal.pthread_sigmask(signal.SIG_UNBLOCK, LAUNCH_SIGNALS)
    for number, handler in previous.items():
        signal.signal(number, signal.SIG_DFL if handler is None else handler)
    return None


def run(script, unit=None, fallback='direct session-only invocation', supervisor=False):
    # Every staging descendant must remain identifiable after wrapper death.
    if unit:
        waiter = detach()
        if waiter is not None:
            return waiter
    elif os.getsid(0) != os.getpid():
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


def cgroup_status(identity, kind='staging'):
    if identity.get('tracking', 'session') == 'session' and 'cgroup' not in identity:
        return {'tracking': 'session', 'limit': ABANDONED_LIMIT,
                'fallbackReason': identity.get('fallbackReason', 'legacy session-only identity')}
    status = {'tracking': 'cgroup-v2', 'cgroupPopulated': None, 'cgroupGone': False}
    try:
        group = identity['cgroup']
        path = group['path']
        unit = group['unit']
        if (identity.get('tracking') != 'cgroup-v2' or not isinstance(path, str)
                or not re.fullmatch(r'putgd-' + kind + r'-install-[0-9a-f]{32}\.scope', unit)
                or not path.startswith(('/system.slice/', '/user.slice/')) or Path(path).name != unit
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


def process_status(kind='staging'):
    handle = None
    status = {'exactProcessAlive': None, 'sessionAlive': None}
    try:
        identity = json.loads(Path(kind + '-process.json').read_text())
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
        return {**status, **session_status(identity['sessionId']), **cgroup_status(identity, kind)}
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


def terminal_result():
    """The root supervisor's result.json, written once after its package work ends.

    progress.json alone means it may still run. Refuse links, other owners and
    non-terminal content: the lab user owns this directory.
    """
    try:
        fd = os.open('result.json', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return None
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != RESULT_OWNER:
            raise ValueError('result.json is not the root supervisor\'s single-link regular file')
        result = json.loads(stream.read(1 << 20))
    if (not isinstance(result, dict) or type(result.get('ok')) is not bool
            or type(result.get('aborted')) is not bool or type(result.get('remainingPids')) is not list):
        raise ValueError('result.json is not a terminal supervisor result')
    return {key: result.get(key) for key in ('ok', 'aborted', 'timedOut', 'exitCodes', 'remainingPids', 'error')}


def reconcile_supervisor(seconds):
    """Prove an admitted supervisor finished, for a job whose controller is gone.

    Requires its terminal result.json and its recorded system scope empty
    (populated 0) or gone, plus every wrapper identity and session check. A
    session-tracked or identity-less supervisor is never released here.
    """
    deadline = time.monotonic() + seconds
    with open('staging.lock', 'a') as fence:
        while True:
            try:
                fcntl.flock(fence, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    return {'finished': False, 'state': 'supervisor-running', **process_status('supervisor')}
                time.sleep(.1)
        Path('staging-cancelled').touch(mode=0o600, exist_ok=True)
        status = process_status('supervisor')
        while ((status['exactProcessAlive'] is True or status['sessionAlive'] is True
                or status.get('cgroupPopulated') is True) and time.monotonic() < deadline):
            time.sleep(.1)
            status = process_status('supervisor')
        if status.get('tracking') != 'cgroup-v2' and 'inspectionError' not in status:
            return {'finished': False, 'state': 'supervisor-session-tracked', **status}
        if (status['exactProcessAlive'] is not False or status['sessionAlive'] is not False
                or status.get('cgroupPopulated') is not False):
            return {'finished': False, 'state': 'supervisor-outcome-unknown', **status}
        try:
            result = terminal_result()
        except (OSError, ValueError) as error:
            return {'finished': False, 'state': 'supervisor-outcome-unknown', **status,
                    'inspectionError': 'Cannot read terminal result.json: ' + str(error)}
        if result is None:
            return {'finished': False, 'state': 'supervisor-no-result', **status}
        return {'finished': True, 'state': 'supervisor-finished', **status, 'result': result}


def main():
    os.umask(0o077)
    os.chdir(sys.argv[2])
    if sys.argv[1] in ('launch', 'launch-supervisor'):
        return launch(sys.argv[4], sys.argv[2], sys.argv[3], sys.argv[1] == 'launch-supervisor')
    if sys.argv[1] in ('run', 'supervise'):
        return run(sys.argv[3], unit=sys.argv[5] if sys.argv[4:5] == ['--scope'] else None,
                   fallback=sys.argv[5] if sys.argv[4:5] == ['--session-fallback'] else
                   'direct session-only invocation', supervisor=sys.argv[1] == 'supervise')
    if sys.argv[1] == 'reconcile-supervisor':
        print(json.dumps(reconcile_supervisor(float(sys.argv[3]))))
        return 0
    print(json.dumps(reconcile(float(sys.argv[3]), abandoned='--abandoned' in sys.argv[4:])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
