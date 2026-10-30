#!/usr/bin/env python3
"""Build an exact public commit in parallel, disposable Boat workers.

Requires the authenticated boat CLI, OpenSSH and distro python3-yaml locally.
All compilation and rendering happen remotely. See docs/boat-candidates.md.
"""
import argparse
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tarfile
import threading
import time
import uuid

ROOT = Path(__file__).resolve().parents[3]
REPOSITORY = 'https://github.com/patrick-hudson/put.gd-desktop.git'
TARGETS = ('appimage', 'ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44',
           'ui-catalog', 'lab-fixture')
CANCEL = threading.Event()
CREATE_TIMEOUT = 120
LATE_CREATE_GRACE = 30
LEASE_ROOT = Path.home() / '.local/state/putgd/boat-candidate-runs'


@contextmanager
def run_lease(nonce):
    """A shared, cross-worktree flock; retained files identify dead local runs.

    Unknown leases (e.g. a controller on another workstation) are protected until
    the sandbox TTL. Never unlink lock files: that would split the lock namespace.
    """
    LEASE_ROOT.mkdir(parents=True, mode=0o700, exist_ok=True)
    with (LEASE_ROOT / nonce).open('x') as lease:
        os.chmod(lease.name, 0o600)
        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


def orphaned(item, nonce):
    if nonce:
        try:
            with (LEASE_ROOT / nonce).open('r') as lease:
                fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return True
        except BlockingIOError:
            return False  # A known live controller wins even over an expired TTL.
        except FileNotFoundError:
            pass  # Unknown controllers may only be reaped after TTL expiry.
    try:
        created = datetime.fromisoformat(item['createdAt'].replace('Z', '+00:00')).timestamp()
        ttl = int(item['ttlSeconds'])
        if ttl > 0 and time.time() >= created + ttl:
            return True
    except (KeyError, TypeError, ValueError):
        pass  # Missing/unknown remote metadata is not permission to delete.
    return False


def exact_sha(value):
    if not re.fullmatch(r'[0-9a-fA-F]{40}', value):
        raise argparse.ArgumentTypeError('commit must be a full 40-character Git SHA')
    return value.lower()


def ttl_seconds(value):
    try:
        number = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('TTL must be an integer') from error
    if not 600 <= number <= 14400:
        raise argparse.ArgumentTypeError('TTL must be between 600 and 14400 seconds')
    return number


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('commit', type=exact_sha, nargs='?')
    result.add_argument('--cleanup', type=exact_sha, metavar='SHA',
                        help='delete only eligible orphan candidate workers for this full commit; preserve live peers; no build')
    result.add_argument('--output', required=True, type=Path,
                        help='new private directory outside every Git checkout')
    result.add_argument('--artifact', choices=TARGETS, action='append',
                        help='repeat to select a subset; default: all seven')
    result.add_argument('--benchmark-catalog-scp', action='store_true',
                        help='also collect the same catalog by legacy recursive SCP for timing evidence')
    result.add_argument('--ttl', type=ttl_seconds, default=10800)
    result.add_argument('--type', choices=('default', 'large'), default='large')
    return result


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def run(argv, *, timeout=120, input=None, log=None, cleanup=False):
    """Bound the controller too; cancellation kills SSH while deletion kills its worker."""
    process = subprocess.Popen(argv, stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL,
                               stdout=log or subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True)
    deadline = time.monotonic() + timeout
    first = True
    while True:
        try:
            output, _ = process.communicate(input=input if first else None, timeout=min(1, max(0, deadline - time.monotonic())))
            break
        except subprocess.TimeoutExpired:
            first = False
            if (CANCEL.is_set() and not cleanup) or time.monotonic() >= deadline:
                process.kill()
                process.communicate()
                raise TimeoutError('Controller cancelled or deadline exceeded: ' + argv[0])
    if process.returncode:
        raise RuntimeError(f'{argv[0]} exited {process.returncode}: {(output or "see build.log")[-2000:]}')
    return output or ''


def boat(*args, timeout=120, cleanup=False):
    output = run(['boat', '--no-update', '--json', *args], timeout=timeout, cleanup=cleanup)
    # `new` is JSONL (created/state/ready); all other commands return one object.
    return [json.loads(line) for line in output.splitlines() if line.strip()][-1]


