#!/usr/bin/env python3
"""Unattended Tier 1 controller. Run on d69, never on the workstation.

  python3 tools/lab/nightly_desktops.py --now [vYY.MDD.N-nightly]
  python3 tools/lab/nightly_desktops.py vYY.MDD.N --stable-draft --draft-token-file PATH

Private configuration: ~/.config/putgd-nightly/config.json and github-token
(owned, mode 0600). config.json supplies repository, builders (ssh argv, root,
rows), and each row's golden, bootstrapKey, knownHosts, profile and package.
The user service calls this without --now: already published tags are skipped.
It first fast-forwards d69's checkout to origin/nightly, and the controller brings
every builder to that commit, so merged driver fixes need no deployment step.
The worker uses only existing lab launch/install/recipes/collection/retirement.
No compilation, map updates, per-night commits, or visual approval step.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
# The idle-memory cell runs first: a fresh guest, before any cell can leave residue.
FLOWS = dict(idle_memory='Idle memory', region_upload='Region', window_desktop='Window / desktop',
             recording_upload='Recording → upload', edit_upload='Edit → upload',
             link_browser='Link → browser', history_delete='History delete',
             auto_copy='Auto-copy', upload_clipboard='Upload clipboard',
             copy_image='Copy image', survives_exit='Survives exit',
             file_manager_upload='Right-click upload', window_recording='Window recording',
             tray_menu='Tray menu', copy_text='Copy text / redact',
             s3_region_upload='Live S3', s3_r2_region_upload='Live R2', proton_region_upload='Live Proton')
LOCAL = [name for name in FLOWS if not name.startswith(('s3_', 'proton_'))]
LIVE = [name for name in FLOWS if name not in LOCAL]
# A feature the acceptance matrix (docs/testing/acceptance-matrix.md) says a
# desktop does not support is reported as such (➖), never as red or not run.
# Only for what no user of that desktop can do: something a user can set up
# (Fedora GNOME's AppIndicator tray) is set up in the guest, and a check the lab
# can't drive stays not run (⚪). Keyed by (row desktop id, flow); the reason quotes the matrix.
UNSUPPORTED = {}
STATUS_MARK = {'green': '🟢', 'red': '🔴', 'not_run': '⚪', 'unsupported': '➖'}
STABLE_TAG = re.compile(r'v\d{2}\.\d{3,4}\.\d+')
TAG = re.compile(r'v\d{2}\.\d{3,4}\.\d+-nightly')
HOSTS = ('m74', 'd69', 'k72', 'aurora-amd', 'f44')
MAX_ROWS = 2  # Per builder, run in turn; each row is about half an hour.
# The worker budget includes checkout verification, await_host and setup. Leave
# fifteen minutes for retirement/lease cleanup before the controller's limit.
HOST_TIMEOUT = 18000
WORKER_SECONDS = HOST_TIMEOUT - 900
TERMINATE_GRACE = 300
ATTEMPT_TIMEOUT = 1800  # Recipes allow 300–720s, plus fixture/live setup/cleanup.


# Reasons a row never starts that lie with its builder, not with the build (preflight, await_host,
# run_host). A row stopped by one of these on two nights running is a host problem someone has to
# fix: on 11-22 and 11-23 lab scratch in k72's and f44's RAM-backed /tmp kept their rows from
# running at all, and nothing said so (#1978).
HOST_REASONS = ('RAM below guest requirement', 'load at or above core count', 'disk below sparse overlay budget',
                'builder KVM unavailable', 'golden missing', 'golden copy awaiting', 'reserved nightly guest port',
                'desktop lease held', 'proton lease held', 'nightly guest slot held', 'SSH unreachable',
                # The same occupied slot or lease, met by stale-guest recovery before await_host.
                'nightly guest slot belongs to a live or unverified worker',
                'nightly guest recovery refused: desktop lease is live or belongs to another session',
                # A guest kept for cleanup or recovery keeps every later row off the slot until someone acts.
                'guest retained', 'guest recovery')
HOST_BLOCKED_ALERT = 2  # nights running


class NotRun(RuntimeError):
    """Infrastructure prevented a valid app-flow observation."""


def cell(status='not_run', assertion='', **proof):
    return dict(status=status, green=status == 'green', assertion=assertion, **proof)


def row_cells(row, reason):
    """Every cell a row runs, each starting not run with REASON; unsupported cells say why."""
    cells = {}
    # A row with "proton": false has not proven a Proton lend on its golden: its Live Proton cell is
    # left out (— in the table), so a failed lend can't leave an outstanding Proton copy there.
    live = [f for f in LIVE if f != 'proton_region_upload' or row.get('proton', True)] if row.get('live') else []
    for flow in LOCAL + live:
        unsupported = UNSUPPORTED.get((row.get('desktop'), flow))
        cells[flow] = cell('unsupported', unsupported) if unsupported else cell(assertion=reason)
    return cells


def passed(value):
    """A cell that needs no follow-up: green, or a feature the desktop does not support."""
    return value['status'] in ('green', 'unsupported')


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')
    tmp.chmod(0o600)
    tmp.replace(path)


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def private_file(path):
    path = Path(path)
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_mode & 0o077 or info.st_nlink != 1):
        raise RuntimeError('private configuration must be an owned mode-0600 regular file')
    return path


def command(argv, *, log=None, timeout=900, env=None, terminate_grace=0, local_workers=None):
    # Never include a command or its output in exceptions/public receipts.
    if log:
        with Path(log).open('xb') as stream:
            result = run_command(argv, env=env, stdout=stream, stderr=stream,
                                 timeout=timeout, terminate_grace=terminate_grace, local_workers=local_workers)
    else:
        result = run_command(argv, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             timeout=timeout, terminate_grace=terminate_grace, local_workers=local_workers)
    if result.returncode:
        step = Path(log).name if log else Path(str(argv[0])).name
        raise RuntimeError(f'{step} failed (exit {result.returncode}); private log retained')
    return result.stdout.decode() if not log else None


class LocalWorkers:
    """Own local worker processes across the controller's dispatch threads."""
    def __init__(self):
        self.lock = threading.Lock()
        self.processes = set()
        self.stopping = False

    def start(self, args, **kwargs):
        # Registration and the stop snapshot are atomic: no late child may
        # appear after the controller has finished waiting for cleanup.
        with self.lock:
            if self.stopping:
                raise NotRun('controller stopping; local worker not started')
            process = subprocess.Popen(args, **kwargs)
            self.processes.add(process)
            return process

    def forget(self, process):
        with self.lock:
            self.processes.discard(process)

    def stop(self):
        """Forward TERM only to owned local workers and wait one shared grace."""
        with self.lock:
            self.stopping = True
            processes = list(self.processes)
        deadline = time.monotonic() + TERMINATE_GRACE
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(timeout=max(0, deadline-time.monotonic()))
            except subprocess.TimeoutExpired:
                process.kill()
        # Reap escalated children within the service's additional stop margin.
        for process in processes:
            process.wait(timeout=5)


@contextmanager
def controller_shutdown(local_workers):
    """Keep the service controller alive until local worker cleanup exits."""
    def stop(signum, _frame):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        try:
            local_workers.stop()
        finally:
            # ThreadPoolExecutor's exit hook otherwise joins the remote SSH
            # waiters indefinitely. Their watchdogs own remote cleanup; do not
            # signal those clients. Local workers have already been waited for.
            os._exit(128+signum)
    previous = signal.signal(signal.SIGTERM, stop)
    try:
        yield
    finally:
        signal.signal(signal.SIGTERM, previous)


def run_command(argv, *, env, stdout, stderr, timeout, terminate_grace, local_workers=None):
    """Give an outer worker SIGTERM cleanup time before escalating to SIGKILL."""
    args = [str(a) for a in argv]
    if not terminate_grace:
        return subprocess.run(args, cwd=ROOT, env=env, stdout=stdout, stderr=stderr, timeout=timeout)
    start = local_workers.start if local_workers is not None else subprocess.Popen
    process = start(args, cwd=ROOT, env=env, stdout=stdout, stderr=stderr)
    try:
        with process:
            try:
                output, errors = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.communicate(timeout=terminate_grace)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()
                raise
            return subprocess.CompletedProcess(args, process.returncode, output, errors)
    finally:
        if local_workers is not None:
            local_workers.forget(process)


# Why the worker must stop before its next row (termination or the spent budget), once known.
STOP = {}


@contextmanager
def worker_deadline():
    """Bound setup and flow work, then disarm the timer while finally blocks run."""
    deadline = time.monotonic() + WORKER_SECONDS
    STOP.clear()
    def expired(signum, _frame):
        signal.setitimer(signal.ITIMER_REAL, 0)
        # A second termination request must not interrupt retirement.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        reason = ('worker terminated; cleanup required' if signum == signal.SIGTERM
                  else 'worker deadline exhausted; remaining flows not run')
        STOP.setdefault('reason', reason)
        raise NotRun(reason)
    previous_term = signal.signal(signal.SIGTERM, expired)
    previous_alarm = signal.signal(signal.SIGALRM, expired)
    previous_timer = signal.setitimer(signal.ITIMER_REAL, WORKER_SECONDS)
    try:
        yield deadline
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_alarm)
        signal.signal(signal.SIGTERM, previous_term)
        signal.setitimer(signal.ITIMER_REAL, *previous_timer)


def remaining_timeout(deadline, maximum=ATTEMPT_TIMEOUT):
    """Do not begin or retry a flow once the shared worker budget is exhausted."""
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise NotRun('worker deadline exhausted; remaining flows not run')
    return min(maximum, remaining)


class GitHub:
    def __init__(self, repo, token):
        self.repo = repo
        self.env = dict(os.environ, GH_TOKEN=private_file(token).read_text().strip())

    def gh(self, *args):
        return command(['gh', *args], env=self.env)

    def api(self, path, method='GET', body=None):
        result = subprocess.run(['gh', 'api', path, '--method', method,
                                 *(['--input', '-'] if body is not None else [])],
                                input=json.dumps(body) if body is not None else None,
                                text=True, capture_output=True, env=self.env, timeout=120)
        if result.returncode:
            raise RuntimeError('GitHub request failed; response withheld')
        return json.loads(result.stdout) if result.stdout.strip() else {}

    def asset(self, asset_id, target):
        # Draft assets have no public download route. gh supplies authentication
        # from the private environment, never from argv or a receipt.
        with target.open('xb') as stream:
            result = subprocess.run(['gh', 'api', f'repos/{self.repo}/releases/assets/{asset_id}',
                                     '-H', 'Accept: application/octet-stream'],
                                    stdout=stream, stderr=subprocess.PIPE, env=self.env, timeout=900)
        if result.returncode:
            raise RuntimeError('authenticated release asset download failed')

    def issues(self):
        issues = []
        for page in range(1, 1001):
            values = self.api(f'repos/{self.repo}/issues?state=all&per_page=100&page={page}')
            issues.extend(i for i in values if 'pull_request' not in i)
            if len(values) < 100:
                return issues
        raise RuntimeError('issue pagination limit exceeded')

    def issue(self, title, body, existing=None):
        base = f'repos/{self.repo}/issues'
        return self.api(base + (f'/{existing["number"]}' if existing else ''),
                        'PATCH' if existing else 'POST', dict(title=title, body=body, state='open'))


def validate_release(release, tag, stable_draft=False):
    pattern = STABLE_TAG if stable_draft else TAG
    if (not tag or not pattern.fullmatch(tag) or release['tag_name'] != tag
            or bool(release['draft']) != stable_draft
            or stable_draft and bool(release.get('prerelease'))):
        raise RuntimeError('expected a named stable draft' if stable_draft
                           else 'expected a published nightly release tag')


def draft_release(github, tag):
    # releases/tags/TAG answers only for published releases; a draft is found by listing.
    matches = []
    for page in range(1, 1001):
        listed = github.api(f'repos/{github.repo}/releases?per_page=100&page={page}')
        matches += [r for r in listed if r['draft'] and r['tag_name'] == tag]
        if len(listed) < 100:
            break
    if len(matches) != 1:
        raise RuntimeError('expected exactly one draft release for '+tag)
    return matches[0]


def tag_commit(github, tag):
    obj = github.api(f'repos/{github.repo}/git/ref/tags/{tag}')['object']
    for _ in range(8):
        if obj['type'] == 'commit' and re.fullmatch('[0-9a-f]{40}', obj['sha']):
            return obj['sha']
        if obj['type'] != 'tag':
            break
        obj = github.api(f'repos/{github.repo}/git/tags/{obj["sha"]}')['object']
    raise RuntimeError('release tag does not resolve to a commit')


def release_assets(github, tag, out, rows, *, release=None):
    """Download the published bytes; every selected package must match SHA256SUMS."""
    out.mkdir(mode=0o700)
    version = tag[1:]
    assets = {f'putgd-{version}-{row["asset"]}' for row in rows}
    for name in sorted(assets | {'SHA256SUMS', 'release-builds.json'}):
        if release is not None:
            matches = [a for a in release['assets'] if a['name'] == name]
            if len(matches) != 1:
                raise RuntimeError('missing or duplicate draft asset: '+name)
            github.asset(matches[0]['id'], out/name)
        else:
            github.gh('release', 'download', tag, '--repo', github.repo, '--pattern', name, '--dir', str(out))
    sums = {}
    for line in (out/'SHA256SUMS').read_text().splitlines():
        sha, name = line.split(maxsplit=1)
        name = name.lstrip('*')
        if name in sums or not re.fullmatch('[0-9a-f]{64}', sha):
            raise RuntimeError('invalid SHA256SUMS')
        sums[name] = sha
    for name in assets | {'release-builds.json'}:
        if sums.get(name) != digest(out/name):
            raise RuntimeError('release asset checksum mismatch')
    identity = json.loads((out/'release-builds.json').read_text())['release']
    if identity['tag'] != tag or not re.fullmatch('[0-9a-f]{40}', identity['gitCommit']):
        raise RuntimeError('release identity mismatch')
    if release is not None and identity['gitCommit'] != tag_commit(github, tag):
        raise RuntimeError('release build differs from tag commit')
    return identity


