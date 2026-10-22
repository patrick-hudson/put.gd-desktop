#!/usr/bin/env python3
"""The weekly builder stress lane: fetch nightly, build it, stress it, report its flaky tests.

tools/lab/install-stress-weekly.sh installs this script on k72 as the systemd
user service putgd-stress-weekly.service, started by putgd-stress-weekly.timer
on Sundays at 03:00 local time. One run:

1. Guards. The week is skipped, with the reason logged, when the builder's
   desktop lock (tools/lab/desktop_lock.py, ~/putgd-lab/locks) is held for
   quiet, A/V or performance work, or when another weekly run holds
   ~/.local/state/putgd-stress/weekly.lock. A running `ctest` or
   stress_suite.py process (any owner, matched by its exact process name or
   script, never killed) or a load above the CPU count makes it wait up to
   --wait-minutes, then skip.
2. It fetches origin/REF (nightly) into its own tree, ~/work/put.gd-stress,
   with build/sdk and build/dependencies linked to ~/work/put.gd/build.
3. When the disk lacks room (docs/lab-storage.md: a 10 GiB reserve plus the
   build), it removes the tree's previous build/dev first.
4. It configures with -DPUTGD_TEST_TIMEOUT_SCALE=3 and builds at nice 10, -j3.
5. tools/ci/stress_suite.py runs the integration tests --iterations times with
   --hogs busy loops into ~/work/flake-stress/weekly-DATE/ (ledger.json,
   summary.md, the iterations' JUnit and logs, weekly.log, weekly.json).
6. tools/ci/flake_ledger.py comments on each failed test's `flaky-test`
   issue, with the token in ~/.config/putgd-stress/github-token (mode 600,
   created by Patrick). The token goes only into that one child's GH_TOKEN;
   it is never printed, logged or copied. Without the file it logs why and
   keeps the local results.
7. It keeps the newest --keep weeks of weekly-* directories that it created
   and deletes only older ones of those.

Every child gets a headless environment (HEADLESS below): the desktop
session's DISPLAY, WAYLAND_DISPLAY and XDG_SESSION_* never reach the tests.
It stops only what it started (systemd stops the service's own cgroup) and
deletes only its tree's build/dev and its own weekly-* directories.

  stress_weekly.py [--dry-run] [--ref nightly] [--lock-dir DIR] [--keep 8] ...

--dry-run runs the guards and prints the plan without fetching, building,
running or writing anything. PUTGD_STRESS_REF and PUTGD_STRESS_LOCK_DIR set
--ref and --lock-dir (for `systemctl --user set-environment`).
"""
import argparse
import datetime
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

HOME = Path.home()
REPO = 'patrick-hudson/put.gd-desktop'
REPO_URL = f'https://github.com/{REPO}.git'
TREE_MARKER = 'putgd-stress-tree'  # inside the tree's .git, so `git clean` keeps it
RUN_MARKER = '.putgd-stress-weekly'
RUN_NAME = re.compile(r'^weekly-(\d{4}-\d{2}-\d{2})(?:-(\d+))?$')
QUIET = re.compile(r'quiet|record|\ba/?v\b|audio|video|perf|latency|benchmark|memory|timing|profil', re.IGNORECASE)
GIB = 1 << 30
RESERVE = 10 * GIB  # docs/lab-storage.md host reserve
INCREMENTAL = 2 * GIB  # headroom for rebuilding over an existing build/dev
DEFAULT_BUILD = 4 * GIB  # until a build has been measured
MIN_INODES = 1024
# The children get only these variables. A desktop session (Plasma, GNOME) imports DISPLAY, WAYLAND_DISPLAY,
# XAUTHORITY and the XDG_SESSION_* set into the systemd user manager; tests inheriting them reach the live
# desktop instead of their own Xvfb and private buses (the first k72 run, 2026-10-21, timed out five tests).
HEADLESS = ('HOME', 'USER', 'LOGNAME', 'SHELL', 'PATH', 'LANG', 'LANGUAGE', 'TZ', 'XDG_RUNTIME_DIR',
            'DBUS_SESSION_BUS_ADDRESS', 'SSL_CERT_FILE', 'SSL_CERT_DIR')


class Skip(Exception):
    """This week's run is skipped; the message says why."""


