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
    try:
        identity = json.loads(Path('staging-process.json').read_text())
        pid = identity['pid']
        handle = os.pidfd_open(pid)
        try:
            ticks = int(Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19])
            alive = ticks == identity['startTicks'] and not select.select([handle], [], [], 0)[0]
            return {'process': identity, 'exactProcessAlive': alive}
        finally:
            os.close(handle)
    except (OSError, ValueError, KeyError, TypeError):
        return {'exactProcessAlive': None}


def reconcile(seconds):
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
    print(json.dumps(reconcile(float(sys.argv[3]))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