def prepare_output(path):
    path = path.expanduser().resolve()
    ancestor = path
    while not ancestor.exists():
        ancestor = ancestor.parent
    probe = subprocess.run(['git', '-C', str(ancestor), 'rev-parse', '--git-dir'],
                           capture_output=True, text=True)
    if probe.returncode == 0:
        raise ValueError('output must be outside Git, including ignored work/ directories')
    path.mkdir(parents=True, mode=0o700, exist_ok=False)
    path.chmod(0o700)
    return path


def check_capacity(limits, workers):
    if not limits.get('canStart'):
        raise ValueError('Boat account cannot start workers')
    available = limits['maxActiveSandboxes'] - limits['activeSandboxes']
    starts = limits['starts']
    if available < workers or any(starts[period]['remaining'] < workers
                                  for period in ('minute', 'hour', 'day')):
        raise ValueError('Insufficient Boat capacity/start allowance for the entire parallel run')


def load_workflows(commit):
    import yaml  # standard distro python3-yaml; no workstation provisioning
    if run(['git', '-C', str(ROOT), 'cat-file', '-t', commit]).strip() != 'commit':
        raise ValueError('SHA must identify a commit, not a tree or tag object')
    def read(name):
        return yaml.safe_load(run(['git', '-C', str(ROOT), 'show', f'{commit}:.github/workflows/{name}.yml']))
    return read('package'), read('ci')


def step(job, name, replacements=None):
    found = [item for item in job['steps'] if item.get('name') == name]
    if len(found) != 1 or 'run' not in found[0]:
        raise ValueError('Exact commit has no supported CI recipe step: ' + name)
    script = found[0]['run']
    for key, value in (replacements or {}).items():
        script = script.replace('${{ ' + key + ' }}', value)
    if '${{' in script:
        raise ValueError('Unresolved CI expression in ' + name)
    return script.rstrip() + '\n'