class Log:
    def __init__(self):
        self.lines, self.file = [], None

    def __call__(self, message):
        for line in str(message).splitlines() or ['']:
            stamped = f'{datetime.datetime.now().astimezone().isoformat(timespec="seconds")} {line}'
            print(line, flush=True)
            if self.file:
                self.file.write(stamped + '\n')
                self.file.flush()
            else:
                self.lines.append(stamped)

    def attach(self, path):
        self.file = path.open('a')
        self.file.write(''.join(line + '\n' for line in self.lines))
        self.file.flush()

    def close(self):
        if self.file:
            self.file.close()
            self.file = None


def load_module(name, candidates):
    for path in candidates:
        if path.is_file():
            spec = importlib.util.spec_from_file_location(name, path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module, path
    raise Skip(f'{name}.py not found in ' + ', '.join(map(str, candidates)))


def desktop_lock_module(tree):
    here = Path(__file__).resolve().parent
    return load_module('desktop_lock', [tree / 'tools/lab/desktop_lock.py', here / 'desktop_lock.py',
                                        here.parent / 'lab/desktop_lock.py'])


def lock_verdict(status, host):
    """Return (skip reason or None, description) for a desktop_lock.py status reply."""
    state = status.get('state')
    lock = status.get('lock') or {}
    who = f"{lock.get('owner', '?')} ({lock.get('session', '?')})"
    purpose = lock.get('purpose', '')
    if state == 'free':
        return None, f'{host} desktop lock: free'
    if state == 'expired':
        return None, f'{host} desktop lock: expired lock of {who} for "{purpose}", ignored'
    if state == 'invalid':
        if status.get('breakable'):
            return None, f'{host} desktop lock: unreadable and over 15 minutes old, ignored'
        return f'{host} desktop lock is unreadable ({status.get("error")}) and recent', ''
    if QUIET.search(purpose):
        return (f'{host} desktop lock is {state} by {who} for "{purpose}": quiet, A/V or performance work', '')
    return None, f'{host} desktop lock: {state} by {who} for "{purpose}"; not quiet work, running headless'


def check_desktop_lock(tree, lock_dir, host, log):
    module, path = desktop_lock_module(tree)
    status = module.Desktop(lock_dir, host).status()
    reason, description = lock_verdict(status, host)
    if reason:
        raise Skip(reason)
    log(f'{description} (read with {path})')


def process_args(pid):
    return [part.decode(errors='replace') for part in Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0') if part]


def suite_kind(comm, args):
    """'ctest' or 'stress_suite.py' for a full-suite process, else None. Exact names, not patterns."""
    if comm == 'ctest':
        return 'ctest'
    if args and Path(args[0]).name.startswith('python'):
        script = next((arg for arg in args[1:] if not arg.startswith('-')), '')
        if Path(script).name == 'stress_suite.py':
            return 'stress_suite.py'
    return None


def other_suites(own=None):
    own = own if own is not None else {os.getpid()}
    found = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit() or int(entry.name) in own:
            continue
        try:
            comm = (entry / 'comm').read_text().strip()
            kind = suite_kind(comm, process_args(entry.name))
            uid = entry.stat().st_uid
        except OSError:
            continue
        if kind:
            found.append({'pid': int(entry.name), 'uid': uid, 'kind': kind})
    return sorted(found, key=lambda item: item['pid'])


def busy_reason():
    suites = other_suites()
    if suites:
        return 'another suite is running: ' + ', '.join(f"{item['kind']} pid {item['pid']} uid {item['uid']}"
                                                         for item in suites)
    load, cpus = os.getloadavg()[0], os.cpu_count() or 1
    if load > cpus:
        return f'load {load:.2f} is above the {cpus} CPUs'
    return None


def wait_until_idle(log, minutes, dry_run, poll=60):
    deadline = time.monotonic() + minutes * 60
    reason = busy_reason()
    if reason and dry_run:
        raise Skip(f'{reason} (a real run would wait up to {minutes} min)')
    while reason:
        if time.monotonic() >= deadline:
            raise Skip(f'{reason}, still after waiting {minutes} min')
        log(f'waiting: {reason}')
        time.sleep(poll)
        reason = busy_reason()
    log(f'no other ctest or stress suite running; load {os.getloadavg()[0]:.2f} on {os.cpu_count()} CPUs')


def free_space(path):
    info = os.statvfs(path)
    return info.f_bavail * info.f_frsize, (info.f_favail if info.f_files else None)


def tree_bytes(path):
    total = 0
    for root, directories, files in os.walk(path):
        for name in directories + files:
            try:
                total += os.lstat(os.path.join(root, name)).st_blocks * 512
            except OSError:
                pass
    return total


def gib(value):
    return f'{value / GIB:.1f} GiB'


def ensure_disk(tree, state_dir, log, dry_run, measure=free_space):
    """Room for the build: 10 GiB reserve plus the build, removing the previous build/dev if needed."""
    build = tree / 'build/dev'
    try:
        estimate = int(json.loads((state_dir / 'build-size.json').read_text())['bytes'])
    except (OSError, ValueError, KeyError):
        estimate = DEFAULT_BUILD
    anchor = tree if tree.exists() else tree.parent
    free, inodes = measure(anchor)
    if inodes is not None and inodes <= MIN_INODES:
        raise Skip(f'only {inodes} free inodes on {anchor}')
    if build.is_dir() and not build.is_symlink():
        if free >= RESERVE + INCREMENTAL:
            log(f'disk: {gib(free)} free, keeping the previous build/dev for an incremental build')
            return
        if dry_run:
            log(f'disk: {gib(free)} free; would remove the previous {build} ({gib(estimate)} measured)')
            free += estimate
        else:
            log(f'disk: {gib(free)} free, under {gib(RESERVE + INCREMENTAL)}; removing the previous {build}')
            shutil.rmtree(build)
            free, inodes = measure(anchor)
    if free < RESERVE + estimate:
        raise Skip(f'disk: {gib(free)} free on {anchor}, needs {gib(RESERVE)} reserve plus a {gib(estimate)} build')
    log(f'disk: {gib(free)} free, enough for a {gib(estimate)} build plus the {gib(RESERVE)} reserve')


def headless_environment(source=None):
    """The environment for every child: no display, desktop session or tokens, like an SSH login."""
    source = os.environ if source is None else source
    return {key: value for key, value in source.items() if key in HEADLESS or key.startswith('LC_')}


def run(command, log, output=None, **options):
    """Run a command; its output goes to the log, or to OUTPUT (a path) when given. Returns the exit code."""
    log('$ ' + ' '.join(map(str, command)))
    options.setdefault('env', headless_environment())
    if output:
        with output.open('a') as stream:
            return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  **options).returncode
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                               text=True, **options)
    for line in process.stdout:
        log(line.rstrip('\n'))
    return process.wait()


