#!/usr/bin/env python3
"""Unprivileged lifetime fence for helper staging in a disposable overlay."""
import ctypes
import errno
import fcntl
import json
import os
import select
from pathlib import Path
import subprocess
import sys
import time


def save(name, value):
    temporary = Path(name + '.tmp')
    temporary.write_text(json.dumps(value) + '\n')
    temporary.replace(name)


def run(script):
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
        save('staging-process.json', identity)
        process = subprocess.Popen(['sh', '-c', script], stdin=subprocess.DEVNULL)
        code = process.wait()
        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                break
        save('staging-result.json', {**identity, 'exitCode': code, 'finished': True})
        return code


# A wrapper that reached its terminal receipt also waited, as subreaper, for
# descendants that called setsid(). After the wrapper is killed, only the session
# scan remains, and it cannot see those: the in-tree hook and apt-get update with
# no sources start none, but a guest's own APT Pre-Invoke hook could.
ABANDONED_LIMIT = ('Abandonment proves only that no process remains in the staging session. A '
                   'descendant that called setsid() (for example a daemonizing guest APT Pre-Invoke '
                   'hook) left that session and is not checked, and the /proc scan is not atomic: '
                   'a member that keeps forking and exiting could evade both passes. Inspect the '
                   'guest process list if its hooks can daemonize.')


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
        return {**status, **session_status(identity['sessionId'])}
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
        while ((status['exactProcessAlive'] is True or status['sessionAlive'] is True)
               and time.monotonic() < deadline):
            time.sleep(.1)
            status = process_status()
        # A free flock or terminal receipt does not establish that a killed
        # wrapper's privileged descendants exited. Both paths require a complete
        # session scan and a verifiable, no-longer-live session leader.
        if status['exactProcessAlive'] is not False or status['sessionAlive'] is not False:
            return {'finished': False, 'state': 'staging-outcome-unknown', **status}
        if abandoned:
            return {'finished': True, 'state': 'staging-abandoned', **status, 'limit': ABANDONED_LIMIT}
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
    if sys.argv[1] == 'run':
        return run(sys.argv[3])
    print(json.dumps(reconcile(float(sys.argv[3]), abandoned='--abandoned' in sys.argv[4:])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