def ci_passed(github, run):
    jobs = github.api(f'repos/{github.repo}/actions/runs/{run["id"]}/jobs?per_page=100')['jobs']
    return any(job['name'] == 'ci / CI' and job['conclusion'] == 'success' for job in jobs)


def catalog(github, build, out, staged=False):
    """Find a successful producer with the exact build, including reused source CI.

    A staged draft's Release run can still fail after staging, in its draft audit;
    its catalog counts when that run's CI job passed.
    """
    status = '' if staged else '&status=success'
    runs = github.api(f'repos/{github.repo}/actions/runs?head_sha={build}{status}&per_page=100')['workflow_runs']
    for run in runs:
        artifacts = github.api(f'repos/{github.repo}/actions/runs/{run["id"]}/artifacts?per_page=100')['artifacts']
        if any(a['name'] == 'ui-catalog' and not a['expired'] for a in artifacts) \
                and (run.get('conclusion') == 'success' or staged and ci_passed(github, run)):
            github.gh('run', 'download', str(run['id']), '--repo', github.repo,
                      '--name', 'ui-catalog', '--dir', str(out))
            manifest = json.loads((out/'manifest.json').read_text())
            if manifest['build']['gitCommit'] != build or manifest['build']['dirty']:
                raise RuntimeError('UI catalog identity mismatch')
            return dict(runId=run['id'], manifestSha256=digest(out/'manifest.json'))
    raise RuntimeError('no exact-build UI catalog available')


def idle_reading(rows):
    """The idle-memory recipe's flat summary for the public receipt, if it got that far."""
    match = next(filter(None, (re.search(r'idle_memory=(\{.*?\})', r.get('note') or '') for r in rows)), None)
    try:
        return json.loads(match[1]) if match else None
    except ValueError:
        return None


def verdict(out, recipe):
    """Only terminal audited automatic assertions count; review/visual flags do not."""
    from tools.lab.surface_driver.collect import require_terminal
    try:
        terminal = require_terminal(out)
        receipt = json.loads((Path(out)/'receipt.json').read_text())
        rows = [r for r in receipt['interactions'] if r['recipe'] == recipe]
        env = receipt['environment']
        observed = env.get('observed_build', {})
        if observed.get('gitCommit') != env.get('build') or observed.get('dirty') is not False:
            return cell(assertion='installed clean build was not verified')
        passed = bool(rows) and all(r['functional'] == 'passed' for r in rows)
        assertion = next((r.get('note') or r['functional'] for r in rows if r['functional'] != 'passed'), '')
        if not rows:
            assertion = env.get('blocked') or 'flow produced no assertion receipt'
        if env.get('cleanup_errors'):
            assertion = 'private target cleanup failed'
        # The runner retains safe assertion messages for RecipeFailed; unexpected
        # driver errors, blocked fixtures, deadlines and cleanup are infrastructure.
        infrastructure = ('driver error:', 'fixture cleanup failed', 'recipe cleanup',
                          'deadline exceeded', 'shell crashed')
        failed = any(r['functional'] == 'failed' for r in rows)
        status = 'green' if passed else 'red' if failed else 'not_run'
        input_blocked = re.search(r"(?:input target '[^']+'|desktop input) is obscured by "
                                 r"'(?:shell overview|no focused window)'(?: \(window None\))?$", assertion)
        if (env.get('cleanup_errors') or env.get('interrupted') or not rows
                or input_blocked or any(text in assertion.lower() for text in infrastructure)):
            status = 'not_run'
        memory = {'memory': idle_reading(rows)} if recipe == 'outcomes.idle_memory' else {}
        return cell(status, assertion[-1800:], terminal=terminal, observedBuild=observed['gitCommit'],
                    observations=list(dict.fromkeys((r.get('note') or '')[-1800:] for r in rows)),
                    receiptSha256=digest(Path(out)/'receipt.json'), receipt=str(Path(out)/'receipt.json'), **memory)
    except (OSError, ValueError, KeyError, RuntimeError, TypeError):
        return cell(assertion='terminal audited receipt missing or invalid', receipt=str(Path(out)/'receipt.json'))


def controller_ssh_identities(*, proc=Path('/proc')):
    """Excuse only live SSH clients forked by our controller for other rows.

    The local worker receives its controller's PID/start time and exact configured
    SSH argv prefixes. Never excuse descendants, command substrings, or bare PIDs:
    the release inventory must still match the process's start time.
    """
    try:
        controller = json.loads(os.environ.get('PUTGD_NIGHTLY_CONTROLLER', '{}'))
        parent = controller['pid']
        if parent != os.getppid():
            return []
        def identity(pid):
            fields = (proc/str(pid)/'stat').read_text().rsplit(')', 1)[1].split()
            return int(fields[1]), fields[19]
        if identity(parent)[1] != controller['start']:
            return []
        prefixes = [argv for argv in controller['ssh'] if argv and Path(argv[0]).name == 'ssh']
        children = {pid for task in (proc/str(parent)/'task').iterdir()
                    for pid in (task/'children').read_text().split()}
        result = []
        for pid in children:
            try:
                path = proc/pid
                before = identity(pid)
                argv = os.fsdecode((path/'cmdline').read_bytes()).rstrip('\0').split('\0')
                if (before[0] == parent and path.stat().st_uid == os.getuid()
                        and (path/'exe').readlink().name == 'ssh'
                        and any(argv[:-1] == prefix for prefix in prefixes)
                        and identity(pid) == before):
                    result.append(f'process:{pid}:{before[1]}')
            except (OSError, ValueError, IndexError):
                continue  # Exited or unreadable processes are never excused.
        return result if identity(parent)[1] == controller['start'] else []
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return []


RECOVERY_SECONDS = 180
# A cell that cannot quit its resident has lost the guest session (#1214). The
# next cells run against a guest that cannot pass them, so the row stops.
LOST_SESSION_ERRORS = frozenset(('resident quit transport unavailable; private session retained',
                                 'resident quit was rejected; private session retained'))


def lost_guest_session(out):
    """Whether this attempt's receipt shows its resident could not be quit."""
    try:
        errors = json.loads((Path(out)/'receipt.json').read_text()).get('environment', {}).get('cleanup_errors') or []
        return bool(LOST_SESSION_ERRORS.intersection(errors))
    except (OSError, ValueError, AttributeError, TypeError):
        return False


def failed_recipe_note(out, fallback):
    """A failed recipe's own note; a recipe that passed only has its cleanup to report.

    A recipe that names its failed pass and step (a failed_step=… note, #2051) gives that note alone,
    and the fixture cleanup failures after it: the counters of the passes before it are not the cause.
    """
    try:
        rows = json.loads((Path(out)/'receipt.json').read_text()).get('interactions', [])
        note = next((r['note'] for r in rows if r.get('functional') != 'passed' and r.get('note')), fallback)
    except (OSError, ValueError, AttributeError, TypeError):
        return fallback
    if not isinstance(note, str):
        return fallback
    parts = note.split('; ')
    step = next((i for i, part in enumerate(parts) if part.startswith('failed_step=')), None)
    if step is None:
        return note
    cleanup = next((i for i in range(step+1, len(parts)) if parts[i].startswith('fixture cleanup failed')), len(parts))
    return '; '.join([parts[step].removeprefix('failed_step=')]+parts[cleanup:])


def worker_identity():
    from tools.lab.desktop_lock import process_start
    return dict(pid=os.getpid(), start=process_start(os.getpid()),
                boot=Path('/proc/sys/kernel/random/boot_id').read_text().strip())


def previous_worker_gone(owner):
    from tools.lab.desktop_lock import process_start
    if owner.get('worker'):
        old = owner['worker']
        if old['boot'] != worker_identity()['boot']:
            return True
        current = process_start(old['pid'])
        # An unreadable live process is not proof of exit.
        if not current:
            return not Path(f'/proc/{old["pid"]}').exists()
        return bool(old['start']) and current != old['start']
    # Legacy retained slots have no PID identity. Require a terminal matching
    # row AND absence of the exact worker output argv (not a process pattern).
    evidence = Path(owner['evidence'])
    result = json.loads((evidence/'result.json').read_text())
    if result.get('session') != owner['session'] or not result.get('finishedAt'):
        return False
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            argv = os.fsdecode((proc/'cmdline').read_bytes()).rstrip('\0').split('\0')
            if ('--worker' in argv and '--output' in argv
                    and argv[argv.index('--output')+1] == str(evidence.parent)):
                return False
        except FileNotFoundError:
            continue
        except (OSError, IndexError):
            return False
    return True


def guest_clients_live(run):
    """Refuse an orphaned lend/recipe, or a QEMU whose run was removed."""
    disk = str(run/'disk.qcow2').encode()
    for proc in Path('/proc').glob('[0-9]*'):
        try:
            if proc.stat().st_uid != os.getuid():
                continue
            raw = (proc/'cmdline').read_bytes().rstrip(b'\0').split(b'\0')
            argv = [os.fsdecode(a) for a in raw]
            if (str(run) in argv and ('tools.lab.surface_driver.run' in argv or
                    any(Path(a).name == 'proton_lab.py' for a in argv) and 'lend' in argv)):
                return True
            if (not run.exists() and raw and Path(os.fsdecode(raw[0])).name.startswith('qemu-system-')
                    and any(disk in a.split(b',') or b'file='+disk in a.split(b',') for a in raw)):
                return True
        except FileNotFoundError:
            continue
        except OSError:
            return True
    return False


# Operator records for a guest kept over live cleanup (docs/lab/remote-builders.md#a-nightly-guest-kept-for-live-cleanup).
# Each lives in the row's evidence and clears one recovery gate only.
LIVE_RESOLUTION = 'live-cleanup-resolution.json'
UNAUDITED_ATTEMPT = 'unaudited-attempt.json'


def owned_overlay(owner):
    run, evidence = Path(owner['run']), Path(owner['evidence'])
    if run.parent != ROOT/'work/lab-runs' or not re.fullmatch(r'n[0-9a-f]{8}', run.name):
        raise NotRun('guest recovery refused: run is not an owned nightly overlay')
    from tools.lab.overlay_cleanup import _no_symlinks
    _no_symlinks(run)
    _no_symlinks(evidence)
    return run, evidence


def run_identity(run):
    """The overlay's lab run id, or None once the run directory is gone."""
    from tools.lab import lab
    return lab.state(run)['id'] if (run/'state.json').exists() else None


def live_resolution(run, evidence, session):
    """The operator's resolution record for this run and session, or None. A record that does not match is refused."""
    path = evidence/LIVE_RESOLUTION
    if not path.exists():
        return None
    record = json.loads(private_file(path).read_text())
    publication = record.get('publication')
    # Either a confirmed-gone publication, or a record that the row had none pending (it clears no live gate).
    confirmed = (record.get('objectConfirmedGone') is True and isinstance(publication, dict)
                 and sorted(publication) == ['destination', 'objectKey', 'prefix']
                 and all(isinstance(v, str) and v for v in publication.values()))
    nothing_pending = (publication is None and record.get('objectConfirmedGone') is False
                       and record.get('livePublicationPending') is False)
    if (record.get('schemaVersion') != 1 or record.get('kind') != 'live-cleanup-resolution'
            or record.get('run') != str(run) or record.get('session') != session
            or record.get('runId') != run_identity(run) or not (confirmed or nothing_pending)
            or not isinstance(record.get('receipts'), dict) or not isinstance(record.get('guestOnlyErrors'), dict)):
        raise NotRun('guest retained: '+str(path)+' does not record a confirmed cleanup of this run')
    return record


def unaudited_attempt(attempt):
    """True when the operator recorded this attempt as unaudited and its receipt is the one they saw."""
    path = attempt/UNAUDITED_ATTEMPT
    if not path.exists():
        return False
    record = json.loads(private_file(path).read_text())
    if (record.get('schemaVersion') != 1 or record.get('kind') != 'unaudited-attempt'
            or record.get('attempt') != attempt.name or os.path.lexists(attempt/'terminal.json')
            or record.get('receiptSha256') != digest(attempt/'receipt.json')):
        raise NotRun('guest retained: '+str(path)+' does not match the attempt receipt')
    return True


def retained_owner():
    """The retained nightly slot's owner, only when its worker is gone. Operator commands act on nothing else."""
    slot = Path.home()/'putgd-lab/locks/tier1-guest.lock.d'
    if not slot.exists():
        raise NotRun('no retained nightly guest slot on this builder')
    owner = json.loads(private_file(slot/('retained.json' if (slot/'retained.json').exists()
                                          else 'owner.json')).read_text())
    if (slot/'session').read_text().strip() != owner['session'] or not previous_worker_gone(owner):
        raise NotRun('nightly guest slot belongs to a live or unverified worker')
    return owner


def settled_overlay(owner, checked_run):
    """The slot's run, only when it is the run the person checked and nothing still publishes from it."""
    run, evidence = owned_overlay(owner)
    if run.name != checked_run:
        raise NotRun('the retained slot holds '+run.name+', not the run you checked ('+checked_run+')')
    # An orphaned recipe or Proton lend may still be uploading after its worker died.
    if guest_clients_live(run):
        raise NotRun('a lend or recipe for this run is still live; wait until it has stopped')
    return run, evidence


