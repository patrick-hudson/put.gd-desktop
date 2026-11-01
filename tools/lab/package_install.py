#!/usr/bin/env python3
"""Bounded disposable-guest apt supervisor. Staged root-owned in each disposable overlay."""
import argparse
import ctypes
import fcntl
import json
import os
from pathlib import Path
import re
import select
import signal
import stat
import subprocess
import time


APT_LOCK_TIMEOUT = 120


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


def admit(token, owner, wait=10):
    """Record this supervisor's start while its job still holds install-active.

    Staging recovery releases install-active under the same install-guard flock
    and only while progress.json is absent. Taking the guard, checking the token
    and writing progress.json before releasing it means either recovery refuses
    or this supervisor never starts: it can never run without the install lock.
    Runs inside the pinned job directory; every lookup is relative and refuses
    links, as the lab user owns the parent.
    """
    if not re.fullmatch(r'install-[0-9a-f]{32}', token):
        raise ValueError('Install directory must be named by its install job')
    parent = os.open('..', os.O_RDONLY | os.O_DIRECTORY)
    try:
        here, named = os.stat('.'), os.stat(token, dir_fd=parent, follow_symlinks=False)
        if (named.st_dev, named.st_ino) != (here.st_dev, here.st_ino):
            raise ValueError('Install directory is no longer its job directory')
        guard = os.open('install-guard', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
        try:
            info = os.fstat(guard)
            if not stat.S_ISREG(info.st_mode) or info.st_uid != owner:
                raise ValueError('install-guard must be a regular file owned by the invoking user')
            deadline = time.monotonic() + wait
            while True:
                try:
                    fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('install-guard stayed busy') from None
                    time.sleep(.05)
            active = os.open('install-active', os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent)
            try:
                job = os.open('job', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=active)
                try:
                    if not stat.S_ISREG(os.fstat(job).st_mode):
                        raise ValueError('install-active/job must be a regular file')
                    held = os.read(job, 128).decode('ascii', 'replace').strip()
                finally:
                    os.close(job)
            finally:
                os.close(active)
            if held != token:
                raise PermissionError(f'{token} does not hold install-active')
            save(Path('progress.json'), {'schemaVersion': 1, 'admittedJob': token})
        finally:
            os.close(guard)  # Releases the flock only after progress.json exists.
    finally:
        os.close(parent)


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


def become_subreaper():
    # Reparent double-forked descendants here instead of losing ownership to init.
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'Cannot become package subreaper')


def verify_scope(required=False):
    # sudo must preserve the dedicated system scope. Refuse before admission if a
    # guest PAM policy moved root elsewhere; never migrate a root process into
    # a caller-selected cgroup. Legacy/direct callers retain session tracking.
    try:
        identity = json.loads(Path('supervisor-process.json').read_text())
    except FileNotFoundError:
        if required:
            raise ValueError('Missing supervisor scope identity') from None
        return
    if identity.get('tracking', 'session') == 'session' and 'cgroup' not in identity:
        if required:
            raise ValueError('Root supervisor did not enter its dedicated cgroup')
        return
    group = identity['cgroup']
    unit = 'putgd-supervisor-' + Path.cwd().name + '.scope'
    if (identity.get('tracking') != 'cgroup-v2' or group['unit'] != unit
            or (required and group['path'] != '/system.slice/' + unit)
            or Path(group['path']).name != unit
            or '0::' + group['path'] not in Path('/proc/self/cgroup').read_text().splitlines()):
        raise ValueError('Root supervisor left its dedicated cgroup')