def recipe(target, packages, ci):
    """Use the candidate's CI matrix images and run blocks, never today's recipes."""
    native = packages['jobs']['native']
    matrix = {item['name']: item for item in native['strategy']['matrix']['include']}
    base = matrix['ubuntu24.04']['image']
    common = 'export PATH=/usr/bin:/bin:$PATH\n'
    common += 'runner_state=$(mktemp -d /tmp/putgd-runner.XXXXXX)\n'
    common += 'export GITHUB_WORKSPACE=$PWD GITHUB_STEP_SUMMARY=$runner_state/summary.md GITHUB_ENV=$runner_state/env GITHUB_OUTPUT=$runner_state/output RUNNER_TEMP=$runner_state\n'
    common += 'export CCACHE_DIR=$PWD/build/ccache CCACHE_BASEDIR=$PWD CCACHE_MAXSIZE=256M\n'
    common += 'export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache\n'
    common += 'export PUTGD_SDK_JOBS=$(nproc) npm_config_fetch_retries=5 RELEASE_TAG=\n'
    common += '(\n' + step(native, 'Bootstrap checkout tools', {'matrix.name': target}) + ')\n'
    common += 'git config --global --add safe.directory "$PWD"\n'
    if target in matrix:
        row = matrix[target]
        if not re.fullmatch(r'[a-z0-9:./-]+@sha256:[0-9a-f]{64}', row['image']):
            raise ValueError('Native CI image must be pinned by digest')
        script = common + step(native, 'Standard distro dependencies')
        script += step(native, 'Verify pinned SDK source')
        # Cache restore/save are Actions operations. Provision from the exact same scripts cold.
        script += 'bash tools/releases/provision-sdk.sh\n'
        script += step(native, 'Provision pinned Proton helper')
        script += f'export FORMAT={shlex.quote(row["format"])} PACKAGE_TARGET={shlex.quote(target)} KDE_REQUIREMENT={shlex.quote(row["kde"])}\n'
        script += step(native, 'Build production release and audit native package')
        if target == 'ubuntu24.04':
            script += step(native, 'Export exact application source')
        script += 'python3 tools/releases/stage_artifacts.py --kind native --packages-only\ncp -a build/release-artifact/. /out/\n'
        return row['image'], script, 'release'
    if target == 'appimage':
        app = packages['jobs']['appimage']
        # The minimal container lacks the hosted runner's preinstalled Python.
        script = common + 'bash tools/ci/install-packages.sh python3\n'
        script += step(app, 'Use official Ubuntu archive mirrors')
        script += step(app, 'Standard build dependencies')
        # install-qt-action uses aqtinstall 3.3.*, py7zr 1.1.0; install its locked Qt/modules.
        script += 'export LOG_CFG=$PWD/tools/ci/aqt-logging.ini\n'
        script += 'bash tools/ci/install-packages.sh python3-venv\npython3 -m venv /opt/aqt\n/opt/aqt/bin/pip install "aqtinstall==3.3.*" "py7zr==1.1.0"\n'
        script += 'qt_version=$(python3 -c \'import json; print(json.load(open("packaging/appimage/tools.lock.json"))["buildInputs"]["qt"])\')\n'
        script += '/opt/aqt/bin/aqt install-qt linux desktop "$qt_version" linux_gcc_64 -m qtimageformats -O /opt/Qt\n'
        script += 'export QT_ROOT_DIR=/opt/Qt/$qt_version/gcc_64\nexport PATH=$QT_ROOT_DIR/bin:$PATH\nexport LD_LIBRARY_PATH=$QT_ROOT_DIR/lib\nexport QT_PLUGIN_PATH=$QT_ROOT_DIR/plugins QML2_IMPORT_PATH=$QT_ROOT_DIR/qml\n'
        script += 'export PKG_CONFIG_PATH=${PKG_CONFIG_PATH:+$PKG_CONFIG_PATH:}$QT_ROOT_DIR/lib/pkgconfig\n'
        script += step(app, 'Verify pinned SDK source')
        script += 'bash tools/releases/provision-sdk.sh\npython3 tools/releases/provision-appimage.py --qt-root "$QT_ROOT_DIR"\n'
        script += step(app, 'Provision pinned Proton helper')
        script += step(app, "Headless Wayland compositor for the image's start check")
        script += step(app, 'Build and audit AppImage')
        script += 'python3 tools/releases/stage_artifacts.py --kind appimage --packages-only\ncp -a build/release-artifact/. /out/\n'
        return base, script, 'release'
    if target == 'ui-catalog':
        linux = ci['jobs']['linux']
        script = common + step(linux, 'Install distro dependencies', {'matrix.distribution': '26.04'})
        script += 'bash tools/ci/provision-linux-sdk.sh\n'
        script += step(linux, 'Provision pinned Proton helper')
        script += step(linux, 'Build')
        script += 'catalog_runtime=$(mktemp -d /tmp/pgc.XXXXXX)\n'
        script += 'python3 tools/diagnostics/check_socket_paths.py --socket "$catalog_runtime/putgd/session.sock" > /out/socket-preflight.json\n'
        script += step(linux, 'Render the UI catalog for the surface map').replace('$(mktemp -d)', '$catalog_runtime')
        script += 'cp -a build/ui-catalog /out/\n'
        return matrix['ubuntu26.04']['image'], script, 'dev'
    # Disposable VM drivers need putgd-desktop-fixture in a separately labeled DEB.
    script = common + step(native, 'Standard distro dependencies')
    script += step(native, 'Verify pinned SDK source') + 'bash tools/releases/provision-sdk.sh\n'
    script += step(native, 'Provision pinned Proton helper')
    script += 'cmake --preset release -DBUILD_TESTING=OFF -DPUTGD_LAB_FIXTURE=ON -DPUTGD_PHYSICAL_BENCHMARKS=OFF -DPUTGD_BUNDLE_PROTON=ON -DCMAKE_INSTALL_PREFIX=/usr -DPUTGD_PACKAGE_TARGET= -DPUTGD_UPDATE_CHECK=OFF\n'
    script += 'cmake --build --preset release --parallel "$(nproc)"\ncpack --config build/release/CPackConfig.cmake -G DEB -D CPACK_PACKAGE_NAME=putgd-lab-fixture -D CPACK_DEBIAN_PACKAGE_NAME=putgd-lab-fixture -D CPACK_PACKAGE_FILE_NAME=putgd-lab-fixture -D CPACK_DEBIAN_PACKAGE_CONFLICTS=putgd -D CPACK_DEBIAN_PACKAGE_REPLACES=putgd -D "CPACK_PACKAGE_DESCRIPTION_SUMMARY=Disposable lab fixture build of put.gd; test use only" -D "CPACK_DEBIAN_PACKAGE_DESCRIPTION=Disposable lab fixture build of put.gd; test use only" -B build/lab-packages\n'
    script += 'test "$(dpkg-deb -f build/lab-packages/*.deb Package)" = putgd-lab-fixture\n'
    script += 'dpkg-deb -f build/lab-packages/*.deb > /out/lab-package-control.txt\n'
    script += 'build/release/putgd-cli --update-info > /out/update-info.json\n'
    script += 'cp build/lab-packages/*.deb /out/\ncp build/release/packaging/linux/putgd-desktop-fixture /out/\n'
    return base, script, 'release'