def frozen_overlay(owner, checked_run):
    """settled_overlay, plus a guest VM that stayed paused since --check-live-cleanup paused it."""
    run, evidence = settled_overlay(owner, checked_run)
    paused = guest_paused(run)
    if paused is None:
        raise NotRun('the guest VM is running, or its stop is not recorded; run --check-live-cleanup (it pauses the VM or '
                     'records that QEMU has exited), then check the destination again')
    if time.time()-paused < PAUSE_SETTLE_SECONDS:
        raise NotRun('the guest VM was paused under '+str(PAUSE_SETTLE_SECONDS)+' s ago; check the destination again after that')
    return run, evidence


# An upload request whose body reached the destination before the pause can still commit just after it.
# Proton and S3 multipart uploads need a final call from the paused client; a single S3 PUT commits within
# seconds of its body arriving. Destination checks count only this long after the pause.
PAUSE_SETTLE_SECONDS = 120
PAUSE_RECORD = 'live-cleanup-paused.json'


def vm_process_gone(run):
    """True only when the lab's full host inventory finds no QEMU, TPM or open/mapped handle on the run's overlay.

    The same check guarded retirement makes: a filtered /proc or hidden PID namespace is no proof, and the
    overlay is matched by file identity, not only by path.
    """
    from tools.lab import overlay_cleanup
    disk = run/'disk.qcow2'
    try:
        overlay_cleanup._no_symlinks(disk)
        if not disk.is_file():
            return False
        overlay_cleanup._check_processes(run, disk)
    except (overlay_cleanup.CleanupError, OSError):
        return False
    return True


def disk_identity(run):
    """The overlay's identity and last write; a QEMU that ran on it since changes this."""
    info = (run/'disk.qcow2').stat()
    return [info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns]


def pause_record(run, kind):
    """The pausedAt of the run's pause record when it is of KIND ('paused' or 'exited'); else None.

    An exit record holds only while the overlay is unchanged since it was written: a QEMU started on it
    again writes to it, and its exit later must start a new settle period.
    """
    try:
        held = json.loads(private_file(run/PAUSE_RECORD).read_text())
        # Records written before the exited kind existed are pauses.
        if held.get('vm', 'paused') != kind:
            return None
        if kind == 'exited' and held.get('disk') != disk_identity(run):
            return None
        return float(held['pausedAt'])
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, AttributeError):
        return None


def guest_paused(run, *, pause=False):
    """When the guest's VM is paused, the time.time() it was paused; else None. ``pause`` pauses it first.

    A recipe killed on the host can leave put.gd in the guest finishing an upload; a destination check made
    while the VM runs proves nothing. Retirement still quits a paused VM; QMP ``cont`` resumes it, and the
    next pause starts a new settle period. A run with neither disk nor state has no VM (paused at 0).

    A VM whose QEMU has already exited (a transient unit can kill it with its worker) can never publish
    again. ``pause`` records the first look that found no QEMU on the overlay, and the settle period runs
    from then; a pause recorded before QEMU exited does not count, since the VM may have resumed in between.
    """
    from tools.lab import lab
    if not any((run/name).exists() for name in ('qmp.sock', 'disk.qcow2', 'state.json')):
        return 0
    record = run/PAUSE_RECORD
    if (run/'qmp.sock').exists():
        try:
            with lab.QMP(run/'qmp.sock', timeout=5) as qmp:
                # A QEMU answered: any exit record is from an earlier one, even if this session drops next.
                if pause_record(run, 'paused') is None:
                    record.unlink(missing_ok=True)
                running = qmp.execute('query-status').get('running')
                if pause and running is not False:
                    # A stale time must never outlive a resume, even if the stop reply is lost.
                    record.unlink(missing_ok=True)
                    qmp.execute('stop')
                    running = qmp.execute('query-status').get('running')
                    if running is False:
                        save(record, dict(pausedAt=time.time(), vm='paused'))
                if running is not False:
                    # The VM ran after that pause: a later pause, by anything, starts a new settle period.
                    record.unlink(missing_ok=True)
                    return None
            if pause and pause_record(run, 'paused') is None:
                # Paused by something else (or an exit record from an earlier QEMU): the settle period starts now.
                save(record, dict(pausedAt=time.time(), vm='paused'))
            return pause_record(run, 'paused')
        except (OSError, lab.LabError) as error:
            # Only a refused or missing socket says nothing listens. Anything that accepted the connection
            # (even one that dropped during the greeting or negotiation) was a QEMU: no exit record outlives it.
            if not isinstance(error, (ConnectionRefusedError, FileNotFoundError)) and pause_record(run, 'paused') is None:
                record.unlink(missing_ok=True)
    # No QMP answer: only a QEMU that is provably gone counts as stopped.
    if not vm_process_gone(run):
        if pause_record(run, 'paused') is None:
            record.unlink(missing_ok=True)
        return None
    if pause and pause_record(run, 'exited') is None:
        save(record, dict(pausedAt=time.time(), vm='exited', disk=disk_identity(run)))
    return pause_record(run, 'exited')


def publication_pending(run, evidence):
    """Whether the run's marker or any receipt of the row records a live publication not yet cleaned up."""
    from tools.lab import lab
    if (run/lab.LIVE_CLEANUP_MARKER).exists():
        return True
    return any(json.loads(path.read_text()).get('environment', {}).get('unresolved_live_cleanup')
               for path in evidence.glob('*/receipt.json'))


def resolution_overlay(owner, checked_run, no_live_publication=False):
    """The overlay a resolution may be recorded for: frozen for a live publication, settled for none."""
    if not no_live_publication:
        return frozen_overlay(owner, checked_run)
    run, evidence = settled_overlay(owner, checked_run)
    if publication_pending(run, evidence):
        raise NotRun('a live publication is pending (a marker or a receipt flag); follow the live cleanup steps instead')
    return run, evidence


