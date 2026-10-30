"""Guarded local VM overlay retirement. Never boots a guest or contacts a service.

Custom completed runs may supply an explicit manifest with schemaVersion=1,
kind="vm-overlay-cleanup-evidence", absolute run, runId (Linux state id or Windows run_id),
completed=true, outcome="passed" or "failed", and files={relative: sha256}.
The files must include completion.json repeating run/runId/completed/outcome.
All files are owned regular files below the manifest's nonsymlink parent.
This records completion, not acceptance; failure evidence remains failure evidence.
Default cleanup instead requires the existing passed lab scenario evidence/journal.
Only disk.qcow2 is deleted; all other run files and credentials stay in place.
"""
from contextlib import contextmanager
import datetime
import fcntl
import hashlib
import inspect
import json
import os
from pathlib import Path
import re
import select
import stat
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
_PROC = Path('/proc')
_thread_lock = threading.RLock()
_lock_depth = threading.local()


class CleanupError(RuntimeError):
    pass


def _no_symlinks(path):
    if '..' in Path(path).parts:
        raise CleanupError('Parent traversal is not eligible for cleanup')
    path = Path(os.path.abspath(path))
    for component in (path, *path.parents):
        if component.is_symlink():
            raise CleanupError('Symlink path component is not eligible for cleanup')
    return path