def check_collection_deadline(deadline):
    if deadline is not None and time.monotonic() >= deadline:
        raise TimeoutError("Collection deadline reached (bounded by sandbox TTL)")


def sha256(path, *, deadline=None):
    check_collection_deadline(deadline)
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            check_collection_deadline(deadline)
            digest.update(chunk)
    check_collection_deadline(deadline)
    return digest.hexdigest()


def inventory(directory):
    files = []
    for path in sorted(directory.rglob('*')):
        if path.is_symlink():
            raise ValueError('Collected output contains a symlink: ' + str(path))
        if path.is_file():
            files.append({'path': path.relative_to(directory).as_posix(),
                          'bytes': path.stat().st_size, 'sha256': sha256(path)})
    return files


def verify_identity(directory, commit, target):
    identity = json.loads((directory / 'build-info.json').read_text())
    binary = json.loads((directory / 'binary-version.json').read_text())
    if identity.get('gitCommit') != commit or identity.get('dirty') is not False:
        raise ValueError('Built source is dirty or does not match the requested SHA')
    if binary.get('gitCommit') != commit or binary.get('version') != identity['version']:
        raise ValueError('Compiled CLI identity does not match build-info.json')
    expected = '*.AppImage' if target == 'appimage' else '*.rpm' if target == 'fedora44' else '*.deb'
    if target == 'ui-catalog':
        if not any((directory / 'ui-catalog').rglob('*.png')):
            raise ValueError('UI catalog contains no rendered PNGs')
    elif len(list(directory.glob(expected))) != 1:
        raise ValueError('Expected exactly one ' + expected + ' artifact')
    return identity


def ssh_options(pin):
    # Ignore user/system configuration and forbid forwarding, multiplexing and proxies.
    return ['-F', '/dev/null', '-i', str(Path.home() / '.ssh/ascii_box_ed25519'),
            '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', f'UserKnownHostsFile={pin}',
            '-o', 'GlobalKnownHostsFile=/dev/null', '-o', 'ForwardAgent=no',
            '-o', 'ClearAllForwardings=yes', '-o', 'ForwardX11=no',
            '-o', 'ControlMaster=no', '-o', 'ControlPath=none',
            '-o', 'ProxyCommand=none', '-o', 'ProxyJump=none',
            '-o', 'ConnectTimeout=30', '-o', 'ServerAliveInterval=15',
            '-o', 'ServerAliveCountMax=3']


def restore_archive(archive, destination, expected, *, deadline=None):
    if sha256(archive, deadline=deadline) != expected:
        raise ValueError('Transport archive SHA-256 mismatch')
    # Validate the entire archive before writing anything. Never restore ownership,
    # permissions, links, devices or timestamps supplied by the worker.
    with tarfile.open(archive) as stream:
        members = []
        for member in stream:
            check_collection_deadline(deadline)
            members.append(member)
        names = set()
        for member in members:
            check_collection_deadline(deadline)
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or not (member.isdir() or member.isfile()):
                raise ValueError('Unsafe transport archive member: ' + member.name)
            name = path.as_posix()
            if name in names:
                raise ValueError('Duplicate transport archive member: ' + name)
            names.add(name)
        destination.mkdir(mode=0o700)
        for member in members:
            check_collection_deadline(deadline)
            path = destination / member.name
            if member.isdir():
                path.mkdir(parents=True, mode=0o700, exist_ok=True)
            else:
                path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
                with stream.extractfile(member) as source, path.open('xb') as target:
                    while chunk := source.read(1024 * 1024):
                        check_collection_deadline(deadline)
                        target.write(chunk)
                path.chmod(0o700 if member.mode & 0o100 else 0o600)
    check_collection_deadline(deadline)


def collection_budget(size, files):
    # Historical 60–65 MB outliers took ~385s. Allow 128 KiB/s plus
    # per-file work and startup reserve, rather than a size-blind 150s floor.
    return 150 + math.ceil(size / (128 * 1024)) + math.ceil(files * 0.05)