def resolve_live_cleanup(owner, checked_run, *, publication=None, guest_only_errors=(), no_live_publication=False, now=None):
    """Record what a person checked about a retained row; for a live publication, then remove the marker.

    The record names the run, its lab run id, the publication from the marker and the hash of every
    receipt in the row, so it clears the live-cleanup gate for exactly those receipts. ``guest_only_errors``
    are receipt cleanup errors the person checked are held only in the guest; recovery counts them as
    guest-local for those receipts and nothing more.

    ``no_live_publication`` is for a row with no marker and no receipt flag. It needs no paused guest or
    destination check, names no publication, never touches a marker and clears no live-cleanup gate.
    """
    from tools.lab import lab
    if no_live_publication and publication:
        raise NotRun('--no-live-publication names no publication')
    if no_live_publication and not guest_only_errors:
        raise NotRun('nothing to record: give the --guest-only-error values you checked, or run --recover-retained')
    run, evidence = resolution_overlay(owner, checked_run, no_live_publication)
    marker = run/lab.LIVE_CLEANUP_MARKER
    identity = run_identity(run)
    if no_live_publication:
        pass
    elif marker.exists():
        details = json.loads(private_file(marker).read_text())
        found = {k: details.get(k) for k in ('destination', 'prefix', 'objectKey')}
        if details.get('schemaVersion') != 1 or details.get('runId') != identity:
            raise NotRun('live cleanup marker does not belong to this run; inspect '+str(marker))
        if publication and publication != found:
            raise NotRun('the given publication differs from the marker; check the object named in '+str(marker))
        publication = found
    elif not publication:
        raise NotRun('no live cleanup marker; give --destination, --prefix and --object-key of the confirmed object, '
                     'or --no-live-publication when the row had none')
    if publication is not None:
        if not all(isinstance(v, str) and v for v in publication.values()):
            raise NotRun('live cleanup publication needs a destination, prefix and object key')
        if not publication['objectKey'].startswith(publication['prefix']):
            raise NotRun('live cleanup object key is outside its prefix')
    receipts, errors = {}, {}
    for path in sorted(evidence.glob('*/receipt.json')):
        receipts[path.parent.name] = digest(path)
        held = json.loads(path.read_text()).get('environment', {}).get('cleanup_errors', [])
        if matched := [e for e in guest_only_errors if e in held]:
            errors[path.parent.name] = matched
    unknown = [e for e in guest_only_errors if not any(e in v for v in errors.values())]
    if unknown:
        raise NotRun('--guest-only-error must be an exact cleanup error of a receipt in this row')
    record = dict(schemaVersion=1, kind='live-cleanup-resolution', run=str(run), runId=identity,
                  session=owner['session'], publication=publication, objectConfirmedGone=publication is not None,
                  markerPresent=marker.exists(), receipts=receipts, guestOnlyErrors=errors,
                  resolvedAt=now or time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    if publication is None:
        record['livePublicationPending'] = False
    if (evidence/LIVE_RESOLUTION).exists():
        shutil.copy2(evidence/LIVE_RESOLUTION, evidence/(LIVE_RESOLUTION+'.before-'+uuid.uuid4().hex[:8]))
    # The record first: a crash in between leaves both, which still keeps the guest.
    save(evidence/LIVE_RESOLUTION, record)
    if publication is not None:
        # A marker written after a no-publication check stays, and keeps the guest.
        marker.unlink(missing_ok=True)
    return record


def check_s3_absent(publication, *, client=None):
    """Read-only proof that a Tier 1 S3 publication is gone: exact-key GET 404 and an empty prefix.

    Uses key set 2 in memory, as the live fixture does; it prints no credential and changes nothing.
    """
    if (publication.get('destination') != 's3'
            or not re.fullmatch(r'acceptance/tier1-live/[0-9a-f]{32}/', publication.get('prefix', ''))
            or not publication.get('objectKey', '').startswith(publication['prefix'])):
        raise NotRun('the S3 check covers only a Tier 1 live acceptance prefix')
    # Fixed messages only: credential-file values and SDK errors never reach output.
    if client is None:
        try:
            from tools.lab import provision_live_credentials as bridge
            from tools.lab.surface_driver.fixtures.live_outcomes import key_set_two
            import boto3
            from botocore.config import Config
            selected = key_set_two(bridge.read_private_credentials(Path.home()/'Documents/creds.txt'))
            keys, region = selected['credentials'], selected['settings'].get('region', '')
            if (selected['settings'].get('bucket') != 'put.gd' or selected['settings'].get('endpoint')
                    or not re.fullmatch(r'[a-z]{2}(?:-gov)?-[a-z]+-[1-9][0-9]?', str(region))):
                raise ValueError
            client = boto3.Session(region_name=region).client(
                's3', region_name=region, endpoint_url='https://s3.'+region+'.amazonaws.com',
                aws_access_key_id=keys['accessKey'], aws_secret_access_key=keys['secretKey'],
                aws_session_token=keys.get('sessionToken') or None,
                config=Config(connect_timeout=10, read_timeout=20, retries={'total_max_attempts': 2, 'mode': 'standard'}))
        except Exception:
            raise NotRun('S3 check: key set 2 or its standard put.gd bucket settings are unavailable') from None
    try:
        try:
            client.get_object(Bucket='put.gd', Key=publication['objectKey'])['Body'].close()
            key_absent = False
        except client.exceptions.NoSuchKey:
            key_absent = True
        listed = client.list_objects_v2(Bucket='put.gd', Prefix=publication['prefix'], MaxKeys=1)
        # Only an explicit, complete, empty listing proves the prefix is empty.
        prefix_empty = (listed.get('KeyCount') == 0 and listed.get('IsTruncated') is False
                        and not listed.get('Contents'))
        # A delete in a versioned bucket can leave the uploaded version behind a delete marker.
        status = client.get_bucket_versioning(Bucket='put.gd').get('Status')
        if status is None:
            versions_gone = True
        else:
            # Delete markers can fill pages; follow them (bounded) and look for any remaining version.
            versions_gone, page = False, dict(Bucket='put.gd', Prefix=publication['prefix'], MaxKeys=1000)
            for _ in range(20):
                versions = client.list_object_versions(**page)
                if versions.get('Versions'):
                    break
                if versions.get('IsTruncated') is False:
                    versions_gone = True
                    break
                # Both markers or stop: a key marker alone can resume past that key's remaining versions.
                if (versions.get('IsTruncated') is not True or not versions.get('NextKeyMarker')
                        or not versions.get('NextVersionIdMarker')):
                    break
                page.update(KeyMarker=versions['NextKeyMarker'], VersionIdMarker=versions['NextVersionIdMarker'])
    except Exception:
        raise NotRun('S3 check could not complete; keep the guest and try again') from None
    return dict(objectKeyAbsent=key_absent, prefixEmpty=prefix_empty, versionsGone=versions_gone)


def record_unaudited_attempt(owner, name, *, now=None):
    """Mark one attempt killed before its terminal proof as unaudited, keeping its receipt in place.

    Retirement then archives only the audited attempts and names this one, with its record and receipt
    hashes, in the completion manifest. It never makes the attempt count as a result.
    """
    _, evidence = owned_overlay(owner)
    attempt = evidence/name
    if Path(name).name != name or name in ('.', '..') or not (attempt/'receipt.json').is_file():
        raise NotRun('attempt must be a directory of this row with a receipt.json')
    # Only a runner killed before it wrote terminal.json; a terminal proof that fails its checks stays a refusal.
    if os.path.lexists(attempt/'terminal.json'):
        raise NotRun('attempt has a terminal.json; only an attempt killed before its terminal proof can be unaudited')
    record = dict(schemaVersion=1, kind='unaudited-attempt', attempt=name, session=owner['session'],
                  receiptSha256=digest(attempt/'receipt.json'),
                  recordedAt=now or time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    save(attempt/UNAUDITED_ATTEMPT, record)
    return record


def recover_guest(config, owner, out, cells):
    """Recover only this slot's credential baton, then guarded overlay retirement.

    Each external recovery step is bounded. Unknown/private publication cleanup
    stays retained; destroying a disposable VM can resolve only guest-local loss.
    """
    from tools.lab.desktop_lock import Desktop
    from tools.lab import lab
    run, evidence = owned_overlay(owner)
    session = owner['session']
    if (run/lab.LIVE_CLEANUP_MARKER).exists():
        raise NotRun('guest retained: live remote cleanup unresolved (marker in the run directory); resolve it before recovery')
    if guest_clients_live(run):
        raise NotRun('guest recovery refused: exact lend/recipe or missing-run QEMU is still live')
    locks = Path.home()/'putgd-lab/locks'
    proton = Desktop(locks, config['host'], resource='proton')
    state = proton.status()
    held = state.get('lock', {})
    if state['state'] != 'free' and (held.get('session') != session or held.get('owner') != 'nightly-desktops'):
        raise NotRun('guest recovery waiting: Proton lease belongs to another session')
    lends = []
    for path in (Path.home()/'putgd-lab/proton/receipts').glob('*/lend.json'):
        lent = json.loads(private_file(path).read_text())
        if lent.get('run') == str(run) and lent.get('session') == session:
            lends.append((lent.get('updated', ''), path.parent, lent))
    safe_steps = ('cleaned', 'recovered', 'recovered_nothing_lent', 'provision_failed_abandoned')
    lend_step = None
    if lends:
        _, folder, lent = max(lends, key=lambda item:item[0])
        if lent.get('step') not in safe_steps:
            if run.exists():
                # Use this driver's recover implementation, including on Atomic.
                command([sys.executable, ROOT/'tools/lab/proton_lab.py', 'recover', folder,
                         '--session', session, '--owner', 'nightly-desktops'],
                        log=out/'proton-recover.json', timeout=120,
                        env=dict(os.environ, HOME=str(Path.home().resolve())))
                lent = json.loads((folder/'lend.json').read_text())
                if lent.get('step') not in safe_steps:
                    raise NotRun('guest recovery incomplete: run proton_lab.py recover with retained lend receipts')
            else:
                from tools.lab.provision_live_credentials import _load_receipt
                _, provision = _load_receipt(folder/'provision.json')
                if not (lent.get('step') == 'carried_back' and provision['run'] == str(run)
                        and provision['provider'] == 'proton' and provision['kind'] == 'chain_guest'
                        and provision.get('cleanupAllowed') is True
                        and provision.get('state') in ('cleanup_pending', 'cleanup_confirmed')):
                    raise NotRun('guest missing with unresolved Proton carry-back; inspect retained lend receipts')
        lend_step = lent.get('step')
        # The guest is no longer the sole current copy. This also handles old
        # expired leases whose VM/run directory has already been retired.
        if proton.status()['state'] != 'free':
            save(out/'proton-release.json', proton.release(session, verified_clean=True))
    elif state['state'] != 'free':
        raise NotRun('guest recovery refused: owned Proton lease has no matching lend receipt')
    resolution = live_resolution(run, evidence, session)
    for path in evidence.glob('*/receipt.json'):
        environment = json.loads(path.read_text()).get('environment', {})
        errors = environment.get('cleanup_errors', [])
        # Only a resolution that hashed this very receipt speaks for it.
        resolved = bool(resolution) and resolution['receipts'].get(path.parent.name) == digest(path)
        # A live destination that still holds an object or share link keeps its History row and credential
        # reference in this guest. Retiring would discard the only way to remove it (never retire) until a
        # person confirms the object is gone and records it (LIVE_RESOLUTION). A record that the row had
        # no publication pending never clears this gate.
        if environment.get('unresolved_live_cleanup') and not (resolved and resolution['publication'] is not None):
            raise NotRun('guest retained: live remote cleanup unresolved in '+str(path)+'; resolve it before recovery')
        # The receipt's own record of which cleanup errors only the guest holds (the guest was unreachable
        # or its resident refused to quit). Retire only when every error is one of those.
        guest_local = list(environment.get('guest_local_cleanup', []))
        if resolved:
            guest_local += resolution['guestOnlyErrors'].get(path.parent.name, [])
        if (any(error not in guest_local for error in errors) or
                'retained' in str(environment.get('private_cleanup', '')) and not errors):
            raise NotRun('guest retained: inspect private cleanup in '+str(path)+' before explicit recovery')
    if ((run/'state.json').exists() and (evidence/'completion-manifest.json').exists()
            and (os.path.lexists(run/'overlay-cleanup.json') or not os.path.lexists(run/'disk.qcow2'))):
        # An earlier retirement got as far as guarded cleanup (prepared or completed), then stopped or its lease
        # release refused. Finish or re-verify it against its own manifest; retiring again would write new
        # evidence that no longer matches the cleanup receipt.
        finish_retirement(run, evidence)
        guest = 'cleanup finished or re-checked from the earlier manifest'
    elif (run/'state.json').exists():
        # retire uses exact QEMU/overlay ownership and the existing evidence gate.
        retire(run, evidence, cells)
        guest = 'retired'
    elif run.exists():
        raise NotRun('guest retained: missing state.json; inspect exact overlay before recovery')
    else:
        guest = 'run directory already gone'
    # Why the guest could go: each gate and what cleared it.
    summary = dict(session=session, run=str(run), evidence=str(evidence), guest=guest, protonLend=lend_step)
    if resolution:
        summary['resolution'] = dict(path=str(evidence/LIVE_RESOLUTION), sha256=digest(evidence/LIVE_RESOLUTION),
                                     publication=resolution['publication'],
                                     guestOnlyErrors=resolution['guestOnlyErrors'])
    save(out/'guest-recovered.json', summary)
    return summary


@contextmanager
def recovery_mutex():
    """One recovery or operator record at a time on this builder."""
    with (Path.home()/'putgd-lab/locks/tier1-recovery.lock').open('a') as mutex:
        try:
            fcntl.flock(mutex, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise NotRun('nightly guest recovery is already running in another worker') from None
        yield


def recover_stale_guest(config, out, *, checked_run=None, by='nightly'):
    """Before waiting, reconcile one dead worker's durable slot. Never steal a live lease.

    The nightly calls this before every row; ``--recover-retained`` calls it for the run a person checked
    (``checked_run``). Each recovery leaves one record in locks/history/recovered naming the slot's files,
    the reason it was kept and the gates that cleared.
    """
    from tools.lab.desktop_lock import Desktop
    locks = Path.home()/'putgd-lab/locks'
    slot = locks/'tier1-guest.lock.d'
    if not slot.exists():
        if checked_run:
            raise NotRun('no retained nightly guest slot on this builder')
        return
    try:
        with recovery_mutex():
            owner = retained_owner()
            if checked_run and Path(owner['run']).name != checked_run:
                raise NotRun('the retained slot holds '+Path(owner['run']).name+', not the run you checked ('+checked_run+')')
            desktop = Desktop(locks, config['host'])
            state = desktop.status()
            if state['state'] != 'free' and (state.get('lock', {}).get('session') != owner['session']
                    or state.get('lock', {}).get('owner') != 'nightly-desktops' or not state.get('breakable')):
                raise NotRun('nightly guest recovery refused: desktop lease is live or belongs to another session '
                             '(a kept lease turns stale 15 minutes after its last heartbeat)')
            recovery = out/'stale-recovery'
            recovery.mkdir(mode=0o700)
            summary = recover_guest(config, owner, recovery, {})
            # Ordinary checked release refuses any unexplained host leftovers.
            released = None
            if state['state'] != 'free':
                save(recovery/'desktop-release.json', desktop.release(owner['session'], verified_clean=True,
                     not_mine=controller_ssh_identities if config['host'] == 'd69' else [], tmp_blocks=False))
                released = str(recovery/'desktop-release.json')
            files = [name for name in ('session', 'owner.json', 'retained.json') if os.path.lexists(slot/name)]
            stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
            save(locks/'history/recovered'/(stamp+'-'+Path(owner['run']).name+'-'+uuid.uuid4().hex[:8]+'.json'),
                 dict(schemaVersion=1, kind='tier1-slot-recovered', recoveredBy=by, recoveredAt=stamp,
                      owner=owner, keptReason=owner.get('reason'), slotFiles=files, cleared=summary,
                      recovery=str(recovery), desktopRelease=released))
            for name in ('session', 'owner.json', 'retained.json'):
                (slot/name).unlink(missing_ok=True)
            slot.rmdir()
            return summary
    except NotRun:
        raise
    except Exception as error:
        # The type alone: exception payloads can carry private paths or responses.
        save(out/'stale-recovery-error.json', dict(type=type(error).__name__))
        raise NotRun('nightly guest recovery incomplete; inspect '+str(out/'stale-recovery-error.json')+
                     ' and tier1-guest.lock.d; explicit recovery required') from None


def recover_retained(config, checked_run):
    """--recover-retained: recover the slot now, exactly as the next nightly would, for the run a person checked."""
    owner = retained_owner()
    # Refuse a wrong run or a live client before writing anything into the row.
    run, evidence = settled_overlay(owner, checked_run)
    out = evidence/'operator-recovery'/(time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())+'-'+uuid.uuid4().hex[:8])
    out.mkdir(mode=0o700, parents=True)
    with recovery_deadline():
        return recover_stale_guest(config, out, checked_run=checked_run, by='operator')


@contextmanager
def recovery_deadline():
    """One bounded cleanup attempt, independent of the expired flow budget."""
    def expired(_signum, _frame):
        raise NotRun('guest recovery deadline exhausted; explicit recovery required')
    previous = signal.signal(signal.SIGALRM, expired)
    started = time.monotonic()
    remaining, interval = signal.setitimer(signal.ITIMER_REAL, RECOVERY_SECONDS)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)
        if remaining:
            # Recovery spends the outer budget; one that ran out meanwhile fires now.
            remaining = max(remaining-(time.monotonic()-started), 0.001)
        signal.setitimer(signal.ITIMER_REAL, remaining, interval)


def recover_row(config, reservation, session, out, cells):
    recovery = out/'recovery'
    try:
        recovery.mkdir(mode=0o700, exist_ok=True)
        with recovery_deadline():
            recover_guest(config, dict(reservation, session=session, evidence=str(out)), recovery, cells)
    except NotRun:
        raise
    except Exception:
        raise NotRun('guest recovery failed; inspect '+str(recovery)+'; explicit recovery required') from None
    reservation.pop('retained', None)
    reservation.pop('reason', None)


@contextmanager
def lease(host, session, out, *, reservation=None):
    from tools.lab.desktop_lock import Desktop
    lock = Desktop(Path.home()/'putgd-lab/locks', host)
    save(out/'lock-acquire.json', lock.acquire('nightly-desktops', session, 'automatic Tier 1 nightly; '+str(out), 45))
    stop = threading.Event()
    errors = []
    def beat():
        while not stop.wait(240):
            try:
                lock.extend(session, 45)
            except Exception:
                errors.append('desktop lease renewal failed')
                return
    thread = threading.Thread(target=beat, daemon=True)
    thread.start()
    slot = Path.home()/'putgd-lab/locks/tier1-guest.lock.d'
    acquired = False
    try:
        slot.mkdir(mode=0o700)
        acquired = True
        (slot/'session').write_text(session+'\n')
        (slot/'session').chmod(0o600)
        owner = dict(reservation or {}, session=session, evidence=str(out), worker=worker_identity())
        save(slot/'owner.json', owner)
        yield errors
    finally:
        stop.set()
        thread.join()
        if acquired and reservation and reservation.get('retained'):
            # The guest may still hold a credential baton or private publication.
            # Keep the durable slot until explicit recovery, even after lease expiry.
            save(slot/'retained.json', dict(owner, **reservation))
            save(out/'lock-retained.json', lock.heartbeat(session, blocked=reservation['reason']))
        else:
            try:
                not_mine = controller_ssh_identities if host == 'd69' else []
                # Another session's large /tmp scratch is reported in the receipt but can't retain the slot.
                save(out/'lock-release.json', lock.release(session, verified_clean=True, not_mine=not_mine, tmp_blocks=False))
                if acquired:
                    (slot/'session').unlink()
                    (slot/'owner.json').unlink(missing_ok=True)
                    slot.rmdir()
            except Exception as error:
                if acquired:
                    save(slot/'retained.json', dict(owner, reason='desktop release failed; checked recovery required'))
                save(out/'lock-release-error.json', dict(type=type(error).__name__, message=str(error),
                                                        details=getattr(error, 'details', {})))
                raise


def preflight(config, row):
    from tools.lab import lab
    if not Path(row['golden']).is_file():
        if Path(row['golden']+'.partial').is_file():
            raise NotRun('golden copy awaiting checksum verification')
        raise NotRun('golden missing')
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        raise NotRun('builder KVM unavailable')
    profile = lab.load_profile(row['profile'])
    available = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                         if line.startswith('MemAvailable:'))) * 1024
    needed = profile['memory_mib']*2**20 + 4*2**30
    if available < needed:
        raise NotRun('RAM below guest requirement plus 4 GiB headroom')
    if os.getloadavg()[0] >= os.cpu_count():
        raise NotRun('load at or above core count')
    budget = config.get('overlayBudgetGiB', 4)
    if shutil.disk_usage(ROOT).free < (10+budget)*2**30:
        raise NotRun('disk below sparse overlay budget plus 10 GiB reserve')
    for port in (config.get('sshPort', 22446), 5900+config.get('vncDisplay', 46)):
        with socket.socket() as sock:
            try:
                sock.bind(('127.0.0.1', port))
            except OSError:
                raise NotRun('reserved nightly guest port is occupied') from None
    return dict(kvm=True, availableMemory=available, requiredMemory=needed,
                freeBytes=shutil.disk_usage(ROOT).free, overlayBudgetGiB=budget,
                overlay='sparse qcow2; no preallocation', placement='guest')


