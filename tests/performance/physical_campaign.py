#!/usr/bin/env python3
"""Opt-in #379 supervisor for installed release bytes on physical Cinnamon.

Run under the campaign coordinator's existing desktop lock. The capture phase
reuses bench_capture.cpp unchanged (5 warmups, 30 samples, exact helper owner).
This supervisor is separate from frozen run_capture.py because that supervisor
always creates an empty profile. --fixture-root consumes #495's stopped,
independent 10,000-operation fixture; it never manufactures history rows.

Example (all output/profile directories must be new):
  python3 tests/performance/physical_campaign.py capture --output /tmp/p379-c1 \
    --package /private/release/putgd.deb --package-sha256 SHA --version VERSION \
    --source-revision COMMIT --desktop-session ID --lock-session OWNER \
    --driver /private/build/putgd-bench-capture
Use the same command with --fixture-root ROOT for populated capture, or replace
capture with memory for cold/closed 300-second CPU/RSS and open-History PSS.
No history response threshold is inferred. Raw evidence stays private.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import shutil
import sqlite3
import stat
import socket
import subprocess
import sys
import tarfile
import time
import uuid

from bench_ui_process import (EvidenceError, ProcessFamily, identity,
                              request, same_identity, sha256, stat_fields, verify_socket_owner,
                              write_json)
from observe_idle import check_physical_session, environment_metadata, owned_windows, stop_owned_process_group

REPO = Path(__file__).resolve().parents[2]
MIB = 1024 ** 2


def require(condition, code):
    if not condition:
        raise EvidenceError(code)


def private_path(path, directory=False):
    """Reject links, other owners and group/world access on private inputs."""
    path = Path(path)
    info = path.lstat()
    require((stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode))
            and info.st_uid == os.getuid() and info.st_mode & 0o077 == 0,
            'private_owned_input_required')
    return path.resolve(strict=True)


def loopback_endpoint(value):
    from urllib.parse import urlsplit
    try:
        parsed = urlsplit(value)
        return (parsed.scheme == 'http' and parsed.hostname == '127.0.0.1'
                and parsed.port is not None and 1 <= parsed.port <= 65535
                and not parsed.username and not parsed.password
                and parsed.path in ('', '/') and not parsed.query and not parsed.fragment)
    except ValueError:
        return False


def safe_document(document):
    """Only local actions and literal IPv4 loopback fixture destinations."""
    for profile in document.get('profiles', []):
        require(profile.get('provider') == 's3', 'fixture_provider_not_s3')
        options = profile.get('settings', {})
        require(loopback_endpoint(options.get('endpoint', '')), 'unsafe_fixture_endpoint')
        require(not any(key in options for key in ('accessKey', 'secretKey', 'sessionToken')),
                'inline_credentials_refused')
        credentials = profile.get('credentials', {})
        require(set(credentials) <= {'mode', 'reference'}, 'inline_credentials_refused')
    return document


def fixture_summary(database, expected=10000):
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as connection:
        require(connection.execute('PRAGMA integrity_check').fetchone() == ('ok',),
                'fixture_integrity_failed')
        count, operations, items = connection.execute(
            'SELECT count(*),count(DISTINCT operation_id),count(DISTINCT item_id) FROM history').fetchone()
        require(count == operations == items == expected, 'fixture_not_independent_10000')
        missing = connection.execute('SELECT count(*) FROM history h LEFT JOIN operations o '
                                     'ON h.operation_id=o.id LEFT JOIN item_state s '
                                     'ON h.id=s.id AND h.operation_id=s.operation_id '
                                     'WHERE o.id IS NULL OR s.id IS NULL').fetchone()[0]
        require(missing == 0, 'fixture_missing_journal_or_item_state')
        require(connection.execute("SELECT count(*) FROM operations WHERE json_extract(data,'$.state') "
                                   "NOT IN ('completed','partial','failed','cancelled','interrupted') "
                                   "OR json_extract(data,'$.state') IS NULL").fetchone()[0] == 0,
                'fixture_active_or_invalid_operation')
        kinds = dict(connection.execute('SELECT category,count(*) FROM history GROUP BY category'))
        require(kinds.get('images', 0) > 0 and kinds.get('videos', 0) > 0,
                'fixture_requires_images_and_videos')
        version = connection.execute('PRAGMA user_version').fetchone()[0]
        require(5 <= version <= 7, 'unsupported_fixture_schema')
        return {'historyCount': count, 'independentOperations': operations,
                'independentItems': items, 'categories': kinds,
                'schemaVersion': version,
                'databaseSha256': sha256(database)}


def relocate(value, old, new):
    if isinstance(value, str):
        # JSON blobs contain quoted absolute paths, as do plain path columns.
        return value.replace(str(old) + '/', str(new) + '/')
    if isinstance(value, list):
        return [relocate(item, old, new) for item in value]
    if isinstance(value, dict):
        return {key: relocate(item, old, new) for key, item in value.items()}
    return value


def copy_fixture(source, destination):
    """Copy stopped fixture data; never mutate the source or share writable DBs."""
    source = private_path(source, directory=True)
    database = source / 'data/putgd/history.sqlite'
    private_path(database)
    fixture_stopped(source, database)
    require(not any((source / name).exists() for name in ('runtime/putgd/session.sock',)),
            'fixture_socket_present_stop_and_retire_endpoint_first')
    require(not Path(str(database) + '-wal').exists(), 'fixture_has_wal_checkpoint_and_stop_first')
    summary = fixture_summary(database)
    def checked_paths(value):
        if isinstance(value, dict):
            for key, item in value.items():
                if key in ('path', 'previewPath', 'root') and isinstance(item, str) and item:
                    require(Path(item).is_absolute() and Path(item).resolve().is_relative_to(source),
                            'fixture_payload_path_escapes_root')
                if key == 'endpoint' and item:
                    require(loopback_endpoint(item), 'persisted_fixture_endpoint_not_loopback')
                if key in ('provider', 'providerId') and isinstance(item, str) and item:
                    require(item == 's3', 'persisted_fixture_provider_not_s3')
                if key in ('uploadJournal', 'uploadId'):
                    require(not item, 'persisted_multipart_cleanup_refused')
                if key == 'credentials' and item:
                    require(isinstance(item, dict) and item == {'mode': 'session', 'reference': None},
                            'persisted_non_synthetic_credential_reference')
                checked_paths(item)
        elif isinstance(value, list):
            for item in value:
                checked_paths(item)
        elif isinstance(value, str) and value.startswith(('http://', 'https://')):
            from urllib.parse import urlsplit
            url = urlsplit(value)
            require(url.scheme == 'http' and url.hostname == '127.0.0.1' and url.port
                    and not url.username and not url.password, 'persisted_fixture_url_not_loopback')
    checked_paths(json.loads((source / 'config/putgd/config.json').read_text()))
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as connection:
        tables = {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        for table in ('deletions', 'deletion_targets', 'sharing_operations'):
            if table in tables:
                require(connection.execute(f'SELECT count(*) FROM {table}').fetchone()[0] == 0,
                        'fixture_contains_unreviewed_management_journal')
        for table in ('operations', 'history', 'item_state'):
            for (value,) in connection.execute(f'SELECT data FROM {table}'):
                checked_paths(json.loads(value))
    for directory in ('config', 'data', 'archive', 'cache', 'media'):
        original = source / directory
        if not original.exists():
            continue
        for path in [original, *original.rglob('*')]:
            info = path.lstat()
            require(info.st_uid == os.getuid() and
                    (stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode)),
                    'fixture_link_or_special_file_refused')
        shutil.copytree(original, destination / directory, dirs_exist_ok=True)
    document = safe_document(json.loads((destination / 'config/putgd/config.json').read_text()))
    write_json(destination / 'config/putgd/config.json', relocate(document, source, destination))
    # #495 owns distinct operation/item/history IDs. Only paths in the copied
    # fixture move; SQL identifiers come from the copied schema, quoted safely.
    with sqlite3.connect(destination / 'data/putgd/history.sqlite') as connection:
        for (table,) in connection.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall():
            quoted = '"' + table.replace('"', '""') + '"'
            columns = connection.execute(f'PRAGMA table_info({quoted})').fetchall()
            for column in columns:
                name = '"' + column[1].replace('"', '""') + '"'
                old, new = str(source) + '/', str(destination) + '/'
                connection.execute(f'UPDATE {quoted} SET {name}=CASE WHEN typeof({name})="blob" '
                                   f'THEN CAST(replace(CAST({name} AS TEXT),?,?) AS BLOB) '
                                   f'ELSE replace({name},?,?) END WHERE typeof({name}) IN ("text","blob") '
                                   f'AND instr(CAST({name} AS TEXT),?)>0', (old, new, old, new, old))
    return summary


def fixture_stopped(root, database):
    """Associate liveness with the actual DB, never only a supplied runtime."""
    expected = database.stat()
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            fields = (process / 'environ').read_bytes().split(b'\0')
            associated = f'XDG_DATA_HOME={root / "data"}'.encode() in fields
            require(not associated, 'fixture_data_profile_has_live_process')
            for descriptor in (process / 'fd').iterdir():
                try:
                    current = descriptor.stat()
                    require((current.st_dev, current.st_ino) != (expected.st_dev, expected.st_ino),
                            'fixture_database_has_live_owner')
                except FileNotFoundError:
                    pass
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue


def verify_package(args):
    package = Path(args.package).resolve(strict=True)
    require(sha256(package) == args.package_sha256, 'package_sha256_mismatch')
    paths = {label: Path(getattr(args, label)).resolve(strict=True)
             for label in ('app', 'cli', 'helper', 'capture_helper')}
    # Stream exact file bytes out of the verified DEB. No package extraction or
    # installation, and no temporary executable can be selected as the product.
    expected = {str(path).lstrip('/'): label for label, path in paths.items()}
    require(len(expected) == len(paths), 'product_paths_must_be_distinct')
    matches = {}
    process = subprocess.Popen(['dpkg-deb', '--fsys-tarfile', str(package)],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
            for entry in archive:
                name = entry.name.removeprefix('./')
                if name not in expected:
                    continue
                require(entry.isfile(), 'package_binary_not_regular')
                digest = hashlib.sha256()
                with archive.extractfile(entry) as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        digest.update(block)
                label = expected[name]
                require(sha256(paths[label]) == digest.hexdigest(), 'installed_binary_differs_from_deb')
                manifest = json.loads(subprocess.check_output([str(paths[label]), '--version-json'],
                                                              stderr=subprocess.DEVNULL, timeout=10))
                require(manifest.get('gitCommit') == args.source_revision and
                        manifest.get('version') == args.version, 'product_build_identity_mismatch')
                matches[label] = {'path': str(paths[label]), 'sha256': digest.hexdigest(), 'build': manifest}
        require(process.wait(timeout=20) == 0, 'package_stream_failed')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
    require(len(matches) == 4, 'product_binary_not_in_package')
    return {'path': str(package), 'sha256': args.package_sha256, 'binaries': matches}


def check_lock(args):
    path = Path.home() / 'putgd-lab/locks/desktop.lock'
    record = dict(line.split(': ', 1) for line in path.read_text().splitlines() if ': ' in line)
    now = datetime.now(timezone.utc)
    require(record.get('session') == args.lock_session, 'coordinator_lock_session_mismatch')
    require(datetime.fromisoformat(record['expires'].replace('Z', '+00:00')) > now and
            (now - datetime.fromisoformat(record['heartbeat'].replace('Z', '+00:00'))).total_seconds() < 900,
            'coordinator_lock_expired_or_stale')
    return record


def physical_identity(args):
    check_physical_session()
    require(os.environ.get('DISPLAY') == ':0', 'physical_display_zero_required')
    check_lock(args)
    text = subprocess.check_output(['loginctl', 'show-session', args.desktop_session,
                                   '-p', 'Active', '-p', 'Remote', '-p', 'Type', '-p', 'Desktop',
                                   '-p', 'Name', '-p', 'Display', '-p', 'Leader'], text=True, timeout=10)
    session = dict(line.split('=', 1) for line in text.splitlines() if '=' in line)
    require(session.get('Active') == 'yes' and session.get('Remote') == 'no' and
            session.get('Type') == 'x11' and 'cinnamon' in session.get('Desktop', '').lower() and
            session.get('Display') == ':0', 'loginctl_physical_cinnamon_mismatch')
    return {'hostname': os.uname().nodename, 'kernel': os.uname().release, 'session': session,
            'sessionId': args.desktop_session, 'requestedQtPlatform': 'xcb',
            'systemVersions': environment_metadata(),
            'osRelease': Path('/etc/os-release').read_text(),
            'loadAverage': Path('/proc/loadavg').read_text().split()[:3]}


class Campaign:
    def __init__(self, args):
        self.args = args
        self.repo = REPO
        raw = Path(args.output).absolute()
        require(not raw.is_symlink() and not raw.exists(), 'output_must_be_new')
        self.root = raw.resolve()
        self.root.mkdir(mode=0o700)
        self.started = time.monotonic()
        self.resident = self.family = self.log = self.cover = self.clipboard = None
        revision = args.driver_revision
        if revision is None:
            revision = subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'],
                                               text=True, stderr=subprocess.DEVNULL).strip()
        require(re.fullmatch('[0-9a-f]{40}', revision), 'invalid_driver_revision')
        self.result = {'schemaVersion': 1, 'scenario': args.scenario, 'status': 'starting',
                       'startedUtc': datetime.now(timezone.utc).isoformat(),
                       'driver': {'sha256': sha256(__file__), 'revision': revision,
                                  'revisionSource': 'explicit-declaration' if args.driver_revision else 'git-HEAD',
                                  'sourceFiles': {str(path.relative_to(REPO)): sha256(path) for path in (
                                      Path(__file__), Path(__file__).with_name('bench_ui_process.py'),
                                      Path(__file__).with_name('observe_idle.py'),
                                      Path(__file__).with_name('campaign_fixture.py'),
                                      Path(__file__).with_name('campaign_ui.py'),
                                      Path(__file__).with_name('campaign_workloads.py'),
                                      Path(__file__).with_name('campaign_proxy.py'),
                                      REPO / 'tools/diagnostics/check_socket_paths.py')}},
                       'product': {}, 'parameters': vars(args), 'metrics': {}, 'gates': {},
                       'limitations': ['Capture witness observes X11 drawable paint, not compositor presentation.',
                           'CLI acknowledgements do not prove painted response.',
                           'Sequential sampled process memory can miss short-lived descendants.',
                           'Family CPU is a lower bound when a child exits between samples.',
                           'Shared desktop daemons are outside the resident process family.',
                           'Host quietness and reviewed sanitized visuals require coordinator evidence.']}
        self.environment = os.environ.copy()
        for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                          ('XDG_STATE_HOME', 'state'), ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
            path = self.root / name
            path.mkdir(mode=0o700)
            self.environment[key] = str(path)
        self.environment['QT_QPA_PLATFORM'] = 'xcb'
        if args.scenario in ('history', 'burst', 'slow') and args.enable_accessibility:
            self.environment['QT_LINUX_ACCESSIBILITY_ALWAYS_ON'] = '1'
        for name in ('APPIMAGE', 'APPDIR', 'PUTGD_TEST_TIME_SCALE'):
            self.environment.pop(name, None)
        self.socket = str(self.root / 'runtime/putgd/session.sock')
        self.persist()

    def persist(self):
        write_json(self.root / 'results.json', self.result)

    def event(self, kind, **values):
        row = {'kind': kind, 'elapsedSeconds': time.monotonic() - self.started, **values}
        with (self.root / 'events.jsonl').open('a') as stream:
            stream.write(json.dumps(row, separators=(',', ':')) + '\n')
        return row

    def initialize(self):
        # Call the repository preflight before *any* isolated application starts.
        checked = subprocess.run([sys.executable, str(REPO / 'tools/diagnostics/check_socket_paths.py'),
                                  '--socket', self.socket], capture_output=True, text=True, timeout=5)
        self.result['socketPreflight'] = json.loads(checked.stdout)
        self.persist()
        require(checked.returncode == 0, 'socket_path_too_long')
        self.result['physical'] = physical_identity(self.args)
        self.result['product'] = verify_package(self.args)
        if self.args.scenario in ('seed', 'history', 'burst', 'slow'):
            verify_fixture(self.args)
        self.result['harness'] = {name: sha256(Path(__file__).with_name(name))
                                  for name in ('run_capture.py', 'bench_capture.cpp', 'bench_ui_process.py')}
        if self.args.fixture_root:
            self.result['fixture'] = copy_fixture(Path(self.args.fixture_root), self.root)
        self.persist()

    def start(self, platform='xcb'):
        self.environment['QT_QPA_PLATFORM'] = platform
        self.log = (self.root / 'resident.log').open('ab')
        self.resident = subprocess.Popen([self.args.app, '--background'], env=self.environment,
                                         stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log,
                                         start_new_session=True)
        self.family = None
        # Wait until /proc/exe is the launched product, not the forking Python.
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            require(self.resident.poll() is None, 'resident_exited_before_readiness')
            try:
                self.family = ProcessFamily(self.resident.pid, self.args.app, self.args.helper)
                if self.call('diagnostics').get('journalReady') is True:
                    break
            except (FileNotFoundError, ConnectionError, EvidenceError, subprocess.SubprocessError, json.JSONDecodeError):
                time.sleep(.1)
        else:
            raise EvidenceError('resident_readiness_timeout')
        self.event('resident-started', identity=self.family.root, platform=platform)

    def call(self, method, params=None):
        verify_socket_owner(self.socket, self.family)
        return request(self.args.cli, method, params, environment=self.environment, socket=self.socket)

    def stop(self):
        if self.resident and self.resident.poll() is None:
            try:
                require(self.family is not None, 'resident_never_ready')
                self.call('ui.quit')
                code = self.resident.wait(timeout=15)
                self.event('resident-normal-exit', pid=self.resident.pid, exitCode=code,
                           socketAbsent=not Path(self.socket).exists())
            except (OSError, EvidenceError, subprocess.SubprocessError, json.JSONDecodeError):
                self.event('resident-required-termination', pid=self.resident.pid)
                stop_owned_process_group(self.resident)
        if self.log:
            self.log.close()
        self.resident = self.log = None

    def static_cover(self):
        if self.cover is not None:
            require(self.cover.poll() is None, 'static_cover_exited')
            return
        path = REPO / 'tools/lab/cover_player.py'
        self.result['staticCover'] = {'sourceSha256': sha256(path), 'color': '505a64',
                                      'animation': False, 'chargedToResidentFamily': False}
        command = [sys.executable, str(path), 'play', '--color', '505a64',
                   '--title', 'put.gd physical performance — synthetic static background']
        self.cover = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.DEVNULL, text=True, start_new_session=True)
        reply = read_line(self.cover, 15)
        require(reply.get('ok') and reply.get('state') == 'covered', 'static_cover_failed')
        checked = subprocess.run([sys.executable, str(path), 'precheck', '--port', str(self.args.vnc_port),
                                  '--color', '505a64', '--min-fraction', '0.98'],
                                 capture_output=True, text=True, timeout=20)
        require(checked.returncode == 0, 'static_cover_not_visible_on_wall')
        self.event('static-cover-visible', pid=self.cover.pid, readiness=reply,
                   vncPrecheck=json.loads(checked.stdout))
        self.persist()

    def visual(self, label):
        """Save only owned UI drawables, or a prechecked synthetic cover frame."""
        from PIL import Image
        from campaign_ui import Drawable
        if self.family and self.resident and self.resident.poll() is None:
            observed = self.sample(label + '-identity')
            saved = False
            for owner in observed['processes']:
                if owner['role'] != 'ui':
                    continue
                for index, window in enumerate(owned_windows(owner['pid'])):
                    info = subprocess.check_output(['xwininfo', '-id', window], text=True, timeout=5)
                    width = int(re.search(r'Width:\s+(\d+)', info)[1])
                    height = int(re.search(r'Height:\s+(\d+)', info)[1])
                    draw = Drawable(window, owner)
                    path = self.root / f'{label}-{owner["pid"]}-{index}.png'
                    try:
                        draw.pixels([0, 0, width, height]).save(path)
                    finally:
                        draw.close()
                    self.event('owned-ui-visual', label=label, file=path.name, sha256=sha256(path), owner=owner)
                    saved = True
            if saved:
                return
        require(self.cover is not None and self.cover.poll() is None, 'no_safe_visual_cover')
        sys.path.insert(0, str(REPO / 'tools/lab'))
        import rfb
        from cover_player import coverage
        client = rfb.RFB(self.args.vnc_port)
        try:
            width, height, pixels = client.frame()
        finally:
            client.close()
        require(coverage(width, height, pixels, 'bgrx', [0, 0, width, height], (80, 90, 100), 10) >= .98,
                'visual_frame_not_synthetic_covered')
        path = self.root / (label + '.png')
        Image.frombytes('RGB', (width, height), pixels, 'raw', 'BGRX').save(path)
        self.event('synthetic-cover-visual', label=label, file=path.name, sha256=sha256(path),
                   limitation='Synthetic cover; companion separately verifies actual desktop tray presentation')

    def close_fixtures(self):
        for process in (self.clipboard, self.cover):
            if process is None:
                continue
            if process.poll() is None:
                try:
                    process.stdin.write('{"action":"quit"}\n' if process is self.clipboard else 'quit\n')
                    process.stdin.flush()
                    process.wait(timeout=5)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    stop_owned_process_group(process)
            process.stdin.close()
            process.stdout.close()

    def clipboard_command(self, action, **values):
        if self.clipboard is None:
            self.clipboard = subprocess.Popen([sys.executable, str(Path(__file__).with_name('campaign_fixture.py')),
                '--root', str(self.root)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, start_new_session=True)
            require(read_line(self.clipboard, 15).get('ok'), 'clipboard_owner_not_ready')
        self.clipboard.stdin.write(json.dumps({'action': action, **values}) + '\n')
        self.clipboard.stdin.flush()
        value = read_line(self.clipboard, 15)
        require(value.get('ok'), 'independent_clipboard_command_failed')
        return value

    def fixture(self, path='/__state', value=None):
        """Never persist raw fixture state: it contains request headers/bodies."""
        from urllib.request import Request, build_opener, ProxyHandler
        verify_fixture(self.args)
        url = self.args.endpoint.rstrip('/') + path
        request = Request(url, data=json.dumps(value).encode() if value is not None else None,
                          headers={'Content-Type': 'application/json'})
        with build_opener(ProxyHandler({})).open(request, timeout=15) as reply:
            return json.load(reply)

    def upload_configuration(self):
        verify_fixture(self.args)
        document = self.call('settings.get')['document']
        template = self.call('settings.developmentTemplate')['document']['profiles'][0]
        template.update(id='physical-fixture', label='Isolated loopback performance fixture',
                        credentials={'mode': 'session', 'reference': None})
        template['settings'].update(endpoint=self.args.endpoint, bucket='fixture', region='us-east-1',
            publicUrlBase=self.args.endpoint.rstrip('/') + '/fixture', objectPrefix='physical379/',
            aclMode='public-read', requestTimeoutMs=120000, lowSpeedTimeSeconds=120)
        document['profiles'] = [template]
        document['limits'].update(pendingOperations=64, concurrentUploads=4)
        document['naming']['mode'] = 'original'
        for action in document['actions'].values():
            if 'destination' in action:
                action.update(destination='upload', profileId='physical-fixture', shortcut='')
        self.call('settings.patch', {'patch': document,
                                    'expectedRevision': self.call('settings.get')['metadata']['observedRevision']})
        # Synthetic fixture credentials travel only through the authenticated
        # production session method, never CLI argv, config, logs or receipts.
        verify_socket_owner(self.socket, self.family)
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(20)
            connection.connect(self.socket)
            request_id = str(uuid.uuid4())
            connection.sendall(json.dumps({'schemaVersion': 1, 'requestId': request_id,
                'session': self.socket, 'method': 'setup.storeCredentials', 'params': {
                    'profileId': 'physical-fixture',
                    'expectedRevision': self.call('settings.get')['metadata']['acceptedRevision'],
                    'credentials': {'accessKey': 'PUTGD_SYNTHETIC_ACCESS',
                                    'secretKey': 'putgd-synthetic-secret-not-a-real-key'}}}).encode() + b'\n')
            data = bytearray()
            while not data.endswith(b'\n'):
                chunk = connection.recv(65536)
                require(bool(chunk) and len(data) < 1024**2, 'synthetic_setup_transport_failed')
                data.extend(chunk)
            reply = json.loads(data)
        require(reply.get('ok') and reply.get('requestId') == request_id, 'synthetic_setup_refused')
        setup_id = reply['result']['setupId']
        deadline = time.monotonic() + 20
        while True:
            job = self.call('setup.inspect', {'id': setup_id})
            if job['state'] != 'running':
                require(job['state'] == 'completed', 'synthetic_setup_failed')
                break
            require(time.monotonic() < deadline, 'synthetic_setup_timeout')
            time.sleep(.1)
        self.event('loopback-destination-ready', endpoint=self.args.endpoint, setupId=setup_id,
                   concurrentUploads=4, pendingOperations=64, credentials='synthetic-session-only')

    def wait_operation(self, operation_id, seconds=180):
        deadline = time.monotonic() + seconds
        while True:
            result = self.call('operation.inspect', {'id': operation_id})
            if result['state'] in ('completed', 'failed', 'partial', 'cancelled'):
                return result
            require(time.monotonic() < deadline, 'operation_timeout')
            self.sample('operation-active')
            time.sleep(.25)

    def seed(self):
        from campaign_fixture import png
        self.upload_configuration()
        require(not self.fixture()['objects'], 'seed_fixture_must_start_empty')
        media = self.root / 'media'
        media.mkdir(mode=0o700)
        operations = []
        for index in range(6):
            if index % 2 == 0:
                path = media / f'landscape-{index:02d}-synthetic.png'
                png(path, index, 640 + index * 32, 360 + index * 16)
            else:
                path = media / f'motion-{index:02d}-synthetic.webm'
                # Distro GStreamer, deterministic finite sources; no FFmpeg CLI.
                command = ['gst-launch-1.0', '-q', 'videotestsrc', 'num-buffers=20',
                    f'pattern={index}', '!', f'video/x-raw,width={320 + index * 16},height=180,framerate=10/1',
                    '!', 'videoconvert', '!', 'vp8enc', 'deadline=1', 'threads=1', '!', 'webmmux',
                    '!', 'filesink', 'location=' + str(path)]
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
                self.event('synthetic-video-generated', command=command)
            self.clipboard_command('file', path=str(path))
            operation_id = self.call('action.invoke', {'action': 'clipboard'})['operationId']
            operation = self.wait_operation(operation_id)
            require(operation['state'] == 'completed' and len(operation['items']) == 1 and
                    operation['items'][0]['uploadStatus'] == 'uploaded', 'seed_upload_failed')
            item_id = operation['items'][0]['artifact']['id']
            preview = self.call('history.preview', {'id': item_id})
            self.event('synthetic-seed', operationId=operation_id, itemId=item_id, name=path.name,
                       bytes=path.stat().st_size, sha256=sha256(path), previewStored=bool(preview.get('previewPath')))
            operations.append(operation_id)
        self.clipboard_command('clear')
        rows = self.call('history.list', {'limit': 20})
        require(rows['total'] == 6 and len(set(operations)) == 6, 'seed_independent_operations_mismatch')
        self.result['metrics']['seed'] = {'historyRows': 6, 'operations': operations, 'images': 3, 'videos': 3,
            'captureSource': 'independent GTK4 text/uri-list owner, production action.invoke clipboard',
            'growRequired': 'Use #495 grow on this stopped root; no 10000 real uploads implied.'}
        self.visual('seed-completed')

    def configure(self):
        self.start('offscreen')
        # Inspect actual production controls and accepted settings first.
        capabilities = self.call('capabilities')
        write_json(self.root / 'capabilities-setup.json', capabilities)
        self.call('settings.get')
        document = self.call('settings.developmentTemplate')['document']
        document['profiles'] = []
        document['actions'] = {name: ({'shortcut': ''} if 'destination' not in action else
            {'destination': 'local', 'profileId': None,
             'shortcut': 'Ctrl+Shift+F12' if name == 'region' and self.args.scenario == 'capture' else ''})
            for name, action in document['actions'].items()}
        document['archive']['root'] = str(self.root / 'archive')
        document['archive']['keepUploads'] = True
        document['preferences'].update(notifications=False, launchAtLogin=False)
        revision = self.call('settings.get')['metadata']['observedRevision']
        self.call('settings.patch', {'patch': document, 'expectedRevision': revision})
        self.stop()

    def checked_live(self):
        capabilities = self.call('capabilities')
        write_json(self.root / 'capabilities.json', capabilities)
        snapshot = self.call('ui.snapshot')
        require(snapshot['build']['gitCommit'] == self.args.source_revision and
                snapshot['build']['version'] == self.args.version, 'live_build_mismatch')
        require(self.call('history.list', {'limit': 1})['total'] == (10000 if self.args.fixture_root else 0),
                'live_history_total_mismatch')
        self.result['liveBuild'] = snapshot['build']
        self.result['status'] = 'running'
        self.persist()
        return capabilities

    def sample(self, label):
        row = self.family.sample()
        require(row['escapedProcessCount'] == 0, 'escaped_product_descendant')
        for process in row['processes']:
            process['qtPlatformMappings'] = platform_mappings(process['pid'])
        self.event('process-sample', label=label, **row)
        return row

    def summarize_processes(self):
        path = self.root / 'events.jsonl'
        if not path.exists():
            return
        rows = [value for line in path.read_text().splitlines()
                if (value := json.loads(line)).get('kind') == 'process-sample']
        if not rows:
            return
        cpu = {}
        for row in rows:
            for process in row['processes']:
                key = (process['pid'], process['startTicks'])
                cpu.setdefault(key, [process['cpuTicks'], process['cpuTicks']])[1] = process['cpuTicks']
        elapsed = rows[-1]['elapsedSeconds'] - rows[0]['elapsedSeconds']
        self.result['metrics']['sampledProcessFamily'] = {
            'peakTotals': {name: max(row['totals'][name] for row in rows)
                           for name in ('rssBytes', 'pssBytes', 'privateBytes')},
            'cpuPercentOneCoreLowerBound': (100 * sum(end - start for start, end in cpu.values()) /
                                           os.sysconf('SC_CLK_TCK') / elapsed) if elapsed > 0 else None,
            'observedProcessGenerations': len(cpu), 'samplingSpanSeconds': elapsed,
            'shortLivedChildrenMayBeMissed': True}

    def interval(self, label, seconds, closed=False):
        rows = []
        started = None
        while True:
            check_lock(self.args)
            require(self.cover is None or self.cover.poll() is None, 'static_cover_exited')
            row = self.sample(label)
            if closed:
                require(row['processCount'] == 1, 'closed_stage_has_live_helpers')
                require(not owned_windows(self.resident.pid), 'closed_stage_has_owned_windows')
            rows.append((time.monotonic(), row))
            if started is None:
                started = rows[0][0]
            remaining = seconds - (time.monotonic() - started)
            if remaining <= 0:
                return rows
            time.sleep(min(5, remaining))

    def capture(self, capabilities):
        require(capabilities['source']['shortcuts']['bindings'].get('region', {}).get('bound'),
                'capture_shortcut_unbound')
        driver = Path(self.args.driver).resolve(strict=True)
        self.result['captureDriver'] = {'path': str(driver), 'sha256': sha256(driver)}
        command = [str(driver), '--resident-pid', str(self.resident.pid), '--capture-helper',
                   self.args.capture_helper, '--live-outline', '--output', str(self.root / 'capture.json')]
        self.event('capture-command', command=command)
        before = operation_ids(self.root / 'data/putgd/history.sqlite')
        with (self.root / 'capture-driver.log').open('xb') as log:
            completed = subprocess.run(command, env=self.environment, stdout=log, stderr=log, timeout=240)
        data = json.loads((self.root / 'capture.json').read_text())
        for index, row in enumerate(data['samples']):
            self.event('capture-sample', index=index, **row)
        require(len(data['samples']) == 30 and data['warmupInvocations'] == 5 and not data['error'],
                'incomplete_capture_workload')
        operations = operation_ids(self.root / 'data/putgd/history.sqlite') - before
        require(len(operations) == 35, 'capture_operation_count_mismatch')
        outcomes = [self.call('operation.inspect', {'id': operation}) for operation in sorted(operations)]
        require(all(row.get('state') == 'cancelled' for row in outcomes), 'capture_did_not_cancel_cleanly')
        self.event('capture-outcomes', operations=[{'id': row['id'], 'state': row['state']} for row in outcomes])
        self.result['metrics']['warmP95DrawablePaintMs'] = data['p95PaintMs']
        self.result['gates']['warmCapture'] = {'status': 'pass' if data['p95PaintMs'] <= 150 else 'fail',
                                              'limitMs': 150, 'driverExit': completed.returncode}
        self.persist()
        self.capture_visual()

    def capture_visual(self):
        """One separate unscored selection; no VNC/encoding during scored35."""
        self.static_cover()
        self.family.expected_hashes[str(Path(self.args.capture_helper).resolve())] = (
            self.result['product']['binaries']['capture_helper']['sha256'])
        operation_id = self.call('action.invoke', {'action': 'region'})['operationId']
        helper = None
        pressed = False
        try:
            deadline = time.monotonic() + 10
            while helper is None:
                observed = self.sample('unscored-selector-wait')
                for process in observed['processes']:
                    if process['executable'] == str(Path(self.args.capture_helper).resolve()):
                        require(stat_fields(Path(f"/proc/{process['pid']}/stat").read_text())['ppid'] == self.resident.pid,
                                'visual_capture_helper_not_immediate_child')
                        helper = process
                require(time.monotonic() < deadline, 'visual_capture_helper_missing')
                if helper is None:
                    time.sleep(.05)
            deadline = time.monotonic() + 10
            windows = []
            while not windows:
                require(same_identity(identity(helper['pid']), helper), 'visual_capture_helper_changed')
                tree = subprocess.check_output(['xwininfo', '-root', '-tree'], text=True, timeout=5)
                for window in re.findall(r'^\s*(0x[0-9a-fA-F]+)', tree, re.M):
                    prop = subprocess.run(['xprop', '-id', window, '_NET_WM_PID'], capture_output=True,
                                          text=True, timeout=5)
                    if not re.search(r'=\s*' + str(helper['pid']) + r'\s*$', prop.stdout):
                        continue
                    info = subprocess.check_output(['xwininfo', '-id', window], text=True, timeout=5)
                    if 'Map State: IsViewable' in info and 'Override Redirect State: yes' in info:
                        windows.append(window)
                require(time.monotonic() < deadline, 'visual_owned_selector_did_not_map')
                if not windows:
                    time.sleep(.05)
            def input(*arguments):
                require(same_identity(identity(helper['pid']), helper), 'visual_capture_helper_changed')
                subprocess.run(['xdotool', *arguments], check=True, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
            input('mousemove', '--sync', '140', '180')
            input('mousedown', '1')
            pressed = True
            input('mousemove', '--sync', '900', '650')
            time.sleep(.3)  # Unscored visual settling, explicitly excluded above.
            sys.path.insert(0, str(REPO / 'tools/lab'))
            import rfb
            from cover_player import coverage
            client = rfb.RFB(self.args.vnc_port)
            try:
                width, height, pixels = client.frame()
            finally:
                client.close()
            # Same actual frame is checked and saved; never export a frame
            # whose synthetic background is hidden by an unrelated window.
            fraction = coverage(width, height, pixels, 'bgrx', [0, 0, width, height], (80, 90, 100), 10)
            require(fraction >= .98, 'visual_frame_not_synthetic_covered')
            require(same_identity(identity(helper['pid']), helper), 'visual_capture_helper_changed')
            from PIL import Image
            path = self.root / 'unscored-capture-overlay.png'
            Image.frombytes('RGB', (width, height), pixels, 'raw', 'BGRX').save(path)
            self.event('unscored-capture-visual', operationId=operation_id, helperIdentity=helper,
                       windows=windows, syntheticBackgroundFraction=fraction, screenshot=path.name,
                       sha256=sha256(path), scored=False, drag=[140, 180, 900, 650])
        finally:
            if helper and same_identity(identity(helper['pid']), helper):
                subprocess.run(['xdotool', 'key', 'Escape'], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
            if pressed:
                subprocess.run(['xdotool', 'mouseup', '1'], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5)
        outcome = self.wait_operation(operation_id, seconds=15)
        require(outcome['state'] == 'cancelled', 'unscored_visual_selection_did_not_cancel')
        self.event('unscored-capture-cancelled', operationId=operation_id, state=outcome['state'],
                   timedOperations=35, visualOperations=1)

    def memory(self):
        time.sleep(5)
        cold = self.interval('cold-closed', 300, closed=True)
        self.visual('cold-closed-completed')
        self.result['metrics']['coldClosed'] = memory_summary(cold)
        for name, value in memory_gates(self.result['metrics']['coldClosed']).items():
            self.result['gates']['cold-' + name] = value
        self.persist()
        start = time.monotonic()
        reply = self.call('ui.show', {'view': 'history'})
        require(reply.get('shown') == 'history', 'history_show_ack_mismatch')
        self.event('history-open-cli-ack', milliseconds=(time.monotonic() - start) * 1000,
                   provesPaint=False)
        deadline = time.monotonic() + 20
        while True:
            observed = self.sample('history-mapping')
            windows = [window for process in observed['processes'] if process['role'] == 'ui'
                       for window in owned_windows(process['pid'])]
            if windows:
                break
            require(time.monotonic() < deadline, 'history_owned_window_did_not_map')
            time.sleep(.05)
        self.event('history-owned-window-mapped', windows=windows,
                   milliseconds=(time.monotonic() - start) * 1000, provesPaint=False)
        self.visual('history-open')
        time.sleep(2)
        opened = self.interval('history-open', self.args.open_seconds)
        require(any(row['uiProcessCount'] for _, row in opened), 'history_ui_helper_missing')
        peak = max(row['totals']['pssBytes'] for _, row in opened)
        self.result['metrics']['historyOpenPeakFamilyPssBytes'] = peak
        self.result['gates']['historyOpenPss'] = {'status': 'pass' if peak <= 60 * MIB else 'fail',
                                               'limitBytes': 60 * MIB}
        for process in opened[-1][1]['processes']:
            for window in owned_windows(process['pid']):
                subprocess.run(['wmctrl', '-ic', window], check=True, timeout=5,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 20
        while self.sample('history-closing')['processCount'] != 1:
            require(time.monotonic() < deadline, 'history_helper_did_not_exit')
            time.sleep(.2)
        closed = self.interval('history-closed', 300, closed=True)
        self.visual('history-closed-completed')
        self.result['metrics']['historyClosed'] = memory_summary(closed)
        for name, value in memory_gates(self.result['metrics']['historyClosed']).items():
            self.result['gates']['closed-' + name] = value
        self.result['gates']['historyPaintResponse'] = {'status': 'unassessed',
                                                       'reason': 'external_paint_scenario_required'}


def read_line(process, seconds):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        require(selector.select(seconds), 'fixture_readiness_timeout')
        line = process.stdout.readline()
    require(len(line) <= 65536 and bool(line), 'fixture_closed_or_oversized_response')
    return json.loads(line)


def verify_fixture(args):
    require(loopback_endpoint(args.endpoint or ''), 'unsafe_fixture_endpoint')
    require(args.fixture_pid and args.fixture_source, 'explicit_fixture_identity_required')
    source = Path(args.fixture_source).resolve(strict=True)
    require(sha256(source) == sha256(REPO / 'tests/fixtures/s3_server.py'), 'fixture_source_hash_mismatch')
    process = Path('/proc') / str(args.fixture_pid)
    require(process.stat().st_uid == os.getuid(), 'fixture_uid_mismatch')
    command = (process / 'cmdline').read_bytes().split(b'\0')
    arguments = [item.decode() for item in command[1:] if item and not item.startswith(b'-')]
    require(arguments == [str(source)], 'fixture_process_source_mismatch')
    from urllib.parse import urlsplit
    port = urlsplit(args.endpoint).port
    inodes = {line.split()[9] for line in Path('/proc/net/tcp').read_text().splitlines()[1:]
              if line.split()[1].upper() == f'0100007F:{port:04X}' and line.split()[3] == '0A'}
    links = set()
    for fd in (process / 'fd').iterdir():
        try:
            links.add(os.readlink(fd))
        except FileNotFoundError:
            pass
    require(any('socket:[' + inode + ']' in links for inode in inodes), 'fixture_does_not_own_loopback_listener')


def operation_ids(database):
    """Read exact journal IDs; recent-activity caps/timestamps cannot hide work."""
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as connection:
        return {row[0] for row in connection.execute('SELECT id FROM operations')}


def platform_mappings(pid):
    """Actual mapped basenames only; a QCoreApplication may have no QPA plugin."""
    return sorted({Path(line.split()[-1]).name for line in Path(f'/proc/{pid}/maps').read_text().splitlines()
                   if '/platforms/' in line or 'libQt6XcbQpa' in line})


def memory_summary(rows):
    elapsed = rows[-1][0] - rows[0][0]
    first = next(row for row in rows[0][1]['processes'] if row['role'] == 'resident')
    last = next(row for row in rows[-1][1]['processes'] if row['role'] == 'resident')
    require(same_identity(first, last), 'resident_changed_during_cpu_interval')
    return {'seconds': elapsed, 'residentCpuPercentOneCore':
            100 * (last['cpuTicks'] - first['cpuTicks']) / os.sysconf('SC_CLK_TCK') / elapsed,
            'peakResidentRssBytes': max(next(p['rssBytes'] for p in r['processes']
                                          if p['role'] == 'resident') for _, r in rows),
            'peakFamilyRssBytes': max(r['totals']['rssBytes'] for _, r in rows),
            'peakFamilyPssBytes': max(r['totals']['pssBytes'] for _, r in rows)}


def memory_gates(metrics):
    return {name: {'status': 'pass' if passed else 'fail', **limit}
            for name, passed, limit in (
                ('cpu', metrics['seconds'] >= 300 and metrics['residentCpuPercentOneCore'] < .5,
                 {'minimumSeconds': 300, 'exclusiveLimitPercentOneCore': .5}),
                ('rss', metrics['peakFamilyRssBytes'] <= 40 * MIB, {'limitBytes': 40 * MIB}),
                ('pss', metrics['peakFamilyPssBytes'] <= 29 * MIB, {'limitBytes': 29 * MIB}))}


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    result.add_argument('scenario', choices=('capture', 'memory', 'seed', 'history', 'burst', 'slow'))
    for name in ('output', 'package', 'package-sha256', 'version', 'source-revision', 'desktop-session', 'lock-session'):
        result.add_argument('--' + name, required=True)
    result.add_argument('--app', default='/usr/bin/putgd')
    result.add_argument('--cli', default='/usr/bin/putgd-cli')
    result.add_argument('--helper', default='/usr/bin/putgd-ui')
    result.add_argument('--capture-helper', default='/usr/bin/putgd-capture')
    result.add_argument('--driver', help='Existing unchanged putgd-bench-capture executable')
    result.add_argument('--fixture-root', help='#495 stopped private independent fixture root')
    result.add_argument('--driver-revision', help='Full driver commit for git-archive exports; declaration plus file hashes retained')
    result.add_argument('--open-seconds', type=int, default=30)
    result.add_argument('--vnc-port', type=int, default=5900, help='Existing host loopback physical wall listener')
    result.add_argument('--endpoint', help='Literal http://127.0.0.1:PORT of an independently started fixture')
    result.add_argument('--fixture-pid', type=int)
    result.add_argument('--fixture-source', help='Exact script argv of the fixture process, same bytes as tests/fixtures/s3_server.py')
    result.add_argument('--enable-accessibility', action='store_true',
                        help='History/burst/slow only: set QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1; no desktop setting changes')
    result.add_argument('--native-driver', help='#495 committed native_acceptance.py for authenticated SNI ownership')
    result.add_argument('--native-driver-revision', help='Separately identified #495 full commit')
    result.add_argument('--threshold-policy', help='Private approved numerical maximums JSON; omission never passes response gates')
    result.add_argument('--settle-seconds', type=int, default=300)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    require(re.fullmatch('[0-9a-f]{64}', args.package_sha256) and
            re.fullmatch('[0-9a-f]{40}', args.source_revision), 'invalid_identity_digest')
    require(args.scenario != 'capture' or args.driver, 'capture_driver_required')
    require(1 <= args.open_seconds <= 300, 'invalid_history_open_duration')
    require(1 <= args.settle_seconds <= 1800, 'invalid_settle_duration')
    require(not args.enable_accessibility or args.scenario in ('history', 'burst', 'slow'),
            'accessibility_override_not_permitted_for_capture_or_memory')
    if args.scenario in ('burst', 'slow'):
        require(args.native_driver and args.native_driver_revision and
                re.fullmatch('[0-9a-f]{40}', args.native_driver_revision), '495_driver_and_revision_required')
    os.umask(0o077)
    def interrupted(_signal, _frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, interrupted)
    campaign = Campaign(args)
    try:
        campaign.initialize()
        campaign.configure()
        campaign.static_cover()
        campaign.start()
        capabilities = campaign.checked_live()
        if args.scenario == 'capture':
            campaign.capture(capabilities)
        elif args.scenario in ('history', 'burst', 'slow'):
            import campaign_workloads
            campaign_workloads.policy(args.threshold_policy)  # Reject malformed policy before actions.
            getattr(campaign_workloads, args.scenario)(campaign)
        else:
            getattr(campaign, args.scenario)()
        campaign.result['status'] = 'complete'
        return 1 if any(gate['status'] == 'fail' for gate in campaign.result['gates'].values()) else 0
    except BaseException as error:
        campaign.result['status'] = 'failed-or-interrupted'
        campaign.result['errorCode'] = str(error) if isinstance(error, EvidenceError) else type(error).__name__
        if campaign.cover:
            try:
                campaign.visual('failed-workload')
            except (OSError, ValueError, EvidenceError, subprocess.SubprocessError):
                campaign.result['failureVisualUnavailable'] = True
        raise
    finally:
        campaign.stop()
        campaign.close_fixtures()
        campaign.summarize_processes()
        campaign.result['elapsedSeconds'] = time.monotonic() - campaign.started
        campaign.persist()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, EvidenceError, sqlite3.Error, subprocess.SubprocessError):
        print('Campaign failed; retained private results.json/events.jsonl contain bounded diagnostics.', file=sys.stderr)
        raise SystemExit(2)