def collect_artifacts(ssh, options, host, port, directory, receipt, build_error, *, timeout=150, ttl_deadline=None):
    """One uncompressed archive; measured phases and a finite size-scaled budget."""
    started = time.monotonic()
    deadline = min(started + max(150, timeout), ttl_deadline) if ttl_deadline is not None else started + max(150, timeout)
    collection_error = None
    phases = receipt['collectionPhases'] = {}
    def remaining():
        check_collection_deadline(deadline)
        return deadline - time.monotonic()
    def measured(name, action):
        began = time.monotonic()
        try:
            return action()
        finally:
            phases[name] = round(time.monotonic() - began, 3)
    try:
        # Separate size discovery from packing so a slow transfer still receives
        # the appropriate budget. Retain filesystem and I/O pressure context.
        probe = measured('probeSeconds', lambda: run([*ssh,
            "set -eu; cd /home/user/candidate/output; "
            "du -sb .; find . -type f | wc -l; df -PT .; "
            "cat /proc/pressure/io 2>/dev/null || true"], timeout=remaining()))
        lines = probe.splitlines()
        size, files = int(lines[0].split()[0]), int(lines[1])
        budget = max(150, timeout, collection_budget(size, files))
        deadline = min(started + budget, ttl_deadline) if ttl_deadline is not None else started + budget
        receipt['collectionBudgetSeconds'] = round(deadline - started, 3)
        receipt['collectionTtlCapped'] = deadline < started + budget
        check_collection_deadline(deadline)
        receipt['collectionInput'] = {'bytes': size, 'files': files, 'diskAndPressure': lines[2:]}
        digest = measured('packSeconds', lambda: run([*ssh,
            'set -eu; cd /home/user/candidate; '
            'tar -cf artifacts.tar -C output .; sha256sum artifacts.tar'],
            timeout=remaining())).split()[0]
        if not re.fullmatch('[0-9a-f]{64}', digest):
            raise ValueError('Invalid transport archive digest')
        archive = directory / 'artifacts.tar'
        measured('transferSeconds', lambda: run(['scp', *options, '-P', port,
            f'user@{host}:/home/user/candidate/artifacts.tar', str(archive)], timeout=remaining()))
        measured('restoreSeconds', lambda: restore_archive(archive, directory / 'artifacts', digest, deadline=deadline))
        if time.monotonic() > deadline:
            raise TimeoutError('Collection deadline exceeded (bounded by sandbox TTL)')
        receipt['transport'] = {'kind': 'tar', 'compression': 'none',
                                'sha256': digest, 'bytes': archive.stat().st_size}
    except Exception as error:
        if ttl_deadline is not None and time.monotonic() >= ttl_deadline:
            error = TimeoutError('Collection cannot finish before sandbox TTL expiry: ' + str(error))
        collection_error = error
        receipt['collectionError'] = str(error)
    receipt['collectionSeconds'] = round(time.monotonic() - started, 3)
    if build_error:
        raise build_error
    if collection_error:
        raise collection_error


def worker_name(commit, target, nonce=None):
    return f'putgd-{commit[:12]}-{target}' + (f'-{nonce}' if nonce else '')


def all_sandboxes():
    listing = boat('list', '--all', cleanup=True)
    if listing.get('pageInfo', {}).get('hasMore'):
        raise RuntimeError('Boat list is truncated; cannot verify cleanup')
    return listing['sandboxes']


def cleanup_workers(name, sandbox_id=None, *, recovery_seconds=0, timeout=120):
    """Delete by exact ID, or by this run's unique nonce name; await absence."""
    if name is None and sandbox_id is None:
        raise ValueError('Cleanup requires an exact name or sandbox ID')
    deadline = time.monotonic() + timeout
    recovery_until = time.monotonic() + recovery_seconds
    requested = set()
    result = {'name': name, 'requests': [], 'verified': False}
    while True:
        listing = all_sandboxes()
        owned = [item for item in listing if (name is not None and item.get('name') == name) or item['id'] == sandbox_id]
        for item in owned:
            if item['id'] not in requested:
                deletion = boat('delete', item['id'], '--yes', cleanup=True)
                result['requests'].append({'sandboxId': item['id'], 'operation': deletion.get('operation', {})})
                requested.add(item['id'])
        if not owned and time.monotonic() >= recovery_until:
            result.update(verified=True, finalState='absent', verification='boat list --all',
                          deletedIds=sorted(requested), verifiedAt=time.time())
            return result
        if time.monotonic() >= deadline:
            raise RuntimeError('Worker deletion not complete: ' + str(name or sandbox_id))
        # CANCEL is deliberately ignored here, including repeated terminal signals.
        time.sleep(2)


def cleanup_orphans(commit, target):
    """Sweep only dead local leases or TTL-expired workers; preserve live peers."""
    prefix = worker_name(commit, target)
    pattern = re.compile(re.escape(prefix) + r'(?:-([0-9a-f]{16}))?')
    result = {'deleted': [], 'preserved': [], 'verified': True}
    for item in all_sandboxes():
        match = pattern.fullmatch(item.get('name') or '')
        if not match:
            continue
        if orphaned(item, match[1]):
            result['deleted'].append(cleanup_workers(None, item['id']))
        else:
            result['preserved'].append(item['id'])
    return result