def await_host(config, out, row, *, clock=time.monotonic, sleep=time.sleep, worker_deadline=None):
    """Wait outside every desktop/Proton lease; other guests are never touched."""
    from tools.lab.desktop_lock import Desktop
    started = clock()
    deadline = started + config.get('waitSeconds', 1200)
    if worker_deadline is not None:
        deadline = min(deadline, worker_deadline)
    attempts = []
    while True:
        if worker_deadline is not None and clock() >= worker_deadline:
            raise NotRun('worker deadline exhausted while awaiting host')
        try:
            for resource in ('desktop', 'proton') if row.get('live') and row.get('proton', True) else ('desktop',):
                state = Desktop(Path.home()/'putgd-lab/locks', config['host'], resource=resource).status()
                if state['state'] != 'free':
                    raise NotRun(resource+' lease held by another session')
            if (Path.home()/'putgd-lab/locks/tier1-guest.lock.d').exists():
                raise NotRun('nightly guest slot held by another session')
            ready = preflight(config, row)
            save(out/'availability.json', dict(attempts=attempts, waitedSeconds=round(clock()-started, 3), ready=ready))
            return clock()-started
        except NotRun as error:
            attempts.append(dict(elapsedSeconds=round(clock()-started, 3), reason=str(error)))
            save(out/'availability.json', dict(attempts=attempts, waitedSeconds=round(clock()-started, 3)))
            if clock() >= deadline or str(error) in ('golden missing', 'builder KVM unavailable'):
                raise NotRun(str(error)+'; not run after '+str(round(clock()-started))+'s') from None
            sleep(min(30, max(0, deadline-clock())))


def keep_aside(path):
    """Copy an evidence file aside before a retirement replaces it; earlier copies are never overwritten."""
    if path.exists():
        shutil.copy2(path, path.with_name(path.name+'.before-recovery-'+uuid.uuid4().hex[:8]))


def put_back(source, target):
    """Make target byte-for-byte the kept copy source, keeping the current target aside."""
    keep_aside(target)
    temporary = target.with_name(target.name+'.restore-'+uuid.uuid4().hex[:8])
    shutil.copy2(source, temporary)
    temporary.replace(target)


def kept_copy(out, name, sha256):
    """The current file or a copy an earlier retirement kept aside whose bytes hash to sha256, or None."""
    for path in [out/name, *sorted(out.glob(name+'.before-recovery-*'))]:
        if path.is_file() and not path.is_symlink() and digest(path) == sha256:
            return path
    return None


def settle_earlier_manifest(run, out):
    """Put back the evidence an earlier retirement's cleanup receipt names (#1681). True when a receipt exists.

    Guarded cleanup records the hash of the completion manifest it checked in run/overlay-cleanup.json.
    A retirement interrupted after that writes no new evidence on retry: it re-verifies the manifest the
    receipt names. When a later retry already replaced the manifest or completion.json (an older controller,
    or the documented manual call of retire), the earlier attempt's exact bytes are still in the evidence as
    .before-recovery copies, so the retry puts those back. A manifest whose bytes are nowhere in the
    evidence is a foreign change and is refused here; a member file changed since is refused by cleanup.
    """
    receipt_path = run/'overlay-cleanup.json'
    if not os.path.lexists(receipt_path):
        return False
    manifest = out/'completion-manifest.json'
    try:
        named = json.loads(receipt_path.read_text()).get('evidence')
    except (OSError, ValueError, AttributeError):
        named = None
    if not (isinstance(named, dict) and named.get('manifest') == str(manifest)
            and isinstance(named.get('sha256'), str)):
        # Not a receipt for this evidence: guarded cleanup refuses it with its own reason.
        return True
    earlier = kept_copy(out, manifest.name, named['sha256'])
    if earlier is None:
        raise NotRun('guest retained: the cleanup receipt names a completion manifest that is not in the evidence; '
                     'inspect '+str(out)+' before recovery')
    files = json.loads(earlier.read_text()).get('files')
    proof = files.get('completion.json') if isinstance(files, dict) else None
    completion = kept_copy(out, 'completion.json', proof) if isinstance(proof, str) else None
    if completion is None:
        raise NotRun('guest retained: the cleanup receipt\'s completion manifest names a completion.json that is '
                     'not in the evidence; inspect '+str(out)+' before recovery')
    if earlier != manifest:
        put_back(earlier, manifest)
    if completion != out/'completion.json':
        put_back(completion, out/'completion.json')
    return True


def finish_retirement(run, out):
    """Guarded cleanup against the manifest an earlier retirement's receipt names."""
    from tools.lab import lab
    settle_earlier_manifest(run, out)
    save(out/'retirement.json', lab.cleanup_overlay(run, evidence_manifest=out/'completion-manifest.json',
         images_root=Path(lab.state(run)['golden']).parents[1]))


def archived_files(archive):
    """{member name: sha256} of an evidence archive's regular files."""
    with tarfile.open(archive) as stream:
        return {member.name: hashlib.sha256(stream.extractfile(member).read()).hexdigest()
                for member in stream.getmembers() if member.isfile()}


def attempt_files(out, attempts):
    """{relative name: sha256} of every regular file under the attempt directories."""
    files = {}
    for attempt in attempts:
        for path in attempt.rglob('*'):
            if path.is_file() and not path.is_symlink():
                files[str(path.relative_to(out))] = digest(path)
    return files


def earlier_archive(out, attempts):
    """The archive an interrupted earlier retirement already wrote and hashed, when it still holds exactly these attempts."""
    manifest = out/'completion-manifest.json'
    if not manifest.is_file():
        return None
    try:
        files = json.loads(manifest.read_text()).get('files', {})
    except (OSError, ValueError, AttributeError):
        return None
    current = None
    for name, sha256 in files.items() if isinstance(files, dict) else ():
        archive = out/name
        if (not re.fullmatch(r'attachments(-recovery-[0-9a-f]{8})?\.tgz', name) or not archive.is_file()
                or archive.is_symlink() or digest(archive) != sha256):
            continue
        current = attempt_files(out, attempts) if current is None else current
        try:
            if archived_files(archive) == current:
                return archive
        except (OSError, tarfile.TarError):
            continue
    return None


def retire(run, out, rows):
    from tools.lab import lab
    if settle_earlier_manifest(run, out):
        # Guarded cleanup already prepared or finished from earlier evidence: never write new evidence.
        lab.reset(run)
        finish_retirement(run, out)
        return
    info = lab.state(run)
    # Collect only terminal audited run directories. Rehash the archive before
    # writing the existing guarded cleanup manifest; never remove failed evidence.
    from tools.lab.surface_driver.collect import collect
    attempts = [p.parent for p in out.glob('*/receipt.json')]
    # An attempt killed before its terminal proof is archived nowhere and stays in place; its operator
    # record and receipt are hashed into the completion manifest instead.
    unaudited = sorted((a for a in attempts if unaudited_attempt(a)), key=lambda a: a.name)
    attempts = [a for a in attempts if a not in unaudited]
    # A retry after an interruption before guarded cleanup reuses the archive the earlier attempt wrote
    # and checked, so each retry does not add another copy of the row's evidence.
    archive = earlier_archive(out, attempts) if attempts else None
    if attempts and archive is None:
        archive = out/'attachments.tgz'
        if archive.exists():
            archive = out/('attachments-recovery-'+uuid.uuid4().hex[:8]+'.tgz')
        collect(attempts, archive)
        for name, sha256 in archived_files(archive).items():
            if sha256 != digest(out/name):
                raise RuntimeError('exported evidence checksum mismatch')
    for name in ('completion.json', 'completion-manifest.json'):
        keep_aside(out/name)
    completion = dict(schemaVersion=1, kind='vm-overlay-cleanup-evidence', run=str(run),
        runId=info['id'], completed=True, evidenceExportVerified=True,
        outcome='passed' if rows and not unaudited and all(passed(c) for c in rows.values()) else 'failed')
    if unaudited:
        completion['unauditedAttempts'] = [a.name for a in unaudited]
    save(out/'completion.json', completion)
    files = {'completion.json': digest(out/'completion.json')}
    for attempt in unaudited:
        for name in (UNAUDITED_ATTEMPT, 'receipt.json'):
            files[attempt.name+'/'+name] = digest(attempt/name)
    if archive is not None:
        files[archive.name] = digest(archive)
    save(out/'completion-manifest.json', dict(completion, files=files))
    lab.reset(run)
    finish_retirement(run, out)


def baton_returned(run):
    locks = Path.home()/'putgd-lab/locks'
    proton = locks/'proton.lock'
    if proton.exists() and 'lent copy outstanding' in proton.read_text():
        raise RuntimeError('Proton carry-back unresolved; overlay retained')
    last = json.loads((locks/'proton-registry.json').read_text()).get('lastLend', {})
    if last.get('run') == str(run) and last.get('carriedBack'):
        return True
    # lend may stop while preparing the guest, before exporting any session.
    return False


def package_path(artifacts, tag, row, identity):
    # CI candidates carry their original artifact names instead of release names.
    # The worker still verifies the exact package digest before installation.
    name = identity.get('packageFiles', {}).get(row['asset'], f'putgd-{tag[1:]}-{row["asset"]}')
    if not isinstance(name, str) or not name or Path(name).name != name or name in ('.', '..'):
        raise NotRun('invalid package artifact name')
    return artifacts/'packages'/name


def worker(config, tag, identity, artifacts, out):
    """Run a host worker with a cleanup-safe deadline and termination handler."""
    with worker_deadline() as deadline:
        return worker_run(config, tag, identity, artifacts, out, deadline)


