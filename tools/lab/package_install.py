#!/usr/bin/env python3
"""Bounded disposable-guest apt supervisor. Installed root-owned by bootstrap."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import time


def save(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


class OwnedProcesses:
    """Track only descendants, holding pidfds so PID reuse cannot target a stranger."""
    def __init__(self):
        self.handles = {}
        self.identities = []

    def discover(self, parent=None):
        parent = os.getpid() if parent is None else parent
        try:
            children = (Path('/proc') / str(parent) / 'task' / str(parent) / 'children').read_text().split()
        except FileNotFoundError:
            return
        for child in map(int, children):
            handle = None
            try:
                # Validate ancestry after opening the pidfd as well: the PID
                # from the children file may have exited and been reused.
                handle = os.pidfd_open(child)
                stat = (Path('/proc') / str(child) / 'stat').read_text().rsplit(')', 1)[1].split()
                if int(stat[1]) != parent or select.select([handle], [], [], 0)[0]:
                    continue
                identity = {'pid': child, 'startTicks': int(stat[19])}
                if child in self.handles and identity in self.identities:
                    self.discover(child)
                    continue
                if child in self.handles:
                    os.close(self.handles.pop(child))
                self.handles[child] = handle
                handle = None
                self.identities.append(identity)
            except (ProcessLookupError, FileNotFoundError):
                continue
            finally:
                if handle is not None:
                    os.close(handle)
            self.discover(child)

    def alive(self):
        return [pid for pid, handle in self.handles.items() if not select.select([handle], [], [], 0)[0]]

    def send(self, pid, sig):
        try:
            signal.pidfd_send_signal(self.handles[pid], sig)
        except ProcessLookupError:
            pass

    def abort(self):
        # Freeze parents before inspecting their children, preventing another fork
        # between the final inventory and kill. No name, uid or global group matching.
        stopped = set()
        while True:
            self.discover()
            pending = [pid for pid in self.alive() if pid not in stopped]
            if not pending:
                break
            for pid in pending:
                self.send(pid, signal.SIGSTOP)
                stopped.add(pid)
        for pid in stopped:
            self.send(pid, signal.SIGKILL)
        return sorted(stopped)

    def close(self):
        for handle in self.handles.values():
            os.close(handle)


def supervise(commands, directory, timeout, recovery, package='putgd'):
    # Reparent double-forked descendants here instead of losing ownership to init.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'Cannot become package subreaper')
    started = time.monotonic()
    owned = OwnedProcesses()
    report = {'schemaVersion': 1, 'timeoutSeconds': timeout, 'recoverySeconds': recovery,
              'timedOut': False, 'aborted': False, 'ownedProcesses': [], 'exitCodes': []}
    try:
        save(directory / 'progress.json', report)
        with (directory / 'apt.log').open('w') as log:
            for arguments in commands:
                # Never start the next apt command after the initial budget expires.
                if report['timedOut'] or time.monotonic() - started >= timeout:
                    report['timedOut'] = True
                    break
                process = subprocess.Popen(arguments, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                                           env={**os.environ, 'DEBIAN_FRONTEND': 'noninteractive'})
                while True:
                    owned.discover()
                    if time.monotonic() - started >= timeout:
                        report['timedOut'] = True
                    report['ownedProcesses'] = owned.identities
                    report['elapsedSeconds'] = time.monotonic() - started
                    save(directory / 'progress.json', report)
                    # Wait for descendants too, even if the original apt exited.
                    status = process.poll()
                    if status is not None and not owned.alive():
                        break
                    if time.monotonic() - started >= timeout + recovery:
                        report['aborted'] = True
                        report['stoppedPids'] = owned.abort()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            pass  # e.g. uninterruptible IO; report exact surviving identities.
                        deadline = time.monotonic() + 10
                        while owned.alive() and time.monotonic() < deadline:
                            time.sleep(.1)
                        break
                    time.sleep(.2)
                report['exitCodes'].append(process.poll())
                if report['aborted'] or process.returncode:
                    break
    except BaseException as error:
        report['error'] = type(error).__name__
        report['aborted'] = True
        report['stoppedPids'] = owned.abort()
    finally:
        report['remainingPids'] = owned.alive()
        # Always retain recovery state, including failures/aborts. Never retry apt.
        for key, args in (('dpkgStatus', ['dpkg', '-s', package]),
                          ('dpkgAudit', ['dpkg', '--audit'])):
            try:
                result = subprocess.run(args, capture_output=True, text=True, timeout=30)
                report[key] = {'exitCode': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
            except subprocess.TimeoutExpired:
                report[key] = {'error': 'query timed out'}
            except OSError as error:
                report[key] = {'error': type(error).__name__}
        report['ok'] = (not report['aborted'] and not report['remainingPids']
                        and report['exitCodes'] == [0] * len(commands)
                        and report['dpkgStatus'].get('exitCode') == 0
                        and 'Status: install ok installed' in report['dpkgStatus'].get('stdout', '').splitlines()
                        and report['dpkgAudit'].get('exitCode') == 0
                        and not report['dpkgAudit'].get('stdout'))
        report['elapsedSeconds'] = time.monotonic() - started
        save(directory / 'result.json', report)
        owned.close()
        while True:
            try:
                if os.waitpid(-1, os.WNOHANG)[0] == 0:
                    break
            except ChildProcessError:
                break

    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('timeout', type=int)
    parser.add_argument('recovery', type=int)
    args = parser.parse_args()
    if args.timeout <= 0 or args.recovery <= 0:
        parser.error('Timeouts must be positive seconds')
    directory = args.directory.resolve(strict=True)
    # This fixed helper is the only newly privileged entrypoint; it cannot execute
    # caller-selected commands. The guest already permits arbitrary DEB installs.
    supervise([['apt-get', 'update'], ['apt-get', 'install', '--reinstall', '-y',
               str(directory / 'package.deb')]], directory, args.timeout, args.recovery)


if __name__ == '__main__':
    main()