def ci_differences(target):
    common = [
        'Cold isolated Docker build on Boat; no Actions package/SDK/compiler caches or hosted runner preinstalls. No weekly native package snapshot/offline fallback.',
        'No Actions upload, split debug staging, SBOM, source rebuild, publication or desktop acceptance; only selected build/audit artifacts.',
        'Untagged development identity for the exact SHA; no release tag supplied.',
        'PATH prefers distro /usr/bin; compiler cache is private and capped at 256M.',
    ]
    if target == 'appimage':
        common += [
            'Pinned native Ubuntu 24.04 container instead of mutable GitHub ubuntu-24.04 hosted VM; package inventory retained. Hosted preinstalled desktop links can differ (appimage-build.json rootDesktopEntry.removedLinks).',
            'Run hosted_apt_mirrors.py and export QT_PLUGIN_PATH/QML2_IMPORT_PATH/PKG_CONFIG_PATH as install-qt-action does; provision locked Qt with aqtinstall 3.3.* / py7zr 1.1.0 in a fresh venv instead of the cached Action.',
            'Installer uses install-packages.sh defaults (4 attempts, 900s overall and 600s stall per attempt), rather than the AppImage CI dependency step environment (3 attempts, 300s overall, 120s stall). Installer labels default to distro/manager.',
        ]
    elif target == 'ui-catalog':
        common += ['Pinned package-matrix Ubuntu 26.04 digest instead of CI linux floating ubuntu:26.04 tag; same dependency/build/catalog run blocks, no ctest. Compiler cache capped at 256M instead of CI linux 1G.']
    elif target == 'lab-fixture':
        common += [
            'Test-only DEB named putgd-lab-fixture; PUTGD_PACKAGE_TARGET empty and PUTGD_UPDATE_CHECK OFF. It must not enter production staging.',
            'native.py audit intentionally not run: production audit forbids fixture files and requires package name putgd. CPack control metadata, binary/build identity and per-file hashes retained instead.',
        ]
    return common


def safe_print(message, **kwargs):
    try:
        print(message, flush=True, **kwargs)
    except (OSError, ValueError):
        # Prevent Python shutdown from flushing the failed stream a second time.
        stream = kwargs.get('file') or sys.stdout
        try:
            with open(os.devnull, 'w') as sink:
                os.dup2(sink.fileno(), stream.fileno())
        except (OSError, ValueError, AttributeError):
            if stream is sys.stdout:
                sys.stdout = open(os.devnull, 'w')
            elif stream is sys.stderr:
                sys.stderr = open(os.devnull, 'w')