def worker_run(config, tag, identity, artifacts, out, deadline):
    """Observe one configured desktop and retain ownership of unfinished cleanup."""
    from tools.lab import lab
    from tools.lab.nightly_desktop_guest import hold_package_updates, install_rpm, prepare, prepare_live_keyring
    out.mkdir(mode=0o700, parents=True)
    if command(['git', 'rev-parse', 'HEAD']).strip() != identity['driver']:
        raise NotRun('worker checkout differs from the controller source identity')
    # Live admission reads the classifier/pin from this exact app commit.
    if not re.fullmatch('[0-9a-f]{40}', identity['gitCommit']):
        raise NotRun('invalid tested source identity')
    command(['git', 'fetch', '--no-tags', 'origin', identity['gitCommit']], log=out/'build-source-fetch.txt')
    host = config['host']
    if not 1 <= len(config['rows']) <= MAX_ROWS:
        raise NotRun('configure one to '+str(MAX_ROWS)+' desktops per builder')
    result = []
    # Rows on one builder run one after another: one guest slot, one set of ports.
    for row in config['rows']:
        if STOP.get('reason'):
            # Terminated or out of budget during an earlier row: take no new lease, launch nothing.
            result += [dict(record, profile=row['profile'], release=tag, build=identity['gitCommit'])
                       for record in not_run_rows(host, dict(config, rows=[row]), 'row not started: '+STOP['reason'], out)]
            save(out/'results.json', result)
            continue
        # The previous row's cleanup disarmed the alarm; the rest of the shared budget bounds this one.
        # With nothing left, the row's own deadline checks (await_host) record it as not run.
        left = deadline - time.monotonic()
        if left > 0:
            signal.setitimer(signal.ITIMER_REAL, left)
        started = time.monotonic()
        rowout = out/row['name'].lower().replace(' ', '-')
        rowout.mkdir(mode=0o700)
        session = 'nightly-'+uuid.uuid4().hex[:12]
        cells = row_cells(row, 'desktop setup did not complete')
        record = dict(desktop=row['name'], host=host, profile=row['profile'], cells=cells,
                      release=tag, build=identity['gitCommit'], driver=identity['driver'],
                      session=session, evidence=str(rowout))
        run = ROOT/'work/lab-runs'/('n'+uuid.uuid4().hex[:8])
        reservation = dict(run=str(run))
        setup_started = None
        try:
            with recovery_deadline():
                recover_stale_guest(config, rowout)
            record['waitSeconds'] = round(await_host(config, rowout, row, worker_deadline=deadline), 3)
            with lease(host, session, rowout, reservation=reservation) as lease_errors:
                try:
                    setup_started = time.monotonic()
                    command(['uptime'], log=rowout/'uptime.txt')
                    save(rowout/'host-preflight.json', preflight(config, row))
                    command([sys.executable, 'tools/diagnostics/check_socket_paths.py', '--socket',
                             run/'qmp.sock', run/'vnc.sock', '/dev/shm/nd/runtime/putgd/session.sock'],
                            log=rowout/'sockets.json')
                    # Remap an existing authenticated pin, never scan/trust a new key.
                    pins = Path(row['knownHosts']).read_text().splitlines()
                    key = next(' '.join(p.split()[1:3]) for p in pins if ' ssh-ed25519 ' in p)
                    known = rowout/'known_hosts'
                    ssh_port = config.get('sshPort', 22446)
                    vnc_display = config.get('vncDisplay', 46)
                    known.write_text(f'[127.0.0.1]:{ssh_port} '+key+'\n')
                    command([sys.executable, 'tools/lab/lab.py', 'launch', '--profile', row['profile'],
                        '--golden', row['golden'], '--run', run, '--bootstrap-key', row['bootstrapKey'],
                        '--known-hosts', known, '--ssh-port', str(ssh_port), '--vnc-display', str(vnc_display), '--timeout', '420'],
                        log=rowout/'launch.log', timeout=600)
                    hold_package_updates(run, rowout)
                    package = package_path(artifacts, tag, row, identity)
                    if digest(package) != identity['packages'][package.name]:
                        raise RuntimeError('worker package checksum mismatch')
                    if row['package'] == 'rpm':
                        install_rpm(run, package, rowout)
                    else:
                        command([sys.executable, 'tools/lab/lab.py', 'install', '--run', run,
                            '--appimage' if row['package'] == 'appimage' else '--deb', package],
                            log=rowout/'install.log', timeout=1600)
                    profiles = prepare(run, row, rowout)
                    save(rowout/'identity.json', dict(lab.state(run), driver=identity['driver']))
                    record.update(runId=lab.state(run)['id'], goldenSha256=lab.state(run)['golden_sha256'],
                                  packageSha256=digest(package))
                    record['setupSeconds'] = round(time.monotonic()-setup_started, 3)
                    env = dict(os.environ, PUTGD_TIER1_SESSION=session, HOME=str(Path.home().resolve()))
                    keyring_ready = False
                    for flow in cells:
                        if cells[flow]['status'] == 'unsupported':
                            continue
                        flow_started = time.monotonic()
                        destination = 's3' if flow.startswith('s3_') else 'proton' if flow.startswith('proton_') else 'local'
                        if (destination == 's3' and row['desktop'] in ('aurora-kde-wayland', 'fedora-budgie-wayland')
                                and not keyring_ready):
                            # KWallet (Aurora) and oo7-daemon (Budgie) can't take the lab collection;
                            # one private keyring per guest serves both S3 cells.
                            prepare_live_keyring(run, rowout)
                            keyring_ready = True
                        recipe = ('outcomes.' if destination == 'local' else 'outcomes_live.')+flow
                        for attempt in (1, 2):
                            remaining_timeout(deadline)
                            if lease_errors:
                                raise RuntimeError(lease_errors[0])
                            name = flow+'-'+str(attempt)
                            target = rowout/name
                            runtime = '/dev/shm/n'+uuid.uuid4().hex[:8]
                            command([sys.executable, 'tools/diagnostics/check_socket_paths.py', '--socket',
                                     target/'runtime/putgd/session.sock', runtime+'/runtime/putgd/session.sock'],
                                    log=rowout/(name+'-sockets.json'))
                            args = [sys.executable, '-m', 'tools.lab.surface_driver.run', '--desktop', row['desktop'],
                                '--host', host, '--placement', 'guest', '--guest', row['profile'], '--run', str(run),
                                '--package', row['package'], '--destination', destination, '--recipes', recipe,
                                '--catalog', str(artifacts/'catalog'), '--build', identity['gitCommit'],
                                '--profiles', str(profiles), '--session-root', runtime,
                                '--out', str(target), '--campaign', 'nightly-desktops',
                                '--coverage', str(rowout/(name+'-coverage.json'))]
                            if destination == 'proton':
                                proton_command = config.get('protonCommand', [sys.executable, str(ROOT/'tools/lab/proton_lab.py')])
                                check = [*proton_command, 'check', '--session', session,
                                         '--owner', 'nightly-desktops']
                                command(check, log=rowout/(name+'-check-before.json'), env=env)
                                # The host adapter selects the native check app. Lending
                                # must use this exact driver's guest/bridge implementation,
                                # not the adapter's older installed copies. Canonical HOME
                                # above preserves Atomic's /var/home lab profile identity.
                                args = [sys.executable, str(ROOT/'tools/lab/proton_lab.py'), 'lend', str(run),
                                        '--session', session, '--owner', 'nightly-desktops',
                                        '--purpose', 'automatic nightly outcome', '--']+args
                            # The driver exits nonzero for functional reds. Collect its
                            # terminal result anyway; never infer success from an exit alone.
                            try:
                                attempt_timeout = remaining_timeout(deadline)
                                command(['nice', '-n', '10', *args], log=rowout/(name+'.log'), env=env,
                                        timeout=attempt_timeout)
                            except NotRun:
                                raise
                            except (RuntimeError, subprocess.TimeoutExpired):
                                pass
                            observed = verdict(target, recipe)
                            observed['attempt'] = attempt
                            cells[flow].setdefault('attempts', []).append(observed)
                            cells[flow].update(observed)
                            if destination == 'proton':
                                # lend must have returned the baton even when assertions failed.
                                returned = baton_returned(run)
                                command(check, log=rowout/(name+'-check-after.json'), env=env)
                                if not returned:
                                    raise NotRun('Proton guest provisioning did not complete; source check passed')
                            marker_left = (run/lab.LIVE_CLEANUP_MARKER).exists()
                            if marker_left or lost_guest_session(target):
                                # The failing cell is the row's reported result (#1214). A live cleanup that is
                                # still unresolved stops the row the same way: no retry or Proton lend on this guest.
                                note = failed_recipe_note(target, observed.get('assertion', ''))
                                what = 'live remote cleanup unresolved' if marker_left else 'guest session lost'
                                reason = f'{what} in {flow} attempt {attempt}: {note}'
                                record['stoppedEarly'] = dict(flow=flow, attempt=attempt, reason=reason)
                                record['notRunReason'] = reason
                                cells[flow]['assertion'] = reason
                                for value in cells.values():
                                    if not value.get('attempts'):
                                        value['assertion'] = 'row stopped; '+reason
                                break
                            if observed['status'] == 'green':
                                break
                        cells[flow]['seconds'] = round(time.monotonic()-flow_started, 3)
                        if record.get('stoppedEarly'):
                            break
                finally:
                    # Work is over. Do not deliver a budget alarm in retirement.
                    signal.setitimer(signal.ITIMER_REAL, 0)
                    reservation.update(retained=True, reason='guest cleanup incomplete; explicit recovery required')
                    # A failed live cleanup may own the only current baton or a
                    # remote publication. Preserve that running guest for recover.
                    proton = Path.home()/'putgd-lab/locks/proton.lock'
                    # Only a row that lends Proton can hold its baton; another session's lend isn't this guest's.
                    outstanding = (bool(row.get('live')) and row.get('proton', True) and proton.exists()
                                   and 'lent copy outstanding' in proton.read_text())
                    retained = False
                    for receipt_path in rowout.glob('*/receipt.json'):
                        evidence = json.loads(receipt_path.read_text())
                        environment = evidence.get('environment', {})
                        retained |= bool(environment.get('cleanup_errors') or environment.get('unresolved_live_cleanup'))
                        retained |= 'retained' in str(environment.get('private_cleanup', ''))
                    # The marker outlives a killed runner, which writes no receipt flag.
                    retained |= (run/lab.LIVE_CLEANUP_MARKER).exists()
                    if outstanding or retained:
                        reservation.update(retained=True, reason='guest retained; explicit cleanup recovery required')
                        recover_row(config, reservation, session, rowout, cells)
                    if (run/'state.json').exists() and not outstanding and not retained:
                        try:
                            try:
                                if (run/'disk.qcow2').exists():
                                    retire(run, rowout, cells)
                            finally:
                                lab.reset(run)
                        except Exception:
                            reservation.update(retained=True, reason='guest cleanup failed; explicit recovery required')
                            recover_row(config, reservation, session, rowout, cells)
                    if not outstanding and not retained:
                        reservation.pop('retained', None)
                        reservation.pop('reason', None)
        except Exception as error:
            # Only our fixed messages are public. Harness/SSH exception payloads stay private.
            reason = str(error) if isinstance(error, NotRun) or type(error) is RuntimeError else 'controller '+type(error).__name__+'; private evidence retained'
            if record.get('stoppedEarly'):
                reason = record['stoppedEarly']['reason']+'; '+reason
            record['notRunReason'] = reason
            for value in cells.values():
                if value['status'] == 'not_run' and not value.get('attempts'):
                    value.update(assertion=reason)
        if reservation.get('retained') or (rowout/'lock-release-error.json').exists():
            reason = record.get('notRunReason') or reservation.get('reason') or 'desktop release failed; inspect lock-release-error.json'
            record['notRunReason'] = reason
            record['cleanupBlocked'] = True
            for value in cells.values():
                if value['status'] != 'unsupported':
                    value.update(status='not_run', green=False, assertion=reason)
        if (rowout/'availability.json').exists():
            record['waitSeconds'] = json.loads((rowout/'availability.json').read_text()).get('waitedSeconds', 0)
        record['seconds'] = round(time.monotonic()-started, 3)
        record['finishedAt'] = dt.datetime.now(dt.timezone.utc).isoformat()
        save(rowout/'result.json', record)
        result.append(record)
        # A finished row stays reported even if a later row's watchdog ends the worker.
        save(out/'results.json', result)
        print(json.dumps(dict(desktop=row['name'], green=sum(c['green'] for c in cells.values()), total=len(cells))), flush=True)
    save(out/'results.json', result)
    return result


def host_blocked(row):
    """The builder's reason when no cell of ROW ran because of its host, else None."""
    cells = [value for value in row['cells'].values() if value['status'] != 'unsupported']
    if not cells or any(value['status'] != 'not_run' or value.get('attempts') for value in cells):
        return None
    reason = row.get('notRunReason') or next((value['assertion'] for value in cells if value.get('assertion')), '')
    return reason if any(part.strip().startswith(HOST_REASONS) for part in reason.split(';')) else None


def last_results(state):
    """The results the last controller run published, or None."""
    try:
        return json.loads(Path(json.loads((state/'last-run.json').read_text())['results']).read_text())
    except (OSError, ValueError, KeyError, TypeError):
        return None


def count_host_blocked(result, last):
    """Give each row its builder kept from running a count of its published runs in a row so
    blocked, from the last published results. A run on the UTC date the row was last counted
    (`hostBlockedDate`, kept with the row when a filtered recheck carries it over) adds none."""
    today = str(result.get('date', ''))[:10]
    try:
        before = {(row['host'], row['desktop']): row for row in (last or {}).get('desktops', [])}
        published = str((last or {}).get('date', ''))[:10]
    except (AttributeError, KeyError, TypeError):
        before, published = {}, ''
    for row in result['desktops']:
        row.pop('hostBlockedNights', None)
        row.pop('hostBlockedDate', None)
        if not host_blocked(row):
            continue
        previous = before.get((row['host'], row['desktop']))
        try:
            blocked_before = bool(previous) and bool(host_blocked(previous))
            earlier = int(previous.get('hostBlockedNights') or 1) if blocked_before else 0
            counted = str(previous.get('hostBlockedDate') or published)[:10] if blocked_before else ''
        except (AttributeError, KeyError, TypeError, ValueError):
            earlier, counted = 0, ''
        row['hostBlockedNights'] = max(1, earlier if counted == today else earlier+1)
        row['hostBlockedDate'] = today


def host_alert(row):
    """One line for a row its builder has kept from running for HOST_BLOCKED_ALERT nights or more."""
    nights = row.get('hostBlockedNights', 0)
    if nights < HOST_BLOCKED_ALERT:
        return None
    return (f'{row["desktop"]} / {row["host"]} ran no cell for {nights} nights running: {host_blocked(row)}. '
            'The builder needs attention before the next nightly; tell the lab coordinator.')


def table(result):
    lines = ['| Desktop / host | '+' | '.join(FLOWS.values())+' | Not run |',
             '| --- | '+' | '.join('---' for _ in FLOWS)+' | --- |']
    for row in result['desktops']:
        values = []
        reasons = []
        for flow in FLOWS:
            value = row['cells'].get(flow)
            if value is None:
                values.append('—')
                continue
            color = STATUS_MARK[value['status']]
            values.append(f'[{color}]({value["receiptUrl"]})' if value.get('receiptUrl') else color)
            if value['status'] == 'not_run' and value.get('assertion'):
                reasons.append(value['assertion'])
        reason = row.get('notRunReason') or '; '.join(dict.fromkeys(reasons))
        if row.get('hostBlockedNights', 0) >= HOST_BLOCKED_ALERT:
            reason = f'⚠️ {row["hostBlockedNights"]} nights running: '+reason
        reason = reason.replace('|', '/').replace('\n', ' ')[:500]
        lines.append('| '+row['desktop']+' / '+row['host']+' | '+' | '.join(values)+' | '+reason+' |')
    return '\n'.join(lines)+'\n'


def select_release(releases, completed=(), force=False):
    published = sorted((r for r in releases if not r['draft'] and TAG.fullmatch(r['tag_name'])),
                       key=lambda r:r['published_at'])
    latest = published[-1]
    return latest if force or latest['tag_name'] not in completed else None


def tested_release(path):
    try:
        marker = json.loads(Path(path).read_text())
        return marker.get('schemaVersion') == 2 and marker.get('eligibleDesktops', 0) >= 3
    except (OSError, ValueError, TypeError):
        return False


def eligible_desktops(result):
    return sum(any(value['status'] in ('red', 'green') and value.get('terminal')
                   and value.get('observedBuild') == result['build']
                   for value in row['cells'].values()) for row in result['desktops'])