def supervise(commands, directory, timeout, recovery, package='putgd'):
    started = time.monotonic()
    owned = OwnedProcesses()
    report = {'schemaVersion': 1, 'timeoutSeconds': timeout, 'recoverySeconds': recovery,
              'timedOut': False, 'aborted': False, 'ownedProcesses': [], 'exitCodes': []}
    try:
        # Inside the guarded path: after admission every failure leaves a
        # terminal result.json, never a progress.json without one.
        become_subreaper()
        save(directory / 'progress.json', report)
        with output_file(directory / 'apt.log') as log:
            pending = iter(commands)
            arguments = next(pending, None)
            update_lock_deadline = min(started + timeout, time.monotonic() + APT_LOCK_TIMEOUT)
            while arguments is not None:
                # Never start the next apt command after the initial budget expires.
                if report['timedOut'] or time.monotonic() - started >= timeout:
                    report['timedOut'] = True
                    break
                log_start = log.tell()
                process = subprocess.Popen(arguments, stdout=log, stderr=log, stdin=subprocess.DEVNULL,
                                           env={**os.environ, 'DEBIAN_FRONTEND': 'noninteractive', 'LC_ALL': 'C'})
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
                        # A parent can exit between the /proc children snapshot
                        # and pidfd validation, adopting a still-live child here
                        # that discover() has not seen yet. Only ECHILD proves
                        # the subreaper's entire package tree has finished. A
                        # zero waitpid result keeps discovery/recovery running.
                        try:
                            while os.waitpid(-1, os.WNOHANG)[0]:
                                pass
                        except ChildProcessError:
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
                # DPkg::Lock::Timeout does not cover update's lists lock. Retry
                # only its early lock refusal, never a partial install or other
                # apt failure, within both the lock wait and shared apt budget.
                if (not report['aborted'] and not report['timedOut'] and process.returncode == 100
                        and arguments[0] == 'apt-get' and arguments[-1] == 'update'):
                    # Read our pinned log inode, never a caller-replaced path/link.
                    with open(f'/proc/self/fd/{log.fileno()}', 'rb') as output:
                        output.seek(log_start)
                        errors = [line for line in output.read().decode(errors='replace').splitlines()
                                  if line.startswith('E: ')]
                    lock_error = any(line.startswith('E: Could not get lock /var/lib/apt/lists/lock')
                                     and ('held by process ' in line or
                                          'Resource temporarily unavailable' in line) for line in errors)
                    only_lock_errors = all(line.startswith(('E: Could not get lock /var/lib/apt/lists/lock',
                                                           'E: Unable to lock directory /var/lib/apt/lists/'))
                                           for line in errors)
                    remaining = update_lock_deadline - time.monotonic()
                    if lock_error and only_lock_errors and remaining > 0:
                        time.sleep(min(1, remaining))
                        if time.monotonic() < update_lock_deadline:
                            report['updateLockRetries'] = report.get('updateLockRetries', 0) + 1
                            continue
                report['exitCodes'].append(process.poll())
                if report['aborted'] or process.returncode:
                    break
                arguments = next(pending, None)
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
    parser.add_argument('--require-scope', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0 or args.recovery <= 0:
        parser.error('Timeouts must be positive seconds')
    owner = int(os.environ.get('SUDO_UID', os.getuid()))
    enter_directory(args.directory, owner)
    try:
        # Fail before admission where possible, leaving the job recoverable.
        become_subreaper()
        verify_scope(required=args.require_scope)
        admit(args.directory.name, owner)
    except (OSError, ValueError, KeyError, TypeError) as error:
        # No progress.json or result.json: the job (if it still holds the lock)
        # stays recoverable through lab.py recover-staging.
        raise SystemExit(f'Install supervisor refused to start: {error}') from None
    directory = Path('.')
    # This fixed helper is the only newly privileged entrypoint; it cannot execute
    # caller-selected commands. The guest already permits arbitrary DEB installs.
    supervise([['apt-get', '-o', f'DPkg::Lock::Timeout={APT_LOCK_TIMEOUT}', 'update'],
               ['apt-get', '-o', f'DPkg::Lock::Timeout={APT_LOCK_TIMEOUT}', 'install', '--reinstall', '-y',
               './package.deb']], directory, args.timeout, args.recovery)


if __name__ == '__main__':
    main()