def build_worker(target, args, output, specification):
    started = time.monotonic()
    directory = output / target
    directory.mkdir(mode=0o700)
    receipt = {'artifact': target, 'commit': args.commit, 'snapshot': 'putgd-build-base-v3',
               'placementReason': 'parallel candidate packaging; preserve quiet physical acceptance hosts',
               'ciDifferences': ci_differences(target), 'runNonce': args.run_nonce,
               'name': worker_name(args.commit, target, args.run_nonce),
               'type': args.type, 'ttlSeconds': args.ttl, 'state': 'pending',
               'cleanupVerified': False}
    sandbox_id = None
    deadline = started + args.ttl - 180
    def remaining():
        return max(1, deadline - time.monotonic())
    creation_started = None
    try:
        receipt['startupCleanup'] = cleanup_orphans(args.commit, target)
        if CANCEL.is_set():
            raise TimeoutError('Cancelled before worker creation')
        # Keep the first `created` event even if startup later fails or is interrupted.
        creation = directory / 'creation.jsonl'
        creation_started = time.monotonic()
        try:
            with creation.open('w') as stream:
                run(['boat', '--no-update', '--json', 'new', '--from', 'putgd-build-base-v3',
                     '--no-env', '--type', args.type, '--ttl', str(args.ttl), '--name',
                     receipt['name']], log=stream, timeout=CREATE_TIMEOUT)
        finally:
            for line in creation.read_text().splitlines():
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if event.get('id'):
                    sandbox_id = event['id']
            if sandbox_id:
                receipt['sandboxId'] = sandbox_id
                write_json(directory / 'receipt.json', receipt)
        if not sandbox_id:
            raise RuntimeError('Boat did not return a sandbox ID')
        while True:
            info = boat('info', sandbox_id)['sandbox']
            if info.get('state') in ('error', 'stopped', 'archived', 'deleted'):
                raise RuntimeError('Worker stopped before hydration')
            if info.get('hydrated') is True and info.get('state') in ('ready', 'idle', 'running'):
                break
            if remaining() <= 1 or CANCEL.wait(2):
                raise TimeoutError('Hydration deadline/cancellation')
        receipt['placement'] = {key: info.get(key) for key in
                                ('id', 'sshEndpoint', 'machineProvider', 'vcpu', 'memoryGB',
                                 'hydrated', 'hydratedAt', 'createdAt')}
        receipt['hydrationSeconds'] = round(time.monotonic() - started, 3)
        host, port = info['sshEndpoint'].rsplit(':', 1)
        key = boat('exec', sandbox_id, 'cat', '/etc/ssh/ssh_host_ed25519_key.pub')['stdout'].split()
        if len(key) < 2 or key[0] != 'ssh-ed25519':
            raise ValueError('Invalid host key read through Boat API')
        pin = directory / 'known_hosts'
        pin.write_text(f'[{host}]:{port} {key[0]} {key[1]}\n')
        options = ssh_options(pin)
        ssh = ['ssh', *options, '-p', port, 'user@' + host]
        image, script, preset = specification
        receipt['containerImage'] = image
        write_json(directory / 'receipt.json', receipt)
        (directory / 'recipe.sh').write_text(script)
        # No mutable application products or dependency prefixes from the pilot are reused.
        # Only the public checkout is cloned; local/untracked files never leave the controller.
        bootstrap = f'''set -euo pipefail
umask 077
mkdir -p /home/user/candidate/output
cd /home/user/candidate
git clone {REPOSITORY} source
cd source
git checkout --detach {args.commit}
test "$(git rev-parse HEAD)" = {args.commit}
test -z "$(git status --porcelain)"
cat > /home/user/candidate/recipe.sh <<'PUTGD_CANDIDATE_RECIPE'
set -euo pipefail
sudo() {{ if [[ ${{1:-}} == --preserve-env=* ]]; then shift; fi; "$@"; }}
export -f sudo
{script}
cp build/{preset}/build-identity/build-info.json /out/build-info.json
build/{preset}/putgd-cli --version-json > /out/binary-version.json
git status --porcelain > /out/source-status.txt
test ! -s /out/source-status.txt
(dpkg-query -W || rpm -qa) > /out/packages.txt
PUTGD_CANDIDATE_RECIPE
sudo -n docker run --rm --init --network host --shm-size=1g \\
  -e DEBIAN_FRONTEND=noninteractive -e LANG=C.UTF-8 -e LC_ALL=C.UTF-8 \\
  -v /home/user/candidate/source:/src -v /home/user/candidate/output:/out \\
  -v /home/user/candidate/recipe.sh:/recipe.sh:ro -w /src {shlex.quote(image)} \\
  bash /recipe.sh
sudo -n chown -R user:user /home/user/candidate/output
'''
        # CI's sudo calls run as container root; the shell function drops --preserve-env.
        (directory / 'bootstrap.sh').write_text(bootstrap)
        build_start = time.monotonic()
        build_error = None
        try:
            with (directory / 'build.log').open('w') as log:
                run([*ssh, 'bash -s'], input=bootstrap, log=log, timeout=remaining())
        except Exception as error:
            build_error = error
        receipt['buildSeconds'] = round(time.monotonic() - build_start, 3)
        if target == 'ui-catalog' and getattr(args, 'benchmark_catalog_scp', False) and not build_error:
            legacy_started = time.monotonic()
            try:
                run(['scp', *options, '-P', port, '-r', f'user@{host}:/home/user/candidate/output/.',
                     str(directory / 'legacy-scp')], timeout=remaining())
                receipt['legacyScpStatus'] = 'passed'
            except Exception as error:
                receipt['legacyScpStatus'] = 'failed'
                receipt['legacyScpError'] = str(error)
            receipt['legacyScpSeconds'] = round(time.monotonic() - legacy_started, 3)
        collect_artifacts(ssh, options, host, port, directory, receipt, build_error, timeout=remaining(),
                          ttl_deadline=started + args.ttl)
        receipt['identity'] = verify_identity(directory / 'artifacts', args.commit, target)
        receipt['files'] = inventory(directory / 'artifacts')
        if receipt.get('legacyScpStatus') == 'passed':
            if inventory(directory / 'legacy-scp') != receipt['files']:
                raise ValueError('Batched collection differs from legacy SCP bytes')
            receipt['legacyScpBytesMatch'] = True
        receipt['state'] = 'passed'
    except Exception as error:
        receipt['error'] = str(error)
        receipt['state'] = 'failed'
    finally:
        try:
            # An ID identifies the single create, so ordinary failures need no
            # late-create wait. Without an ID, observe the original window plus
            # at least a short grace after the CLI has stopped (including timeout).
            recovery = 0
            if creation_started is not None and sandbox_id is None:
                recovery = max(LATE_CREATE_GRACE,
                               creation_started + CREATE_TIMEOUT - time.monotonic())
            receipt['cleanup'] = cleanup_workers(receipt['name'], sandbox_id,
                                                 recovery_seconds=recovery,
                                                 timeout=recovery + 120)
            receipt['cleanupVerified'] = receipt['cleanup']['verified']
        except Exception as error:
            receipt['cleanupError'] = str(error)
            receipt['state'] = 'failed'
        receipt['totalSeconds'] = round(time.monotonic() - started, 3)
        write_json(directory / 'receipt.json', receipt)
    return receipt