def publish(github, result, out, *, previous=None):
    if result.get('stableDraft'):
        # Acceptance attachments stay private/local until a person attaches them.
        # Do not update #1003, pin an issue, or file/close nightly regressions.
        for row in result['desktops']:
            # The slug is for people; the digest of the exact row name keeps distinct rows apart.
            slug = re.sub(r'[^a-z0-9]+', '-', row['desktop'].lower()).strip('-')
            save(out/(row['host']+'-'+slug+'-'+hashlib.sha256(row['desktop'].encode()).hexdigest()[:8]+'-receipt.json'), row)
        (out/'results.md').write_text(f'Stable draft: **{result["release"]}** · {result["date"]}\n\n'+table(result))
        save(out/'results.json', result)
        return
    issues = github.issues()
    title = 'Nightly desktop results'
    pinned = next((i for i in issues if i['title'] == title), None)
    if not pinned:
        pinned = github.issue(title, 'Automatic nightly results are being published.')
    result['issueUrl'] = pinned['html_url']
    for row in result['desktops']:
        compact = {}
        for flow, value in row['cells'].items():
            compact[flow] = {k:value[k] for k in ('status','assertion','seconds','observedBuild','terminal','receipt','receiptSha256','memory') if k in value}
            compact[flow]['attempts'] = [{k:a[k] for k in ('status','assertion','terminal','receipt','receiptSha256','attempt','memory') if k in a}
                                         for a in value.get('attempts', [])]
        receipt = dict(desktop=row['desktop'], host=row['host'], release=result['release'],
            build=result['build'], cells=compact, evidence=row['evidence'],
            identity={k:row.get(k) for k in ('profile','driver','session','runId','goldenSha256','packageSha256')},
            seconds=row.get('totalSeconds', row.get('seconds')), waitSeconds=row.get('waitSeconds', 0),
            attachments=row['evidence']+'/attachments.tgz', notRunReason=row.get('notRunReason'))
        if row.get('hostBlockedNights'):
            receipt.update(hostBlockedNights=row['hostBlockedNights'], hostBlockedDate=row.get('hostBlockedDate'))
        alert = host_alert(row)
        body = f'{result["release"]} · {row["desktop"]} / {row["host"]}\n\n'+(f'> [!WARNING]\n> {alert}\n\n' if alert else '')
        body += '```json\n'+json.dumps(receipt, indent=2, ensure_ascii=False)+'\n```'
        comment = github.api(f'repos/{github.repo}/issues/{pinned["number"]}/comments', 'POST', dict(body=body))
        for value in row['cells'].values():
            value['receiptUrl'] = comment['html_url']
        # Exactly one stable tracking issue per desktop; grey never creates,
        # reopens or closes an issue, and legacy per-flow issues stay untouched.
        failure_title = f'nightly: {row["desktop"]} failing'
        existing = next((i for i in issues if i['title'] == failure_title), None)
        failed = [(flow,value) for flow,value in row['cells'].items() if value['status'] == 'red']
        if failed:
            body = f'Automatic app-flow assertions failed on `{result["release"]}`.\n\n'
            body += '\n'.join(f'- **{FLOWS[flow]}**: '+value['assertion'] for flow,value in failed)
            body += f'\n\n[Desktop receipt]({comment["html_url"]})'
            issue = github.issue(failure_title, body, existing)
            github.api(f'repos/{github.repo}/issues/{issue["number"]}/labels', 'POST',
                       dict(labels=['release-blocker']))
            row['issueUrl'] = issue['html_url']
        elif existing and existing['state'] == 'open' and all(passed(v) for v in row['cells'].values()):
            github.api(f'repos/{github.repo}/issues/{existing["number"]}/comments', 'POST',
                dict(body=f'All enabled flows recovered on `{result["release"]}`. [Receipt]({comment["html_url"]}).'))
            # A filtered recheck is one row's second look; only a full run closes. Red reopens it.
            if previous is None:
                github.api(f'repos/{github.repo}/issues/{existing["number"]}', 'PATCH', dict(state='closed'))
    if previous is not None:
        result['desktops'] = merge_desktops(previous, result)
    markdown = f'Release: **{result["release"]}** · {result["date"]}\n\n'
    alerts = [line for line in map(host_alert, result['desktops']) if line]
    if alerts:
        markdown += '> [!WARNING]\n'+''.join(f'> {line}\n' for line in alerts)+'\n'
    markdown += table(result)
    markdown += '\n🟢 automatic oracles passed · 🔴 app-flow oracle failed on the verified installed build · ⚪ infrastructure or documented manager-free X11 loss (no issue) · ➖ not supported on this desktop (acceptance matrix) · — disabled.\n'
    markdown += '\n| Desktop / host | Row seconds | Host total seconds | Capacity/lease wait seconds |\n| --- | ---: | ---: | ---: |\n'
    for row in result['desktops']:
        markdown += (f'| {row["desktop"]} / {row["host"]} | {row.get("seconds", 0):.1f} | '
                     f'{row.get("totalSeconds", row.get("seconds", 0)):.1f} | {row.get("waitSeconds", 0):.1f} |\n')
    markdown += '\nFrames remain evidence attachments; no manual visual review is required.\n'
    github.issue(title, markdown, pinned)
    query = 'query($id:ID!){node(id:$id){... on Repository{pinnedIssues(first:10){nodes{issue{id}}}}}}'
    repository = github.api(f'repos/{github.repo}')
    pins = json.loads(github.gh('api', 'graphql', '-f', 'query='+query, '-f', 'id='+repository['node_id']))
    if pinned['node_id'] not in [n['issue']['id'] for n in pins['data']['node']['pinnedIssues']['nodes']]:
        github.gh('api', 'graphql', '-f', 'query=mutation($id:ID!){pinIssue(input:{issueId:$id}){issue{id}}}', '-f', 'id='+pinned['node_id'])
    (out/'results.md').write_text(markdown)
    save(out/'results.json', result)


def not_run_rows(host, builder, reason, out):
    return [dict(desktop=row['name'], host=host, evidence=str(out), notRunReason=reason,
                 cells=row_cells(row, reason))
            for row in builder['rows']]


def selected_builders(builders, desktops):
    """The builders to run, each with only the rows named (a builder ID names all of its rows)."""
    if not desktops:
        return builders
    selected = {}
    for name in desktops:
        key = name.casefold()
        matches = {host: [r for r in builder['rows'] if key == host.casefold()
                          or key in (r['name'].casefold(), r['desktop'].casefold())]
                   for host, builder in builders.items()}
        matches = {host: rows for host, rows in matches.items() if rows}
        if len(matches) != 1:
            raise RuntimeError('desktop filter must identify exactly one configured builder: '+name)
        for host, rows in matches.items():
            chosen = selected.setdefault(host, [])
            chosen += [r for r in rows if r not in chosen]
    return {host: dict(builder, rows=[r for r in builder['rows'] if r in selected[host]])
            for host, builder in builders.items() if host in selected}


def merge_desktops(previous, result):
    if (previous['release'], previous['build']) != (result['release'], result['build']):
        raise RuntimeError('filtered publication requires the same release and exact build')
    replacements = {(r['host'], r['desktop']): r for r in result['desktops']}
    if not replacements.keys() <= {(r['host'], r['desktop']) for r in previous['desktops']}:
        raise RuntimeError('filtered publication cannot introduce an unknown desktop')
    return [replacements.get((r['host'], r['desktop']), r) for r in previous['desktops']]


def follow_nightly(argv):
    """The timer tests with the newest merged driver, not whatever was last deployed.

    A failed update keeps the current checkout: an older driver still gives a result.
    """
    root = str(ROOT)
    try:
        before = command(['git', '-C', root, 'rev-parse', 'HEAD']).strip()
        command(['git', '-C', root, 'fetch', '-q', '--no-tags', 'origin', 'nightly'])
        command(['git', '-C', root, 'merge', '-q', '--ff-only', 'FETCH_HEAD'])
        after = command(['git', '-C', root, 'rev-parse', 'HEAD']).strip()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(json.dumps(dict(followNightly='kept current checkout', reason=str(error))), flush=True)
        return
    if after != before:
        os.execv(sys.executable, [sys.executable, *argv])


def sync_checkout(builder, driver):
    """Workers refuse any commit but the controller's, so bring each one to it."""
    git = ['git', '-C', builder['root']]
    # A commit copied here by hand may not be on origin; being on it is enough.
    update = '[ "$('+shlex.join([*git, 'rev-parse', 'HEAD'])+')" = '+shlex.quote(driver)+' ] || { '
    update += shlex.join([*git, 'fetch', '-q', '--no-tags', 'origin', driver])
    update += ' && '+shlex.join([*git, 'merge', '-q', '--ff-only', driver])+'; }'
    try:
        command([*builder['ssh'], update], timeout=300)
    except (RuntimeError, subprocess.TimeoutExpired):
        raise NotRun('builder checkout cannot reach the controller driver') from None


def removal_command(inputs, archive):
    """The shell command that deletes only what this run unpacked; each night's packages would fill the builder."""
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
    names = [shlex.quote(inputs+'/'+m.name) for m in members if not m.isdir()]
    folders = [m.name for m in members if m.isdir()]
    folders = [shlex.quote(inputs+'/'+name) for name in sorted(folders, key=len, reverse=True)]
    return ' && '.join(['rm -f -- '+' '.join(names), 'rmdir -- '+' '.join([*folders, shlex.quote(inputs)])])


def run_host(host, builder, tag, artifacts, archive, out, *, local_workers=None, controller=None):
    """One foreground worker process per host, including d69 itself."""
    started = time.monotonic()
    started_at = dt.datetime.now(dt.timezone.utc).isoformat()
    ssh = builder.get('ssh', [])
    root = builder['root']
    output = str(Path(root).parent/'ne'/uuid.uuid4().hex[:8])
    waited = 0
    inputs = None
    try:
        if ssh:
            deadline = time.monotonic()+builder.get('waitSeconds', 1200)
            while True:
                try:
                    command([*ssh, 'true'], timeout=25)
                    break
                except (RuntimeError, subprocess.TimeoutExpired):
                    if time.monotonic() >= deadline:
                        raise NotRun('SSH unreachable after 20-minute wait') from None
                    time.sleep(min(30, max(0, deadline-time.monotonic())))
            waited = time.monotonic()-started
            sync_checkout(builder, json.loads((artifacts/'identity.json').read_text())['driver'])
            inputs = root+'/inputs/'+out.name
            command([*ssh, 'mkdir -p '+shlex.quote(inputs)])
            with archive.open('rb') as data:
                transfer = subprocess.run([*ssh, 'tar -xf - -C '+shlex.quote(inputs)], stdin=data, capture_output=True, timeout=600)
            if transfer.returncode:
                raise NotRun('artifact transfer failed')
            selected = inputs+'/artifacts'
        else:
            selected = str(artifacts)
        args = ['nice','-n','10','python3',root+'/tools/lab/nightly_desktops.py', '--worker',host,
                '--config',builder['config'], '--artifacts',selected, '--output',output,
                *[a for r in builder.get('rows', []) for a in ('--row', r['name'])], tag]
        # A remote watchdog survives loss of the SSH client. Signal only the
        # worker (foreground), allowing its own finally blocks to retire guests.
        remote = ['timeout', '--foreground', '--signal=TERM', f'--kill-after={TERMINATE_GRACE}',
                  str(HOST_TIMEOUT), *args]
        env = dict(os.environ, PUTGD_NIGHTLY_CONTROLLER=json.dumps(controller)) if controller and not ssh else None
        worker_error = None
        # On a remote builder the same shell removes the unpacked packages once its worker has exited,
        # however the SSH connection fares; the worker's own status is kept.
        remote_line = (shlex.join(remote)+'; status=$?; '+removal_command(inputs, archive)+'; exit $status') if ssh else None
        try:
            command([*ssh, remote_line] if ssh else args, log=out/(host+'.log'), env=env,
                    timeout=HOST_TIMEOUT + TERMINATE_GRACE + 30 if ssh else HOST_TIMEOUT,
                    terminate_grace=TERMINATE_GRACE, local_workers=local_workers if not ssh else None)
        except (RuntimeError, subprocess.TimeoutExpired) as error:
            if not builder.get('rows'):
                raise
            # The worker saves each row as it finishes: keep those, whatever ended the rest.
            worker_error = ('worker timed out' if isinstance(error, subprocess.TimeoutExpired)
                            else 'worker exited with an error') + ' before reporting this row'
        try:
            text = command([*ssh, 'cat '+shlex.quote(output+'/results.json')]) if ssh else Path(output+'/results.json').read_text()
            reported = json.loads(text)
        except (RuntimeError, subprocess.TimeoutExpired, OSError, ValueError):
            if worker_error is None:
                raise
            reported = []
        rows = selected_rows(host, builder, reported, worker_error, out/host)
    except Exception as error:
        reason = str(error) if isinstance(error, NotRun) else 'worker infrastructure failed: '+type(error).__name__
        rows = not_run_rows(host, builder, reason, out/host)
    for row in rows:
        row.update(totalSeconds=round(time.monotonic()-started,3), startedAt=started_at)
        row['waitSeconds'] = round(row.get('waitSeconds', 0)+waited, 3)
    return rows


def selected_rows(host, builder, reported, missing_reason, out):
    """The worker's rows the controller selected, in its order; each selected row it didn't report is not run.

    A row it reports that wasn't selected (it has no package for it) is never published.
    """
    if not builder.get('rows'):
        return reported
    names = [r['name'] for r in builder['rows']]
    found = {}
    for row in reported if isinstance(reported, list) else []:
        if isinstance(row, dict) and row.get('desktop') in names:
            found.setdefault(row['desktop'], row)
    stray = [r.get('desktop') for r in reported if isinstance(r, dict) and r.get('desktop') not in names] \
        if isinstance(reported, list) else []
    reason = missing_reason or ('worker reported other rows than the controller selected' if stray
                                else 'worker reported no result for this row')
    rows = []
    for row in builder['rows']:
        rows += [found[row['name']]] if row['name'] in found else \
            not_run_rows(host, dict(builder, rows=[row]), reason, out)
    return rows


