#!/usr/bin/env python3
"""Unprivileged lifetime fence for helper staging in a disposable overlay."""
import ctypes
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
    # nohup protects this process from the SSH session disappearing. A terminal
    # receipt requires ECHILD, including adopted sudo/apt/hook descendants.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):
        raise OSError(ctypes.get_errno(), 'Cannot become staging subreaper')
    with open('staging.lock', 'a') as fence:
        fcntl.flock(fence, fcntl.LOCK_EX)
        if Path('staging-cancelled').exists():
            return 125
        identity = {'pid': os.getpid(), 'startTicks': int(
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


def process_status():
    handle = None
    try:
        identity = json.loads(Path('staging-process.json').read_text())
        pid = identity['pid']
        if (type(pid) is not int or pid <= 0 or
                type(identity['startTicks']) is not int or identity['startTicks'] <= 0):
            return {'exactProcessAlive': None}
        try:
            # Python < 3.9 has no pidfd_open. /proc still lets us distinguish
            # this exact wrapper from a reused PID without ever signalling it.
            pidfd_open = getattr(os, 'pidfd_open', None)
            if pidfd_open is not None:
                handle = pidfd_open(pid)
            fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
            alive = int(fields[19]) == identity['startTicks'] and fields[0] not in ('Z', 'X')
            if handle is not None:
                alive = alive and not select.select([handle], [], [], 0)[0]
            return {'process': identity, 'exactProcessAlive': alive}
        except ProcessLookupError:
            return {'process': identity, 'exactProcessAlive': False}
        except FileNotFoundError:
            return {'process': identity, 'exactProcessAlive': False}
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return {'exactProcessAlive': None}
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
        if abandoned:
            # Explicit operator recovery only: the flock is free and the recorded
            # exact wrapper is gone. Unreadable/missing identity is not death proof.
            status = process_status()
            if status['exactProcessAlive'] is False:
                return {'finished': True, 'state': 'staging-abandoned', **status}
            return {'finished': False, 'state': 'staging-outcome-unknown', **status}
        if Path('staging-result.json').exists():
            result = json.loads(Path('staging-result.json').read_text())
            identity = json.loads(Path('staging-process.json').read_text())
            if (result.get('finished') is True and all(result.get(k) == identity[k]
                    for k in ('pid', 'startTicks'))):
                return {**result, 'state': 'staging-finished'}
        if not Path('staging-process.json').exists():
            return {'finished': True, 'state': 'staging-not-started'}
        # A dead wrapper is not proof that its privileged descendants stopped.
        return {'finished': False, 'state': 'staging-outcome-unknown', **process_status()}


def main():
    os.umask(0o077)
    os.chdir(sys.argv[2])
    if sys.argv[1] == 'run':
        return run(sys.argv[3])
    print(json.dumps(reconcile(float(sys.argv[3]), abandoned='--abandoned' in sys.argv[4:])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