def git(tree, log, *args):
    if run(['git', '-C', str(tree), *args], log):
        raise RuntimeError(f'git {args[0]} failed')


def update_tree(tree, ref, sdk_from, log):
    marker = tree / '.git' / TREE_MARKER
    if not tree.exists():
        if run(['git', 'clone', '--quiet', '--no-checkout', REPO_URL, str(tree)], log):
            raise RuntimeError('git clone failed')
        marker.write_text('Created by tools/ci/stress_weekly.py; only the weekly stress lane uses this tree.\n')
    elif not marker.is_file():
        raise Skip(f'{tree} exists but this script did not create it; not touching it')
    git(tree, log, 'fetch', '--quiet', '--prune', '--no-tags', 'origin', f'+refs/heads/{ref}:refs/remotes/origin/{ref}')
    git(tree, log, 'checkout', '--quiet', '--force', '--detach', f'refs/remotes/origin/{ref}')
    git(tree, log, 'clean', '-ffdq')  # untracked files only; the ignored build/ stays
    commit = subprocess.run(['git', '-C', str(tree), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                            check=True, env=headless_environment()).stdout.strip()
    for name in ('sdk', 'dependencies'):
        link, target = tree / 'build' / name, sdk_from / name
        if not target.is_dir():
            raise Skip(f'{target} is missing; provision the SDK on this builder first (docs/build.md)')
        if link.is_symlink():
            if Path(os.readlink(link)) != target:
                raise Skip(f'{link} points to {os.readlink(link)}, not {target}')
        elif link.exists():
            raise Skip(f'{link} is a real directory, not a link to {target}')
        else:
            link.parent.mkdir(exist_ok=True)
            link.symlink_to(target)
    log(f'tree {tree} at origin/{ref} {commit}')
    return commit


def read_token(path):
    """Return (token, None) or (None, why). Refuses files that aren't private regular files of this user."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return None, f'{path} is missing'
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
        return None, f'{path} is not a regular file owned by this user; not reading it'
    if info.st_mode & 0o077:
        return None, f'{path} has mode {stat.S_IMODE(info.st_mode):03o}, not 600; not reading it'
    token = Path(path).read_text().strip()
    if not token or not token.isprintable() or any(character.isspace() for character in token):
        return None, f'{path} does not hold a single token'
    return token, None


def update_tracker(tree, ledger, token_file, repo, log):
    token, why = read_token(token_file)
    if token is None:
        log(f'GitHub token {why}: not commenting on flaky-test issues; the results stay in {ledger.parent}')
        return f'skipped: token {why}'
    environment = dict(headless_environment(), GH_TOKEN=token)
    result = subprocess.run([sys.executable, '-I', str(tree / 'tools/ci/flake_ledger.py'), 'issues', str(ledger),
                             '--repo', repo], env=environment, capture_output=True, text=True, timeout=900,
                            stdin=subprocess.DEVNULL)
    for line in (result.stdout + result.stderr).splitlines():
        log(line.replace(token, '[token]'))
    return 'updated' if result.returncode == 0 else f'failed: flake_ledger.py exited {result.returncode}'


def new_run_dir(results, day):
    results.mkdir(parents=True, exist_ok=True)
    for number in range(1, 100):
        directory = results / (f'weekly-{day}' + (f'-{number}' if number > 1 else ''))
        try:
            directory.mkdir()
        except FileExistsError:
            continue
        (directory / RUN_MARKER).write_text('Created by tools/ci/stress_weekly.py; pruned after --keep weeks.\n')
        return directory
    raise RuntimeError(f'no free weekly-{day} directory name in {results}')


def prune(results, keep, log, current=None):
    """Delete this script's weekly-* directories older than the newest KEEP dates. Returns the deleted paths."""
    if not results.is_dir():
        return []
    runs = []
    for directory in results.iterdir():
        match = RUN_NAME.match(directory.name)
        if match and directory.is_dir() and not directory.is_symlink() and (directory / RUN_MARKER).is_file():
            runs.append((match[1], directory))
    kept = set(sorted({day for day, _ in runs}, reverse=True)[:keep])
    deleted = []
    for day, directory in sorted(runs):
        if day not in kept and directory != current:
            shutil.rmtree(directory)
            deleted.append(directory)
            log(f'retention: removed {directory} (keeping the newest {keep} weeks)')
    return deleted


def suite_command(args, tree, out, label):
    return [sys.executable, '-I', str(tree / 'tools/ci/stress_suite.py'), '--source', str(tree), '--out', str(out),
            '--iterations', str(args.iterations), '--hogs', str(args.hogs), '--time-scale', str(args.time_scale),
            '--label', label, '--', '--preset', 'dev', '-L', 'integration', '--parallel', str(args.parallel),
            '--resource-spec-file', str(tree / 'tools/ci/ctest-resources.json')]


def parse(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('--dry-run', action='store_true', help='check the guards and print the plan only')
    parser.add_argument('--ref', default=os.environ.get('PUTGD_STRESS_REF') or 'nightly', help='branch to test')
    parser.add_argument('--host', default='k72', help='desktop lock host name')
    parser.add_argument('--lock-dir', type=Path,
                        default=Path(os.environ.get('PUTGD_STRESS_LOCK_DIR') or HOME / 'putgd-lab/locks'))
    parser.add_argument('--tree', type=Path, default=HOME / 'work/put.gd-stress')
    parser.add_argument('--sdk-from', type=Path, default=HOME / 'work/put.gd/build',
                        help='directory holding the sdk and dependencies to link')
    parser.add_argument('--results', type=Path, default=HOME / 'work/flake-stress')
    parser.add_argument('--state-dir', type=Path, default=HOME / '.local/state/putgd-stress')
    parser.add_argument('--token-file', type=Path, default=HOME / '.config/putgd-stress/github-token')
    parser.add_argument('--repo', default=REPO)
    parser.add_argument('--iterations', type=int, default=10)
    parser.add_argument('--hogs', type=int, default=4)
    parser.add_argument('--time-scale', type=int, default=3)
    parser.add_argument('--parallel', type=int, default=12)
    parser.add_argument('--jobs', type=int, default=3)
    parser.add_argument('--keep', type=int, default=8, help='weeks of results to keep')
    parser.add_argument('--wait-minutes', type=int, default=60)
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9._/-]+', args.ref) or '..' in args.ref:
        parser.error(f'--ref {args.ref!r} is not a plain branch name')
    if args.keep < 1:
        parser.error('--keep must be at least 1')
    return args


def main(argv=None):
    args = parse(argv)
    log = Log()
    log(f'putgd weekly stress lane on {socket.gethostname()}' + (' (dry run)' if args.dry_run else '')
        + f': origin/{args.ref}, {args.iterations} runs, {args.hogs} busy loops, scale {args.time_scale}')
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (args.state_dir / 'weekly.lock').open('a') as guard:
        try:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            log(f'skipped: another weekly run holds {args.state_dir / "weekly.lock"}')
            return 0
        if args.dry_run:
            return dry_run(args, log)
        try:
            return weekly_run(args, log)
        finally:
            log.close()


def weekly_run(args, log):
    def on_signal(number, _frame):
        raise KeyboardInterrupt(f'signal {number}')
    signal.signal(signal.SIGTERM, on_signal)

    out = new_run_dir(args.results, datetime.date.today().isoformat())
    log.attach(out / 'weekly.log')
    status = {'status': 'running', 'host': socket.gethostname(), 'ref': args.ref, 'results': str(out),
              'started': datetime.datetime.now().astimezone().isoformat(timespec='seconds')}
    code = 0
    try:
        check_desktop_lock(args.tree, args.lock_dir, args.host, log)
        wait_until_idle(log, args.wait_minutes, False)
        status['commit'] = update_tree(args.tree, args.ref, args.sdk_from, log)
        ensure_disk(args.tree, args.state_dir, log, False)
        clock = time.monotonic()
        if run(['cmake', '--preset', 'dev', '-DPUTGD_TEST_TIMEOUT_SCALE=3'], log, out / 'build.log', cwd=args.tree) or \
                run(['nice', '-n', '10', 'cmake', '--build', '--preset', 'dev', '-j', str(args.jobs)], log,
                    out / 'build.log', cwd=args.tree):
            tail = (out / 'build.log').read_text(errors='replace').splitlines()[-40:]
            log('build failed; the last lines of build.log:\n' + '\n'.join(tail))
            status.update(status='build-failed')
            return 1
        status['buildSeconds'] = round(time.monotonic() - clock)
        build_size = tree_bytes(args.tree / 'build/dev')
        (args.state_dir / 'build-size.json').write_text(json.dumps({'bytes': build_size}) + '\n')
        log(f'built in {status["buildSeconds"]} s; build/dev is {gib(build_size)}')
        check_desktop_lock(args.tree, args.lock_dir, args.host, log)  # a quiet slot may have started meanwhile
        wait_until_idle(log, args.wait_minutes, False)
        clock = time.monotonic()
        status['stressExit'] = run(suite_command(args, args.tree, out, out.name), log, cwd=args.tree)
        status['stressSeconds'] = round(time.monotonic() - clock)
        if not (out / 'ledger.json').is_file():
            status.update(status='stress-failed')
            return 1
        status['tracker'] = update_tracker(args.tree, out / 'ledger.json', args.token_file, args.repo, log)
        status['status'] = 'completed' if status['stressExit'] == 0 else 'interrupted'
        log(f'results: {out}/ledger.json and {out}/summary.md')
    except Skip as skipped:
        status.update(status='skipped', reason=str(skipped))
        log(f'skipped this week: {skipped}')
    except KeyboardInterrupt as stopped:
        status.update(status='interrupted', reason=str(stopped))
        log(f'interrupted: {stopped}')
        code = 1
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        status.update(status='error', reason=str(error))
        log(f'error: {error}')
        code = 1
    finally:
        status['ended'] = datetime.datetime.now().astimezone().isoformat(timespec='seconds')
        (out / 'weekly.json').write_text(json.dumps(status, indent=1) + '\n')
        prune(args.results, args.keep, log, current=out)
    return code


def dry_run(args, log):
    try:
        check_desktop_lock(args.tree, args.lock_dir, args.host, log)
        wait_until_idle(log, args.wait_minutes, True)
        if args.tree.exists() and not (args.tree / '.git' / TREE_MARKER).is_file():
            raise Skip(f'{args.tree} exists but this script did not create it')
        log(f'would fetch origin/{args.ref} into {args.tree}' + ('' if args.tree.exists() else ' (new clone)')
            + f', linking build/sdk and build/dependencies to {args.sdk_from}')
        ensure_disk(args.tree, args.state_dir, log, True)
        log('would run: cmake --preset dev -DPUTGD_TEST_TIMEOUT_SCALE=3')
        log(f'would run: nice -n 10 cmake --build --preset dev -j {args.jobs}')
        day = datetime.date.today().isoformat()
        log('would run: ' + ' '.join(suite_command(args, args.tree, args.results / f'weekly-{day}', f'weekly-{day}')))
        _token, why = read_token(args.token_file)
        log(f'GitHub token {why}: would not comment on flaky-test issues' if why else
            f'GitHub token present ({args.token_file}, mode 600): would comment on flaky-test issues')
        log(f'would keep the newest {args.keep} weeks of weekly-* results in {args.results}')
    except Skip as skipped:
        log(f'dry run: a real run would skip this week: {skipped}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