def _regular(path):
    path = _no_symlinks(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
        raise CleanupError('Expected an owned, single-link regular file')
    return info


def _identity(path):
    info = _regular(path)
    return {'device': info.st_dev, 'inode': info.st_ino, 'size': info.st_size,
            'mtimeNs': info.st_mtime_ns, 'ctimeNs': info.st_ctime_ns,
            'allocatedBytes': info.st_blocks * 512}


def _read_json(path):
    _regular(path)
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise CleanupError('Expected a JSON object')
    return value


def _sha256(path):
    _regular(path)
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def _save(path, value):
    import tempfile
    fd, name = tempfile.mkstemp(prefix='.overlay-cleanup-', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
        directory = os.open(path.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


@contextmanager
def lifecycle_lock():
    """Serialize cooperating Linux/Windows launch, clone, reset and cleanup calls.

    Reentrant within a thread; nonblocking across processes. Keep this held for
    the entire disk lifecycle mutation, including QEMU start/stop confirmation.
    """
    with _thread_lock:
        depth = getattr(_lock_depth, 'value', 0)
        if depth:
            _lock_depth.value += 1
            try:
                yield
            finally:
                _lock_depth.value -= 1
            return
        path = _no_symlinks(ROOT / 'work/.vm-lifecycle.lock')
        path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
        try:
            _regular(path)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise CleanupError('Another VM lifecycle operation is in progress') from error
            _lock_depth.value = 1
            try:
                yield
            finally:
                _lock_depth.value = 0
        finally:
            os.close(fd)


def _images_root(value):
    """The golden root: this checkout's work/lab-images, or an explicit one.

    A lab.py copy outside the main tree (a scratch checkout on a builder) keeps
    its runs next to itself while its goldens stay in the main tree. The
    explicit root must be an owned, symlink-free `.../work/lab-images`.
    """
    if value is None:
        return ROOT / 'work/lab-images'
    if not Path(value).is_absolute():
        raise CleanupError('The images root must be an absolute path')
    root = _no_symlinks(value)
    if root.name != 'lab-images' or root.parent.name != 'work':
        raise CleanupError('The images root must be a work/lab-images directory')
    if not root.is_dir() or root.stat().st_uid != os.getuid():
        raise CleanupError('The images root must be an owned directory')
    return root


def _run_state(run, images_root=None):
    run = _no_symlinks(run)
    allowed = [ROOT / 'work/lab-runs', ROOT / 'work/windows/runs']
    if run.parent not in allowed or not run.is_dir() or run.stat().st_uid != os.getuid():
        raise CleanupError('Cleanup requires a direct owned run under a canonical lab runs directory; '
                           'for a pre-existing outside run, stop and relocate it using '
                           'docs/vm-lab.md#retire-completed-overlays')
    if (run / 'manifest.json').exists() or (run / 'manifest.json').is_symlink():
        raise CleanupError('Sealed bundles are not disposable overlays')
    info = _read_json(run / 'state.json')
    disk = run / 'disk.qcow2'
    if run.parent == allowed[0]:
        if info.get('schemaVersion') != 1 or info.get('overlay') != str(disk):
            raise CleanupError('Linux state overlay identity does not match the run')
        base_name, identity = info.get('golden'), info.get('id')
        base_root = _images_root(images_root)
    else:
        base_name, identity = info.get('base'), info.get('run_id')
        base_root = ROOT / 'work/windows/bases'
    if not isinstance(identity, str) or not identity or not isinstance(base_name, str):
        raise CleanupError('Missing run or base identity')
    base = _no_symlinks(base_name)
    if not Path(base_name).is_absolute() or not base.is_relative_to(base_root):
        raise CleanupError('Recorded base is outside its canonical golden/base root')
    if run.parent == allowed[1]:
        if not base.is_dir() or base.stat().st_uid != os.getuid():
            raise CleanupError('Windows base must be an owned bundle directory')
        _read_json(base / 'manifest.json')
        base = base / 'disk.qcow2'
    _regular(base)
    return run, disk, base, identity


def _members(manifest_path, manifest):
    entries = manifest.get('files')
    if not isinstance(entries, dict) or not entries:
        raise CleanupError('Evidence manifest has no hashed members')
    for name, digest in entries.items():
        relative = Path(name)
        if (relative.is_absolute() or '..' in relative.parts or not relative.parts
                or not isinstance(digest, str) or not re.fullmatch('[0-9a-f]{64}', digest)):
            raise CleanupError('Invalid evidence member path or checksum')
        member = manifest_path.parent / relative
        if _sha256(member) != digest:
            raise CleanupError('Evidence member checksum mismatch')


def _evidence(run, run_id, explicit):
    if explicit is not None:
        path = _no_symlinks(explicit)
        manifest = _read_json(path)
        required = {'schemaVersion': 1, 'kind': 'vm-overlay-cleanup-evidence',
                    'run': str(run), 'runId': run_id, 'completed': True}
        if (any(manifest.get(key) != value for key, value in required.items())
                or manifest.get('completed') is not True
                or manifest.get('outcome') not in ('passed', 'failed')):
            raise CleanupError('Custom evidence does not record this completed run')
        _members(path, manifest)
        if 'completion.json' not in manifest['files']:
            raise CleanupError('Custom evidence requires hashed completion.json')
        proof = _read_json(path.parent / 'completion.json')
        if proof.get('completed') is not True:
            raise CleanupError('Custom completion proof is unfinished')
        for key in ('run', 'runId', 'completed', 'outcome'):
            if proof.get(key) != manifest[key]:
                raise CleanupError('Custom completion proof does not match the manifest')
    else:
        results = _read_json(run / 'results.json')
        test_id = results.get('testId')
        if not isinstance(test_id, str) or not re.fullmatch('[A-Za-z0-9_.-]+', test_id) or test_id in ('.', '..'):
            raise CleanupError('Missing or invalid completed test identity')
        directory = 'tests/' + test_id
        path = run / directory / 'manifest.json'
        manifest = _read_json(path)
        if (manifest.get('schemaVersion') != 1 or manifest.get('testId') != test_id
                or results.get('ok') is not True or results.get('collectionIncomplete')
                or results.get('evidenceDirectory') != directory):
            raise CleanupError('Only passed, fully collected scenario evidence permits automatic cleanup')
        _members(path, manifest)
        if manifest['files'].get('results.json') != _sha256(run / 'results.json'):
            raise CleanupError('Current results differ from exported evidence')
        exported = _read_json(path.parent / 'results.json')
        if exported != results:
            raise CleanupError('Exported results do not match this run')
        journal = run / 'test-events.jsonl'
        _regular(journal)
        events = [json.loads(line) for line in journal.read_text().splitlines() if line.strip()]
        if not events or any(not isinstance(event, dict) for event in events):
            raise CleanupError('Missing valid test event journal')
        finished = [event for event in events if event.get('event') == 'finished']
        if any(event.get('ok') is not True or event.get('collectionIncomplete') is not False for event in finished):
            raise CleanupError('Prior failed or incomplete scenario requires explicit reviewed completion evidence')
        completed_ids = {event.get('testId') for event in finished}
        if any(event.get('event') == 'started' and event.get('testId') not in completed_ids for event in events):
            raise CleanupError('Unfinished scenario requires explicit reviewed completion evidence')
        finish = events[-1]
        if (finish.get('event') != 'finished' or finish.get('testId') != test_id
                or finish.get('ok') is not True or finish.get('evidenceDirectory') != directory
                or finish.get('profile') != manifest.get('profile')
                or finish.get('scenario') != manifest.get('scenario')
                or results.get('profile') != manifest.get('profile')
                or not any(event.get('event') == 'started' and event.get('testId') == test_id for event in events[:-1])):
            raise CleanupError('Test journal does not finish the exported scenario')
    evidence = {'manifest': str(path), 'sha256': _sha256(path), 'runId': run_id,
                'mode': 'explicit' if explicit is not None else 'passed-scenario'}
    if explicit is None:
        evidence['journalSha256'] = _sha256(journal)
    return evidence


def _chain(disk):
    result = subprocess.run(['qemu-img', 'info', '--force-share', '--output=json', '--backing-chain', str(disk)],
                            capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise CleanupError('Cannot inspect a lab image backing chain; cleanup refused')
    chain = json.loads(result.stdout)
    if not isinstance(chain, list) or not chain or any(not isinstance(image, dict) for image in chain):
        raise CleanupError('Invalid backing chain inventory')
    if any(image.get('format') != 'qcow2' for image in chain):
        raise CleanupError('Only qcow2 lab backing chains are supported')
    paths = []
    current = disk.resolve()
    for image in chain:
        paths.append(current)
        backing = image.get('full-backing-filename', image.get('backing-filename'))
        if backing:
            candidate = Path(backing)
            current = (candidate if candidate.is_absolute() else current.parent / candidate).resolve()
    return paths


def _check_chains(disk, base, images_root=None):
    selected = _chain(disk)
    if len(selected) < 2 or selected[1] != base:
        raise CleanupError('Actual overlay backing file does not match recorded base')
    roots = [ROOT / relative for relative in ('work/lab-runs', 'work/lab-images', 'work/windows')]
    if images_root is not None:
        # Also inventory the tree that owns the explicit golden root.
        explicit = _images_root(images_root)
        roots += [explicit, explicit.parent / 'lab-runs', explicit.parent / 'windows']
    visited = set()
    def incomplete(_error):
        raise CleanupError('Cannot completely inventory lab image dependencies')
    for candidate in dict.fromkeys(roots):
        root = _no_symlinks(candidate)
        if not root.exists():
            continue
        for directory, subdirs, files in os.walk(root, followlinks=False, onerror=incomplete):
            if any((Path(directory) / name).is_symlink() for name in subdirs):
                raise CleanupError('Symlink directory prevents complete lab image inventory')
            for name in files:
                image = Path(directory) / name
                if not image.is_file():
                    continue
                # Inspect only the image signature; arbitrary suffixes are legal.
                with image.open('rb') as stream:
                    if stream.read(4) != b'QFI\xfb':
                        continue
                resolved = image.resolve()
                if resolved in visited:
                    continue
                visited.add(resolved)
                if resolved == disk:
                    continue
                if disk in _chain(image)[1:]:
                    raise CleanupError('Another lab image depends on the selected overlay')


def _lock_alive(process, disk):
    if process.poll() is not None:
        raise CleanupError('Native image lock process exited')
    identity = disk.stat()
    for descriptor in (Path('/proc') / str(process.pid) / 'fd').iterdir():
        try:
            info = descriptor.stat()
        except FileNotFoundError:
            continue
        if (info.st_dev, info.st_ino) == (identity.st_dev, identity.st_ino):
            return
    raise CleanupError('Native image lock no longer owns this disk')


@contextmanager
def _image_lock(disk):
    process = subprocess.Popen(['qemu-io', '-f', 'qcow2', str(disk)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    try:
        deadline = time.monotonic() + 10
        output = b''
        while not output.endswith(b'qemu-io> '):
            remaining = deadline - time.monotonic()
            if remaining <= 0 or len(output) > 65536 or process.poll() is not None:
                raise CleanupError('Could not acquire the native image lock')
            if select.select([process.stdout], [], [], min(remaining, .2))[0]:
                chunk = os.read(process.stdout.fileno(), 4096)
                if not chunk:
                    raise CleanupError('Native image lock closed before readiness')
                output += chunk
        _lock_alive(process, disk)
        yield process
    finally:
        if process.poll() is None:
            try:
                process.stdin.write(b'quit\n')
                process.stdin.flush()
                process.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired):
                process.kill()
                process.wait(timeout=2)
        process.stdin.close()
        process.stdout.close()


def _scan_processes(proc, run, device, inode, lock_pid):
    """Read-only probe, also sent as source to an isolated privileged Python.

    No UID/comm exemption: root, foreign users and nondumpable applications
    can hold ordinary read-only FDs despite the native QEMU writer lock.
    PermissionError must reach the caller; an opaque process is not evidence
    that the disk is unused. Never read or emit process arguments/maps wholesale.
    """
    status = (proc / 'self/status').read_text()
    namespace = re.search(r'^NSpid:\s*(.*)$', status, re.MULTILINE)
    if namespace and len(namespace[1].split()) > 1:
        raise CleanupError('Hidden host PID namespace prevents safe cleanup')
    for line in (proc / 'mounts').read_text().splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1] == '/proc' and any(option.startswith('hidepid=') and option != 'hidepid=0' for option in fields[3].split(',')):
            raise CleanupError('Hidden /proc inventory prevents safe cleanup')
    for directory in proc.iterdir():
        if not directory.name.isdecimal():
            continue
        if int(directory.name) == lock_pid:
            continue
        try:
            def start_ticks():
                fields = (directory / 'stat').read_text().rsplit(')', 1)[-1].split()
                if len(fields) < 20 or not fields[19].isdecimal():
                    raise CleanupError('Cannot interpret host process identity')
                return fields[19]
            start = start_ticks()
            comm = (directory / 'comm').read_text().strip()
            arguments = (directory / 'cmdline').read_bytes().split(b'\0')
            executable = Path(os.fsdecode(arguments[0])).name
            vm = executable.startswith('qemu-system') or executable == 'swtpm'
            possible_vm = vm or comm.startswith('qemu') or comm == 'swtpm'
            paths = (os.fsencode(run), os.fsencode(Path(run) / 'disk.qcow2'))
            if possible_vm and any(path in arg for path in paths for arg in arguments):
                raise CleanupError('A QEMU/TPM process still owns the run')
            for descriptor in (directory / 'fd').iterdir():
                try:
                    info = descriptor.stat()
                    if (info.st_dev, info.st_ino) == (device, inode):
                        raise CleanupError('A process still has the overlay open')
                except FileNotFoundError:
                    continue
            for mapping in (directory / 'maps').read_text().splitlines():
                fields = mapping.split(maxsplit=5)
                if len(fields) < 5 or not fields[4].isdecimal():
                    raise CleanupError('Cannot interpret host memory mapping inventory')
                major, minor = (int(part, 16) for part in fields[3].split(':'))
                if (int(fields[4]) == inode
                        and os.makedev(major, minor) == device):
                    raise CleanupError('A process still maps the overlay')
            if start_ticks() != start:
                raise CleanupError('Host process identity changed during inventory')
        except (FileNotFoundError, ProcessLookupError):
            try:
                directory.stat()
            except (FileNotFoundError, ProcessLookupError):
                continue
            raise CleanupError('Host process inventory disappeared for a live PID')


def _privileged_process_inventory(run, device, inode, lock_pid):
    # Boat grants user passwordless sudo. Elevate only this read-only probe,
    # never lab.py or a checkout import (which could run unrelated root code).
    # -I ignores PYTHONPATH, user site packages and the working directory.
    # No sudoers changes, prompts, installation, disk opens or mutations.
    source = ('import os, re, sys\nfrom pathlib import Path\n'
              'class CleanupError(RuntimeError): pass\n' + inspect.getsource(_scan_processes) +
              '\ntry:\n'
              '    _scan_processes(Path("/proc"), sys.argv[1], int(sys.argv[2]), '
              'int(sys.argv[3]), int(sys.argv[4]))\n'
              'except CleanupError:\n    print("blocked")\n'
              'except Exception:\n    print("unavailable")\n'
              'else:\n    print("clear")\n')
    try:
        result = subprocess.run(['sudo', '-n', '--', '/usr/bin/python3', '-I', '-c', source,
                                 str(run), str(device), str(inode), str(lock_pid)],
                                capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as error:
        raise CleanupError('Host process inventory is not fully visible; '
                           'read-only sudo inventory unavailable; cleanup refused') from error
    if result.returncode != 0 or result.stdout != 'clear\n' or result.stderr:
        raise CleanupError('Privileged host process inventory blocked or incomplete; cleanup refused')


def _check_processes(run, disk, *, lock_process=None):
    identity = disk.stat()
    lock_pid = -1
    if lock_process is not None:
        _lock_alive(lock_process, disk)
        lock_pid = lock_process.pid
    try:
        _scan_processes(_PROC, str(run), identity.st_dev, identity.st_ino, lock_pid)
    except PermissionError:
        # Full host visibility is still required. A filtered /proc or nested
        # PID namespace is rejected by both probes, never cured by sudo.
        _privileged_process_inventory(run, identity.st_dev, identity.st_ino, lock_pid)
    if lock_process is not None:
        _lock_alive(lock_process, disk)


def cleanup(run, *, evidence_manifest=None, dry_run=False, images_root=None):
    """Retire exactly one stopped, unreferenced overlay, retaining durable evidence."""
    try:
        with lifecycle_lock():
            run, disk, base, run_id = _run_state(Path(run), images_root)
            _no_symlinks(disk)
            evidence = _evidence(run, run_id, evidence_manifest)
            receipt_path = _no_symlinks(run / 'overlay-cleanup.json')
            old = _read_json(receipt_path) if receipt_path.exists() else None
            state_hash = _sha256(run / 'state.json')
            if old and (old.get('schemaVersion') != 1 or old.get('status') not in ('prepared', 'completed')
                        or old.get('disk') != str(disk) or old.get('base') != str(base)):
                raise CleanupError('Invalid existing cleanup receipt')
            if old and (old.get('run') != str(run) or old.get('evidence') != evidence):
                raise CleanupError('Existing cleanup receipt belongs to different evidence')
            if old and old.get('stateSha256') != state_hash:
                raise CleanupError('Run state changed since the cleanup receipt')
            if not disk.exists():
                if old and old.get('status') in ('prepared', 'completed') and old.get('disk') == str(disk):
                    if old['status'] == 'prepared' and not dry_run:
                        old.update(status='completed', completedUtc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                        _save(receipt_path, old)
                    return old
                raise CleanupError('Overlay is absent without a matching cleanup receipt')
            if old and old.get('status') == 'completed':
                raise CleanupError('A disk appeared after completed cleanup; refusing replacement')
            identity = _identity(disk)
            with _image_lock(disk) as lock_process:
                _check_processes(run, disk, lock_process=lock_process)
                _check_chains(disk, base, images_root)
                receipt = {'schemaVersion': 1, 'status': 'dry-run' if dry_run else 'prepared',
                           'run': str(run), 'disk': str(disk), 'diskIdentity': identity,
                           'base': str(base), 'stateSha256': state_hash, 'evidence': evidence,
                           'preparedUtc': datetime.datetime.now(datetime.timezone.utc).isoformat()}
                if dry_run:
                    return receipt
                if old and old.get('diskIdentity') != identity:
                    raise CleanupError('Overlay identity changed since the prepared receipt')
                _save(receipt_path, receipt)
                _check_processes(run, disk, lock_process=lock_process)
                if (_identity(disk) != identity or _sha256(run / 'state.json') != state_hash
                        or _evidence(run, run_id, evidence_manifest) != evidence):
                    raise CleanupError('Run or evidence changed before unlink')
                _lock_alive(lock_process, disk)
                disk.unlink()
                receipt.update(status='completed', completedUtc=datetime.datetime.now(datetime.timezone.utc).isoformat())
                _save(receipt_path, receipt)
                return receipt
    except CleanupError:
        raise
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        # Do not expose subprocess stderr, command lines, evidence or credentials.
        raise CleanupError('Cleanup prerequisite failed (' + type(error).__name__ + ')') from error
