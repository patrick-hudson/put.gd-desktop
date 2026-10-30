#!/usr/bin/env python3
"""Bounded disposable-guest apt supervisor. Staged root-owned in each disposable overlay."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import stat
import subprocess
import time


def output_file(path):
    # Never truncate before checking: the lab user can plant links or special
    # files in this directory while the supervisor runs as root.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.geteuid():
            raise ValueError('Output must be an owned single-link regular file')
        # The invoking lab user collects these root-owned artifacts over SSH;
        # the containing pinned directory remains private (0700).
        os.fchmod(fd, 0o644)
        os.ftruncate(fd, 0)
        stream = os.fdopen(fd, 'w')
    except BaseException:
        os.close(fd)
        raise
    return stream


def save(path, value):
    temporary = path.with_suffix('.tmp')
    with output_file(temporary) as stream:
        stream.write(json.dumps(value, indent=2) + '\n')
    # rename replaces a planted destination link; it never follows it.
    temporary.replace(path)


def enter_directory(path, owner):
    # Pin every component without resolving symlinks. Subsequent relative IO
    # stays in this inode even if the lab user renames/replaces its parent.
    path = Path(os.path.abspath(path))
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for component in path.parts[1:]:
            child = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            os.close(fd)
            fd = child
        info = os.fstat(fd)
        if info.st_uid != owner or info.st_mode & 0o077:
            raise ValueError('Install directory must be private and owned by the invoking user')
        os.fchdir(fd)
    finally:
        os.close(fd)


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
        with output_file(directory / 'apt.log') as log:
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
    enter_directory(args.directory, int(os.environ.get('SUDO_UID', os.getuid())))
    directory = Path('.')
    # This fixed helper is the only newly privileged entrypoint; it cannot execute
    # caller-selected commands. The guest already permits arbitrary DEB installs.
    supervise([['apt-get', 'update'], ['apt-get', 'install', '--reinstall', '-y',
               './package.deb']], directory, args.timeout, args.recovery)


if __name__ == '__main__':
    main()
