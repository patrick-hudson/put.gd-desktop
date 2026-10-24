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
import shutil
import sqlite3
import stat
import subprocess
import sys
import tarfile
import time

from bench_ui_process import (EvidenceError, ProcessFamily, identity,
                              request, same_identity, sha256, verify_socket_owner,
                              write_json)
from observe_idle import check_physical_session, owned_windows, stop_owned_process_group

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
        kinds = dict(connection.execute('SELECT category,count(*) FROM history GROUP BY category'))
        require(kinds.get('image', 0) > 0 and kinds.get('video', 0) > 0,
                'fixture_requires_images_and_videos')
        return {'historyCount': count, 'independentOperations': operations,
                'independentItems': items, 'categories': kinds,
                'schemaVersion': connection.execute('PRAGMA user_version').fetchone()[0],
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
    require(not any((source / name).exists() for name in ('runtime/putgd/session.sock',)),
            'fixture_socket_present_stop_and_retire_endpoint_first')
    require(not Path(str(database) + '-wal').exists(), 'fixture_has_wal_checkpoint_and_stop_first')
    summary = fixture_summary(database)
    for directory in ('config', 'data', 'archive', 'cache'):
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
            'sessionId': args.desktop_session, 'qtPlatform': 'xcb',
            'osRelease': Path('/etc/os-release').read_text(),
            'loadAverage': Path('/proc/loadavg').read_text().split()[:3]}


class Campaign:
    def __init__(self, args):
        self.args = args
        raw = Path(args.output).absolute()
        require(not raw.is_symlink() and not raw.exists(), 'output_must_be_new')
        self.root = raw.resolve()
        self.root.mkdir(mode=0o700)
        self.started = time.monotonic()
        self.resident = self.family = self.log = None
        self.result = {'schemaVersion': 1, 'scenario': args.scenario, 'status': 'starting',
                       'startedUtc': datetime.now(timezone.utc).isoformat(),
                       'driver': {'sha256': sha256(__file__), 'revision': subprocess.check_output(
                           ['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()},
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
        if self.resident:
            stop_owned_process_group(self.resident)
        if self.log:
            self.log.close()
        self.resident = self.log = None

    def configure(self):
        self.start('offscreen')
        # Inspect actual production controls and accepted settings first.
        capabilities = self.call('capabilities')
        write_json(self.root / 'capabilities-setup.json', capabilities)
        self.call('settings.get')
        document = self.call('settings.developmentTemplate')['document']
        document['profiles'] = []
        document['actions'] = {name: ({'shortcut': ''} if name == 'recording' else
            {'destination': 'local', 'profileId': None,
             'shortcut': 'Ctrl+Shift+F12' if name == 'region' and self.args.scenario == 'capture' else ''})
            for name in document['actions']}
        document['archive']['root'] = str(self.root / 'archive')
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
        self.event('process-sample', label=label, **row)
        return row

    def interval(self, label, seconds, closed=False):
        rows = []
        started = None
        while True:
            check_lock(self.args)
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
        before = {row['id'] for row in self.call('operation.recent')['operations']}
        with (self.root / 'capture-driver.log').open('xb') as log:
            completed = subprocess.run(command, env=self.environment, stdout=log, stderr=log, timeout=240)
        data = json.loads((self.root / 'capture.json').read_text())
        for index, row in enumerate(data['samples']):
            self.event('capture-sample', index=index, **row)
        require(len(data['samples']) == 30 and data['warmupInvocations'] == 5 and not data['error'],
                'incomplete_capture_workload')
        operations = [row for row in self.call('operation.recent')['operations'] if row['id'] not in before]
        require(len({row['id'] for row in operations}) == 35, 'capture_operation_count_mismatch')
        outcomes = [self.call('operation.inspect', {'id': row['id']}) for row in operations]
        require(all(row.get('state') == 'cancelled' for row in outcomes), 'capture_did_not_cancel_cleanly')
        self.event('capture-outcomes', operations=[{'id': row['id'], 'state': row['state']} for row in outcomes])
        self.result['metrics']['warmP95DrawablePaintMs'] = data['p95PaintMs']
        self.result['gates']['warmCapture'] = {'status': 'pass' if data['p95PaintMs'] <= 150 else 'fail',
                                              'limitMs': 150, 'driverExit': completed.returncode}

    def memory(self):
        time.sleep(5)
        cold = self.interval('cold-closed', 300, closed=True)
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
        self.result['metrics']['historyClosed'] = memory_summary(closed)
        for name, value in memory_gates(self.result['metrics']['historyClosed']).items():
            self.result['gates']['closed-' + name] = value
        self.result['gates']['historyPaintResponse'] = {'status': 'unassessed',
                                                       'reason': 'external_paint_scenario_required'}


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
    result.add_argument('scenario', choices=('capture', 'memory'))
    for name in ('output', 'package', 'package-sha256', 'version', 'source-revision', 'desktop-session', 'lock-session'):
        result.add_argument('--' + name, required=True)
    result.add_argument('--app', default='/usr/bin/putgd')
    result.add_argument('--cli', default='/usr/bin/putgd-cli')
    result.add_argument('--helper', default='/usr/bin/putgd-ui')
    result.add_argument('--capture-helper', default='/usr/bin/putgd-capture')
    result.add_argument('--driver', help='Existing unchanged putgd-bench-capture executable')
    result.add_argument('--fixture-root', help='#495 stopped private independent fixture root')
    result.add_argument('--open-seconds', type=int, default=30)
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    require(re.fullmatch('[0-9a-f]{64}', args.package_sha256) and
            re.fullmatch('[0-9a-f]{40}', args.source_revision), 'invalid_identity_digest')
    require(args.scenario != 'capture' or args.driver, 'capture_driver_required')
    require(1 <= args.open_seconds <= 300, 'invalid_history_open_duration')
    os.umask(0o077)
    campaign = Campaign(args)
    try:
        campaign.initialize()
        campaign.configure()
        campaign.start()
        capabilities = campaign.checked_live()
        getattr(campaign, args.scenario)(capabilities) if args.scenario == 'capture' else campaign.memory()
        campaign.result['status'] = 'complete'
        return 1 if any(gate['status'] == 'fail' for gate in campaign.result['gates'].values()) else 0
    except BaseException as error:
        campaign.result['status'] = 'failed-or-interrupted'
        campaign.result['errorCode'] = str(error) if isinstance(error, EvidenceError) else type(error).__name__
        raise
    finally:
        campaign.stop()
        campaign.result['elapsedSeconds'] = time.monotonic() - campaign.started
        campaign.persist()


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, EvidenceError, sqlite3.Error, subprocess.SubprocessError):
        print('Campaign failed; retained private results.json/events.jsonl contain bounded diagnostics.', file=sys.stderr)
        raise SystemExit(2)