def run_hosts(builders, tag, artifacts, out, result, dispatch=run_host, *, local_workers=None):
    archive = out/'inputs.tar'
    with tarfile.open(archive, 'w') as stream:
        stream.add(artifacts, arcname='artifacts')
    from tools.lab.desktop_lock import process_start
    controller = dict(pid=os.getpid(), start=process_start(os.getpid()),
                      ssh=[b['ssh'] for host, b in builders.items() if host != 'd69' and b.get('ssh')])
    by_host = {}
    with ThreadPoolExecutor(max_workers=len(builders)) as pool:
        kwargs = dict(controller=controller)
        if local_workers is not None:
            kwargs['local_workers'] = local_workers
        futures = {pool.submit(dispatch, host, builder, tag, artifacts, archive, out, **kwargs):host
                   for host,builder in builders.items()}
        for future in as_completed(futures):
            host = futures[future]
            by_host[host] = future.result()
            result['desktops'] = [row for name in builders for row in by_host.get(name, [])]
            save(out/'results.json', result)
            print(json.dumps(dict(host=host, completed=True, seconds=by_host[host][0].get('totalSeconds'))),flush=True)
    return result


def controller(args, config, *, local_workers=None):
    state = Path(config.get('state', str(Path.home()/'putgd-lab/nightly')))
    stable_draft = args.stable_draft
    if stable_draft and (not args.tag or not STABLE_TAG.fullmatch(args.tag)):
        raise RuntimeError('--stable-draft requires a named stable tag')
    state.mkdir(parents=True, mode=0o700, exist_ok=True)
    with (state/'controller.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if stable_draft:
            state = state/'stable-drafts'/args.tag
            state.mkdir(parents=True, mode=0o700, exist_ok=True)
        if not args.now and not args.tag:
            follow_nightly(sys.argv)
        github = GitHub(config['repository'],
                        args.draft_token_file if stable_draft else args.config.parent/'github-token')
        if args.tag:
            if not (STABLE_TAG if stable_draft else TAG).fullmatch(args.tag):
                raise RuntimeError('expected a published nightly release tag')
            release = (draft_release(github, args.tag) if stable_draft
                       else github.api(f'repos/{github.repo}/releases/tags/{args.tag}'))
        else:
            release = select_release(github.api(f'repos/{github.repo}/releases?per_page=100'),
                                     {p.stem for p in state.glob('v*-nightly.json') if tested_release(p)}, args.now)
            if release is None:
                print('No new published nightly')
                return
        tag = release['tag_name']
        validate_release(release, tag, stable_draft)
        done = state/(tag+'.json')
        if tested_release(done) and not args.now and not stable_draft:
            print('Already tested '+tag)
            return
        if set(config['builders']) != set(HOSTS) or any(not 1 <= len(b['rows']) <= MAX_ROWS
                                                        for b in config['builders'].values()):
            raise RuntimeError('configure one to '+str(MAX_ROWS)+' desktops on each of the five builders')
        builders = selected_builders(config['builders'], args.desktops)
        previous = None
        if args.desktops and not stable_draft:
            marker = json.loads((state/'last-run.json').read_text())
            previous = json.loads(Path(marker['results']).read_text())
            if previous['release'] != tag:
                raise RuntimeError('filtered run requires the currently published table release')
        out = state/(tag+'-'+uuid.uuid4().hex[:8])
        out.mkdir(mode=0o700)
        artifacts = out/'artifacts'
        artifacts.mkdir(mode=0o700)
        result = dict(release=tag,build='unverified',date=dt.datetime.now(dt.timezone.utc).isoformat(),desktops=[],
                      stableDraft=stable_draft)
        try:
            rows = [row for b in builders.values() for row in b['rows']]
            identity = release_assets(github, tag, artifacts/'packages', rows,
                                      **(dict(release=release) if stable_draft else {}))
            identity['packages'] = {p.name:digest(p) for p in (artifacts/'packages').iterdir() if p.suffix in ('.deb','.rpm','.AppImage')}
            if stable_draft:
                result['assetChecksums'] = identity['packages']
                result['checksumManifestSha256'] = digest(artifacts/'packages/SHA256SUMS')
            identity['driver'] = command(['git','rev-parse','HEAD']).strip()
            identity['catalog'] = catalog(github, identity['gitCommit'], artifacts/'catalog', staged=stable_draft)
            save(artifacts/'identity.json', identity)
            result['build'] = identity['gitCommit']
            if previous is not None:
                merge_desktops(previous, result)  # Fail before launching any guest on identity mismatch.
        except Exception as error:
            reason = str(error) if type(error) is RuntimeError else 'release preparation failed: '+type(error).__name__
            if previous is not None:
                raise  # Preserve the published table if release preparation was not verified.
            result['desktops'] = [row for host,builder in builders.items()
                                  for row in not_run_rows(host,builder,reason,out)]
        else:
            run_hosts(builders,tag,artifacts,out,result,local_workers=local_workers)
        result['eligibleDesktops'] = eligible_desktops(result)
        if not stable_draft:
            count_host_blocked(result, last_results(state))
        publish(github,result,out,previous=previous)
        save(state/'last-run.json',dict(results=str(out/'results.json'),eligibleDesktops=result['eligibleDesktops']))
        # Grey is not a tested release. A later daily check may try it again.
        if not stable_draft and not args.desktops and result['eligibleDesktops'] >= 3:
            save(done,dict(schemaVersion=2,eligibleDesktops=result['eligibleDesktops'],
                           issueUrl=result['issueUrl'],results=str(out/'results.json')))
        print(str(out/'results.md') if stable_draft else result['issueUrl'])

def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', nargs='?')
    parser.add_argument('--stable-draft', action='store_true', help='manually test exact assets of a named stable draft; save separate local acceptance receipts')
    parser.add_argument('--draft-token-file', type=Path, help='owned mode-0600 token with push access for --stable-draft; the unattended read-only token cannot see drafts')
    parser.add_argument('--now', action='store_true', help='manual trigger, including rechecking the same release')
    parser.add_argument('--desktops', nargs='+', help='recheck only these desktop names, profile names or builder IDs; preserve other rows')
    parser.add_argument('--config', type=Path, default=Path.home()/'.config/putgd-nightly/config.json')
    parser.add_argument('--worker', choices=HOSTS, help=argparse.SUPPRESS)
    parser.add_argument('--artifacts', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--row', action='append', default=[], help=argparse.SUPPRESS)
    parser.add_argument('--output', type=Path, help=argparse.SUPPRESS)
    operator = parser.add_argument_group('a nightly guest kept for live cleanup (docs/lab/remote-builders.md); run on its builder')
    operator.add_argument('--resolve-live-cleanup', metavar='RUN', help='record that you confirmed the remote object of run RUN (n<8 hex>) is gone, then remove the marker')
    operator.add_argument('--no-live-publication', action='store_true', help='with --resolve-live-cleanup: the row has no marker and no receipt flag; record only the --guest-only-error values you checked')
    operator.add_argument('--destination', help='when the marker is already gone')
    operator.add_argument('--prefix', help='when the marker is already gone')
    operator.add_argument('--object-key', help='when the marker is already gone')
    operator.add_argument('--guest-only-error', action='append', default=[], metavar='ERROR', help='an exact receipt cleanup error you checked is held only in the guest')
    operator.add_argument('--check-live-cleanup', action='store_true', help='pause the guest VM so nothing in it can publish; for S3, then check that the object, its prefix and its versions are gone')
    operator.add_argument('--unaudited-attempt', action='append', default=[], metavar='NAME', help='record an attempt killed before its terminal proof as unaudited')
    operator.add_argument('--recover-retained', metavar='RUN', help='recover the retained slot of run RUN now, with every check the next nightly makes, and release its desktop lease')
    args = parser.parse_args()
    given = dict(destination=args.destination, prefix=args.prefix, objectKey=args.object_key)
    if any(given.values()) and not all(given.values()):
        parser.error('--destination, --prefix and --object-key go together')
    for run_name in (args.recover_retained, args.resolve_live_cleanup):
        if run_name is not None and not re.fullmatch(r'n[0-9a-f]{8}', run_name):
            parser.error('RUN is a nightly run directory name such as n1234abcd')
    if args.recover_retained is not None and (args.check_live_cleanup or args.resolve_live_cleanup or args.unaudited_attempt
                                              or any(given.values()) or args.guest_only_error or args.no_live_publication):
        parser.error('--recover-retained runs alone, after the records it needs')
    if args.no_live_publication and (not args.resolve_live_cleanup or any(given.values()) or args.check_live_cleanup):
        parser.error('--no-live-publication goes with --resolve-live-cleanup alone and names no publication')
    if args.check_live_cleanup:
        from tools.lab import lab
        try:
            owner = retained_owner()
            run, _ = owned_overlay(owner)
            # An absence seen while an orphaned recipe or lend can still publish proves nothing.
            if guest_clients_live(run):
                raise NotRun('a lend or recipe for this run is still live; wait until it has stopped')
            marker = run/lab.LIVE_CLEANUP_MARKER
            if marker.exists():
                details = json.loads(private_file(marker).read_text())
                publication = {k: details.get(k) for k in ('destination', 'prefix', 'objectKey')}
                if all(given.values()) and given != publication:
                    raise NotRun('the given publication differs from the marker in '+str(marker))
            elif all(given.values()):
                publication = given
            else:
                raise NotRun('no live cleanup marker in '+str(run)+'; give --destination, --prefix and --object-key')
            # Nothing in the guest may publish after this point; the destination check follows the pause.
            paused = guest_paused(run, pause=True)
            if paused is None:
                raise NotRun('could not pause the guest VM; keep the guest')
            # Let an upload that had already reached the destination commit before reading it.
            time.sleep(max(0, paused+PAUSE_SETTLE_SECONDS-time.time()))
            if guest_paused(run) != paused:
                raise NotRun('the guest VM was resumed during the wait; run --check-live-cleanup again')
            result = dict(guestPaused=True)
            if pause_record(run, 'exited') == paused:
                result['guestExited'] = True
            if publication['destination'] == 's3':
                result.update(check_s3_absent(publication))
        except NotRun as error:
            raise SystemExit(str(error)) from None
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if all(result.values()) else 1)
    if args.resolve_live_cleanup or args.unaudited_attempt:
        if (any(given.values()) or args.guest_only_error) and not args.resolve_live_cleanup:
            parser.error('publication and --guest-only-error need --resolve-live-cleanup')
        try:
            with recovery_mutex():
                owner = retained_owner()
                if args.resolve_live_cleanup:
                    if args.no_live_publication and not args.guest_only_error:
                        raise NotRun('nothing to record: give the --guest-only-error values you checked, or run --recover-retained')
                    resolution_overlay(owner, args.resolve_live_cleanup, args.no_live_publication)
                for name in args.unaudited_attempt:
                    print(json.dumps(record_unaudited_attempt(owner, name), indent=2))
                if args.resolve_live_cleanup:
                    print(json.dumps(resolve_live_cleanup(owner, args.resolve_live_cleanup, publication=given if all(given.values()) else None,
                                                          guest_only_errors=args.guest_only_error,
                                                          no_live_publication=args.no_live_publication), indent=2))
        except NotRun as error:
            raise SystemExit(str(error)) from None
        return
    if args.recover_retained is not None:
        try:
            config = json.loads(private_file(args.config).read_text())
            hosts = [host for host, builder in config['builders'].items()
                     if socket.gethostname() in builder.get('hostnames', ())]
            if len(hosts) != 1:
                raise NotRun('this machine is not exactly one configured nightly builder')
            print(json.dumps(recover_retained(dict(config['builders'][hosts[0]], host=hosts[0]), args.recover_retained), indent=2))
        except NotRun as error:
            raise SystemExit(str(error)) from None
        return
    if args.desktops and (not (args.now or args.stable_draft) or args.worker):
        parser.error('--desktops requires --now or --stable-draft on the controller')
    if args.stable_draft and (not args.tag or not STABLE_TAG.fullmatch(args.tag) or args.worker):
        parser.error('--stable-draft requires a named stable tag on the controller')
    if bool(args.stable_draft) != bool(args.draft_token_file):
        parser.error('--stable-draft and --draft-token-file go together')
    config = json.loads(private_file(args.config).read_text())
    expected = args.worker or 'd69'
    if socket.gethostname() not in config['builders'][expected]['hostnames']:
        parser.error('controller/worker must run on its configured builder')
    if args.worker:
        os.chdir(ROOT)
        tools_bin = config['builders'][args.worker].get('toolsBin')
        if tools_bin:
            os.environ['PATH'] = tools_bin+os.pathsep+os.environ.get('PATH', '/usr/bin:/bin')
        builder = dict(config['builders'][args.worker], host=args.worker)
        if args.row:
            # Exactly the rows the controller downloaded packages for, in the configured order.
            if not set(args.row) <= {r['name'] for r in builder['rows']}:
                parser.error('--row names a row this builder does not have')
            builder['rows'] = [r for r in builder['rows'] if r['name'] in args.row]
        worker(builder, args.tag,
               json.loads((args.artifacts/'identity.json').read_text()), args.artifacts, args.output)
    else:
        local_workers = LocalWorkers()
        with controller_shutdown(local_workers):
            controller(args, config, local_workers=local_workers)


if __name__ == '__main__':
    main()