def main(argv=None):
    args = parser().parse_args(argv)
    CANCEL.clear()
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, lambda *_: CANCEL.set())
    if bool(args.commit) == bool(args.cleanup):
        parser().error('supply either a build commit or --cleanup SHA')
    if args.cleanup:
        if args.artifact:
            parser().error('--artifact is not valid with --cleanup')
        output = prepare_output(args.output)
        results = []
        for target in TARGETS:
            try:
                results.append(dict(target=target, **cleanup_orphans(args.cleanup, target)))
            except Exception as error:
                results.append({'target': target, 'verified': False, 'error': str(error)})
            write_json(output / 'cleanup.json', results)
        return 0 if all(item['verified'] for item in results) else 1
    targets = args.artifact or list(TARGETS)
    if len(targets) != len(set(targets)):
        parser().error('duplicate --artifact')
    packages, ci = load_workflows(args.commit)
    specifications = {target: recipe(target, packages, ci) for target in targets}
    limits = boat('limits')
    check_capacity(limits, len(targets))
    if not any(item['type'] == args.type and item['available'] for item in limits['machineTypes']):
        raise ValueError('Requested Boat machine type is unavailable')
    snapshots = boat('snapshots')['named']
    snapshot = next(item for item in snapshots if item['name'] == 'putgd-build-base-v3')
    output = prepare_output(args.output)
    args.run_nonce = uuid.uuid4().hex[:16]
    started = time.monotonic()
    manifest = {'schemaVersion': 1, 'commit': args.commit, 'repository': REPOSITORY,
                'runNonce': args.run_nonce,
                'snapshot': {key: snapshot[key] for key in ('name', 'snapshotId', 'createdAt')},
                'scope': 'candidate build rehearsal; no desktop or release acceptance',
                'artifacts': []}
    write_json(output / 'manifest.json', manifest)
    with run_lease(args.run_nonce), ThreadPoolExecutor(max_workers=len(targets)) as executor:
        futures = [executor.submit(build_worker, target, args, output, specifications[target])
                   for target in targets]
        for future in as_completed(futures):
            manifest['artifacts'].append(future.result())
            write_json(output / 'manifest.json', manifest)
    manifest['totalSeconds'] = round(time.monotonic() - started, 3)
    manifest['artifacts'].sort(key=lambda item: item['artifact'])
    manifest['passed'] = all(item['state'] == 'passed' and item.get('cleanupVerified')
                             for item in manifest['artifacts'])
    manifest['ownedSandboxesRemaining'] = [item.get('sandboxId', item.get('name')) for item in manifest['artifacts']
                                          if not item.get('cleanupVerified')]
    write_json(output / 'manifest.json', manifest)
    checksums = [f'{item["sha256"]}  {entry["artifact"]}/artifacts/{item["path"]}'
                 for entry in manifest['artifacts'] for item in entry.get('files', [])]
    (output / 'SHA256SUMS').write_text('\n'.join(checksums) + '\n')
    for receipt in manifest['artifacts']:
        safe_print(f'{receipt["artifact"]}: {receipt["state"]} ({receipt.get("totalSeconds")}s)')
    safe_print(f'Manifest: {output / "manifest.json"} ({manifest["totalSeconds"]}s)')
    return 0 if manifest['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, StopIteration, OSError, ImportError) as error:
        safe_print(f'candidate: {error}', file=sys.stderr)
        sys.exit(2)
