#!/usr/bin/env python3
"""Total-process Linux memory evidence, with an optional physical X11 view workload.

Importing this module does not access a display, launch the app, or read content.
The --pid mode only observes an explicitly identified existing resident. Without
--pid, the command launches an isolated local-only fixture and closes its windows
through the window manager. It never acquires the clipboard or invokes uploads.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time


METRICS = ('rssBytes', 'pssBytes', 'privateBytes')
VIEWS = ('settings', 'history', 'activity', 'about')
MAX_FAMILY = 64
MAX_SAMPLES = 4096


class EvidenceError(RuntimeError):
    """A bounded diagnostic code; never a payload or raw process command line."""


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, indent=2)
            stream.write('\n')
            stream.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def stat_fields(text):
    # comm can contain spaces and closing parentheses; fields after its final ')'
    # have the stable Linux /proc/PID/stat ordering (state is field 3).
    fields = text.rsplit(') ', 1)[1].split()
    return {'state': fields[0], 'ppid': int(fields[1]),
            'cpuTicks': int(fields[11]) + int(fields[12]),
            'startTicks': int(fields[19])}


def identity(pid):
    root = Path('/proc') / str(pid)
    stat = stat_fields((root / 'stat').read_text())
    executable = root / 'exe'
    info = executable.stat()
    return {'pid': pid, 'startTicks': stat['startTicks'], 'uid': root.stat().st_uid,
            'executable': os.readlink(executable), 'device': info.st_dev, 'inode': info.st_ino}


def identity_key(value):
    return value['pid'], value['startTicks']


def same_identity(left, right):
    return all(left[key] == right[key] for key in
               ('pid', 'startTicks', 'uid', 'device', 'inode', 'executable'))


def children(pid):
    """A QProcess can be created by any thread, so inspect every task's children."""
    result = set()
    for task in (Path('/proc') / str(pid) / 'task').iterdir():
        try:
            result.update(int(value) for value in (task / 'children').read_text().split())
        except FileNotFoundError:
            pass  # A thread exited while enumerating the live family.
    return result


def memory(pid):
    values = {}
    root = Path('/proc') / str(pid)
    for line in (root / 'smaps_rollup').read_text().splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] == 'kB':
            values[fields[0].rstrip(':')] = int(fields[1]) * 1024
    if not all(key in values for key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty')):
        raise EvidenceError('incomplete_smaps_rollup')
    status = (root / 'status').read_text()
    stat = stat_fields((root / 'stat').read_text())
    return {'rssBytes': values['Rss'], 'pssBytes': values['Pss'],
            'privateBytes': values['Private_Clean'] + values['Private_Dirty'],
            'privateHugetlbBytes': values.get('Private_Hugetlb', 0),
            'swapBytes': values.get('Swap', 0), 'cpuTicks': stat['cpuTicks'],
            'threads': int(re.search(r'^Threads:\s+(\d+)', status, re.M)[1])}


class ProcessFamily:
    """Track one resident and ALL observed descendants, including escaped children.

    A helper that reparents is kept in the ledger until exit, not dropped from the
    total. Every sample validates start time, UID, executable inode and hash. A
    changed process/exec invalidates the sample instead of looking like a saving.
    Snapshots are sequential, not stop-the-world; retries only cover process exits.
    """

    def __init__(self, pid, app, helper=None, start_ticks=None):
        self.root = identity(pid)
        if self.root['uid'] != os.getuid():
            raise EvidenceError('resident_uid_mismatch')
        if start_ticks is not None and self.root['startTicks'] != start_ticks:
            raise EvidenceError('resident_start_time_mismatch')
        self.app = Path(app).resolve(strict=True)
        self.helper = Path(helper).resolve(strict=True) if helper else None
        if Path(self.root['executable']) != self.app:
            raise EvidenceError('resident_executable_mismatch')
        self.expected_hashes = {str(self.app): sha256(self.app)}
        if self.helper:
            self.expected_hashes[str(self.helper)] = sha256(self.helper)
        self.known = {}
        self.exited = []
        self._remember(self.root)

    def _remember(self, item):
        if item['uid'] != self.root['uid']:
            raise EvidenceError('descendant_uid_mismatch')
        live_exe = Path('/proc') / str(item['pid']) / 'exe'
        digest = sha256(live_exe)
        expected = self.expected_hashes.get(item['executable'])
        if expected is not None and digest != expected:
            raise EvidenceError('descendant_binary_hash_mismatch')
        item = {**item, 'sha256': digest,
                'role': ('resident' if identity_key(item) == identity_key(self.root)
                         else 'ui' if self.helper and Path(item['executable']) == self.helper
                         else 'auxiliary')}
        self.known[identity_key(item)] = item
        return item

    def _discover(self):
        current = identity(self.root['pid'])
        if not same_identity(current, self.root):
            raise EvidenceError('resident_identity_changed')
        seen = set()

        def walk(seeds):
            queue, reached = list(seeds), set()
            while queue:
                pid, expected = queue.pop()
                try:
                    item = identity(pid)
                    key = identity_key(item)
                    # A tracked PID may now belong to an unrelated process. Do
                    # not traverse it merely because its old identity is known.
                    if expected is not None and key != identity_key(expected):
                        if identity_key(expected) == identity_key(self.root):
                            raise EvidenceError('resident_identity_changed')
                        continue
                    if key in self.known and not same_identity(item, self.known[key]):
                        raise EvidenceError('descendant_executable_changed')
                    if key in seen:
                        continue
                    if key not in self.known:
                        self._remember(item)
                    descendants = children(pid)
                    # Validate again before adopting the children: the tracked
                    # parent could have exited/reused its PID during enumeration.
                    after = identity(pid)
                    if identity_key(after) != key:
                        if key == identity_key(self.root):
                            raise EvidenceError('resident_identity_changed')
                        continue
                    if not same_identity(after, item):
                        raise EvidenceError('descendant_executable_changed')
                    seen.add(key)
                    reached.add(key)
                    if len(seen) > MAX_FAMILY:
                        raise EvidenceError('process_family_limit')
                    queue.extend((child, None) for child in descendants)
                except FileNotFoundError:
                    if pid == self.root['pid']:
                        raise EvidenceError('resident_exited')
            return reached

        tree = walk([(self.root['pid'], self.root)])
        # Escaped descendants remain part of the accounting family, including
        # workers they create later, but never become current resident-tree roots.
        walk((item['pid'], item) for item in list(self.known.values()))
        return tree

    def sample(self):
        started = time.monotonic()
        for attempt in range(3):
            tree = self._discover()
            rows, disappeared = [], False
            for key, expected in list(self.known.items()):
                try:
                    current = identity(expected['pid'])
                    if identity_key(current) != key:
                        if key == identity_key(self.root):
                            raise EvidenceError('resident_identity_changed')
                        raise FileNotFoundError()  # Old child exited and its PID was reused.
                    if not same_identity(current, expected):
                        raise EvidenceError('descendant_executable_changed')
                    measured = memory(expected['pid'])
                    if not same_identity(identity(expected['pid']), expected):
                        raise EvidenceError('process_changed_during_sample')
                    # Device/inode guards fast samples; hash again prevents an
                    # executable modified in place from passing an old identity.
                    if sha256(Path('/proc') / str(expected['pid']) / 'exe') != expected['sha256']:
                        raise EvidenceError('live_binary_hash_changed')
                    rows.append({**expected, **measured, 'inResidentTree': key in tree})
                except FileNotFoundError:
                    if key == identity_key(self.root):
                        raise EvidenceError('resident_exited')
                    self.exited.append(expected)
                    del self.known[key]
                    disappeared = True
                    if len(self.exited) > 256:
                        raise EvidenceError('helper_generation_limit')
            if len(rows) > MAX_FAMILY:
                raise EvidenceError('process_family_limit')
            after = self._discover()
            if not disappeared and tree == after and len(rows) == len(self.known):
                return {'sampleSeconds': time.monotonic() - started,
                        'processes': sorted(rows, key=lambda item: item['pid']),
                        'totals': {name: sum(row[name] for row in rows) for name in METRICS},
                        'processCount': len(rows),
                        'uiProcessCount': sum(row['role'] == 'ui' for row in rows),
                        'escapedProcessCount': sum(not row['inResidentTree'] for row in rows)}
        raise EvidenceError('process_family_did_not_settle')


def selected_windows(pids, owner_lookup):
    """Union only explicitly verified owners; never export titles or enumerate text."""
    return sorted({window for pid in sorted(set(pids)) for window in owner_lookup(pid)})


def version(path):
    completed = subprocess.run([str(path), '--version'], capture_output=True, text=True,
                               timeout=10, check=True)
    text = completed.stdout.strip()
    if len(text) > 512 or '\n' in text or not re.fullmatch(r'putgd(?:-cli|-ui)? [0-9][A-Za-z0-9.+~-]*', text):
        raise EvidenceError('unexpected_binary_version')
    return text.split(' ', 1)[1]


def system_metadata():
    result = {'kernel': os.uname().release, 'machine': os.uname().machine,
              'python': sys.version.split()[0],
              'desktop': os.environ.get('XDG_CURRENT_DESKTOP', ''),
              'session': os.environ.get('XDG_SESSION_TYPE', ''), 'packages': []}
    packages = ['libqt6core6', 'libqt6core6t64', 'libqt6widgets6', 'libqt6widgets6t64',
                'libc6', 'cinnamon', 'muffin', 'gnome-shell', 'kwin-wayland']
    try:
        query = subprocess.run(['dpkg-query', '-W', '-f=${Package} ${Version}\n', *packages],
                               capture_output=True, text=True, timeout=10)
    except FileNotFoundError:
        try:
            query = subprocess.run(['rpm', '-q', 'qt6-qtbase', 'glibc', 'gnome-shell', 'kwin'],
                                   capture_output=True, text=True, timeout=10)
        except FileNotFoundError:
            query = None
    result['packages'] = query.stdout.splitlines() if query else []
    result['cpu'] = next((line.split(':', 1)[1].strip()
                          for line in Path('/proc/cpuinfo').read_text().splitlines()
                          if line.startswith('model name')), 'unavailable')
    result['logicalCpus'] = os.cpu_count()
    try:
        result['virtualization'] = subprocess.run(['systemd-detect-virt'], capture_output=True,
                                                   text=True, timeout=5).stdout.strip()
    except FileNotFoundError:
        result['virtualization'] = 'unavailable'
    return result


class Recorder:
    def __init__(self, args):
        self.root = Path(args.output).resolve()
        self.root.mkdir(mode=0o700, exist_ok=False)
        self.started = time.monotonic()
        self.data = {'schemaVersion': 1, 'startedUtc': datetime.now(timezone.utc).isoformat(),
                     'command': [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
                     'workingDirectory': str(Path.cwd()), 'mode': args.mode,
                     'scope': 'existing-process observation' if args.pid else 'isolated empty-view lifecycle',
                     'parameters': vars(args), 'harnessSha256': sha256(__file__),
                     'sourceRevision': args.source_revision,
                     'binaries': {}, 'sampleCount': 0, 'sampleFile': 'samples.jsonl',
                     'samples': [], 'events': [], 'status': 'starting', 'passed': False,
                     'acceptanceReceipts': [],
                     'limitations': ['Summed RSS double-counts pages shared between processes; compare total PSS and private memory as well.',
                                     'Shared desktop services outside the resident descendant tree are not charged as separate processes.',
                                     'Sequential /proc snapshots are not atomic; a descendant born and exited between samples can be missed.',
                                     'This is a sampled memory diagnostic, not a leak proof, rendered tray proof, CPU budget or cold-idle acceptance.']}
        self.persist()

    def initialize(self, args):
        self.data['environment'] = system_metadata()
        for label, name in [('app', args.app), ('cli', args.cli), ('helper', args.helper)]:
            if name:
                path = Path(name).resolve(strict=True)
                observed_version = version(path)
                if '+g' + args.source_revision[:12] not in observed_version:
                    raise EvidenceError('binary_source_revision_mismatch')
                if observed_version.endswith('.dirty') and not args.allow_dirty:
                    raise EvidenceError('dirty_build_requires_diagnostic_override')
                self.data['binaries'][label] = {'path': str(path), 'sha256': sha256(path),
                                               'version': observed_version}
        if len({value['version'] for value in self.data['binaries'].values()}) != 1:
            raise EvidenceError('binary_versions_differ')
        if args.source_archive:
            archive = Path(args.source_archive).resolve(strict=True)
            self.data['sourceArchive'] = {'path': str(archive), 'sha256': sha256(archive)}
        for name in args.acceptance_receipt:
            path = Path(name).resolve(strict=True)
            self.data['acceptanceReceipts'].append({'path': str(path), 'sha256': sha256(path)})
        self.persist()

    def persist(self, final=False):
        value = self.data if final else {**self.data, 'samples': self.data['samples'][-1:],
                                          'progressIncludesLatestSampleOnly': True}
        write_json(self.root / ('results.json' if final else 'progress.json'), value)

    def event(self, kind, **values):
        self.data['events'].append({'elapsedSeconds': time.monotonic() - self.started,
                                    'kind': kind, **values})
        self.persist()

    def sample(self, family, label):
        if self.data['sampleCount'] >= MAX_SAMPLES:
            raise EvidenceError('sample_count_limit')
        row = {'label': label, 'elapsedSeconds': time.monotonic() - self.started, **family.sample()}
        path = self.root / 'samples.jsonl'
        with path.open('a') as stream:
            os.chmod(path, 0o600)
            stream.write(json.dumps(row, separators=(',', ':')) + '\n')
        self.data['sampleCount'] += 1
        self.data['samples'].append({key: row[key] for key in
                                     ('label', 'elapsedSeconds', 'totals', 'processCount',
                                      'uiProcessCount', 'escapedProcessCount', 'sampleSeconds')})
        self.persist()
        print(json.dumps(self.data['samples'][-1]), flush=True)
        return row

    def finish(self, family, passed):
        self.data['passed'] = passed
        self.data['status'] = 'complete' if passed else 'failed-or-interrupted'
        self.data['exitedProcesses'] = family.exited if family else []
        rows = self.data['samples']
        if rows:
            self.data['summary'] = {'finalTotals': rows[-1]['totals'],
                                    'peakSampledTotals': {metric: max(row['totals'][metric] for row in rows)
                                                         for metric in METRICS},
                                    'finalTotalRssAtOrBelow40MiB': rows[-1]['totals']['rssBytes'] <= 40 * 1024**2,
                                    'coldIdleBudgetEstablished': False,
                                    'cold40MiBTargetEstablished': False,
                                    'renderedTrayAcceptanceEstablished': False}
            opened = [row for row in rows if '-open-' in row['label']]
            closed = [row for row in rows if '-closed-' in row['label']]
            self.data['summary']['peakOpenTotals'] = (
                {metric: max(row['totals'][metric] for row in opened) for metric in METRICS}
                if opened else None)
            self.data['summary']['finalClosedTotals'] = closed[-1]['totals'] if closed else None
            latency = [event['acknowledgementMilliseconds'] for event in self.data['events']
                       if event['kind'] == 'view-shown']
            self.data['summary']['medianActivationAcknowledgementMilliseconds'] = (
                statistics.median(latency) if latency else None)
        self.data['elapsedSeconds'] = time.monotonic() - self.started
        self.persist(final=True)


def request(cli, method, params=None, *, environment=None, socket=None):
    command = [str(cli)]
    if socket:
        command += ['--socket', socket]
    command += [method, json.dumps(params or {}, separators=(',', ':'))]
    completed = subprocess.run(command, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, timeout=25)
    if len(completed.stdout) > 16 * 1024 * 1024:
        raise EvidenceError('cli_response_limit')
    reply = json.loads(completed.stdout)
    if completed.returncode or reply.get('ok') is not True:
        code = reply.get('error', {}).get('code', '')
        raise EvidenceError('cli_' + code if re.fullmatch('[a-z_]{1,80}', code) else 'cli_failed')
    return reply['result']


def verify_socket_owner(path, family):
    # Correlate --pid with the CLI endpoint before recording any diagnostics.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(3)
        connection.connect(path)
        pid, uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
    if pid != family.root['pid'] or uid != family.root['uid']:
        raise EvidenceError('session_socket_owner_mismatch')
    if not same_identity(identity(pid), family.root):
        raise EvidenceError('resident_identity_changed')


def checked_diagnostics(args, call, recorder):
    if args.mode == 'split':
        snapshot = call('ui.snapshot')
        result = snapshot['diagnostics']
        build = snapshot['build']
    else:
        result = call('diagnostics')
        build = result['build']
    if build.get('gitCommit') != args.source_revision:
        raise EvidenceError('resident_source_revision_mismatch')
    if build.get('version') != recorder.data['binaries']['app']['version']:
        raise EvidenceError('resident_version_mismatch')
    recorder.data['residentBuild'] = build
    if args.mode == 'split' and result.get('trayAvailable') is not True:
        raise EvidenceError('resident_tray_unavailable')
    return result


def checked_helper(args, family, diagnostics, *, closed=False):
    measured = family.sample()
    if measured['escapedProcessCount']:
        raise EvidenceError('descendant_escaped_resident_tree')
    ui_rows = [row for row in measured['processes'] if row['role'] == 'ui']
    if args.mode == 'split':
        ui = diagnostics.get('ui', {})
        if closed:
            if ui.get('pid', 0) != 0 or ui_rows:
                raise EvidenceError('closed_ui_process_still_present')
        elif (len(ui_rows) != 1 or ui.get('pid') != ui_rows[0]['pid']
              or ui.get('state') != 'ready'):
            raise EvidenceError('ui_diagnostics_process_mismatch')
    elif ui_rows:
        raise EvidenceError('unexpected_ui_helper_in_retained_mode')
    return measured


def wait_until(predicate, seconds, code):
    deadline = time.monotonic() + seconds
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise EvidenceError(code)
        time.sleep(.1)


def observe_interval(recorder, family, label, duration, interval, invariant=None):
    deadline = time.monotonic() + duration
    index = 0
    while True:
        if invariant:
            invariant()
        recorder.sample(family, f'{label}-{index}')
        index += 1
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(interval, remaining))


def observe_existing(args, recorder):
    family = None
    passed = False
    try:
        family = ProcessFamily(args.pid, args.app, args.helper, args.start_ticks)
        recorder.data['residentIdentity'] = family.root
        if args.cli:
            verify_socket_owner(args.socket, family)
            call = lambda method, params=None: request(args.cli, method, params, socket=args.socket)
            diagnostics = checked_diagnostics(args, call, recorder)
            recorder.event('diagnostics-identity-checked', ui=diagnostics.get('ui'),
                           trayAvailable=diagnostics.get('trayAvailable'))
        observe_interval(recorder, family, args.label, args.seconds, args.interval)
        passed = True
        return 0
    except BaseException as error:
        recorder.data['errorCode'] = str(error) if isinstance(error, EvidenceError) else type(error).__name__
        raise
    finally:
        recorder.finish(family, passed)


def exercise_views(args, recorder):
    # Dependencies are only needed for this opt-in physical desktop workload.
    from bench_views import fixture_config
    from observe_idle import check_physical_session, operation_count, owned_windows, stop_owned_process_group
    family = resident = None
    passed = False
    log = None
    try:
        check_physical_session()
        relative, document, fixture = fixture_config(Path(args.fixture).resolve(strict=True))
        recorder.data['fixture'] = fixture
        recorder.data['dependencies'] = {name: sha256(Path(__file__).with_name(name))
                                          for name in ('bench_views.py', 'observe_idle.py')}
        environment = os.environ.copy()
        for key, name in [('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                          ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')]:
            directory = recorder.root / name
            directory.mkdir(mode=0o700)
            environment[key] = str(directory)
        environment['QT_QPA_PLATFORM'] = 'xcb'
        config = recorder.root / 'config' / relative
        config.parent.mkdir(parents=True, mode=0o700)
        document['archive']['root'] = str(recorder.root / 'archive')
        write_json(config, document)
        log = (recorder.root / 'resident.log').open('wb')
        os.chmod(recorder.root / 'resident.log', 0o600)
        resident = subprocess.Popen([str(Path(args.app).resolve()), '--background'],
                                    env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True)
        family = ProcessFamily(resident.pid, args.app, args.helper)
        recorder.data['residentIdentity'] = family.root
        call = lambda method, params=None: request(args.cli, method, params, environment=environment)

        def ready():
            if resident.poll() is not None:
                raise EvidenceError('resident_exited_before_readiness')
            try:
                return call('diagnostics').get('journalReady') is True
            except (EvidenceError, subprocess.SubprocessError, json.JSONDecodeError):
                return False

        wait_until(ready, args.timeout, 'resident_readiness_timeout')
        checked_diagnostics(args, call, recorder)

        def windows():
            observed = family.sample()
            return selected_windows([row['pid'] for row in observed['processes']], owned_windows)

        def closed():
            if windows():
                raise EvidenceError('closed_stage_has_owned_windows')
            checked_helper(args, family, checked_diagnostics(args, call, recorder), closed=True)
            if operation_count(recorder.root):
                raise EvidenceError('unexpected_operation')

        closed()
        observe_interval(recorder, family, 'startup-warmup', args.warmup_seconds, args.interval, closed)
        recorder.sample(family, 'cold-closed-after-warmup')
        for cycle in range(1, args.cycles + 1):
            for count, view in enumerate(args.views, 1):
                start = time.monotonic()
                reply = call('ui.show', {'view': view})
                acknowledged = time.monotonic()
                if reply.get('shown') != view:
                    raise EvidenceError('ui_show_ack_mismatch')
                native = wait_until(lambda: (items if len(items := windows()) == count else None),
                                    args.timeout, 'owned_window_readiness_timeout')
                diagnostics = checked_diagnostics(args, call, recorder)
                checked_helper(args, family, diagnostics)
                recorder.event('view-shown', cycle=cycle, view=view,
                               acknowledgementMilliseconds=(acknowledged - start) * 1000,
                               mappedReadyMilliseconds=(time.monotonic() - start) * 1000,
                               mappedOwnedWindows=native,
                               acknowledgementIsShown=args.mode == 'split', ui=diagnostics.get('ui'))
                # Readiness is established above. This interval measures an already
                # ready view; its duration is not evidence of successful activation.
                def opened():
                    if len(windows()) != count:
                        raise EvidenceError('open_stage_window_count_changed')
                    checked_helper(args, family, checked_diagnostics(args, call, recorder))

                observe_interval(recorder, family, f'cycle-{cycle}-{view}-open',
                                 args.visible_seconds, args.interval, opened)
            for window in windows():
                if window in windows():
                    subprocess.run(['wmctrl', '-ic', window], check=True, timeout=5,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            close_start = time.monotonic()

            def all_closed():
                if windows():
                    return False
                diagnostics = checked_diagnostics(args, call, recorder)
                if args.mode == 'split':
                    if diagnostics.get('ui', {}).get('pid', 0) != 0 or family.sample()['uiProcessCount']:
                        return False
                return True

            wait_until(all_closed, args.timeout, 'last_window_close_did_not_release_helper')
            closed()
            recorder.event('all-views-closed', cycle=cycle,
                           closeMilliseconds=(time.monotonic() - close_start) * 1000,
                           helperAbsent=family.sample()['uiProcessCount'] == 0,
                           helperExitRequired=args.mode == 'split', residentAlive=True,
                           closeMethod='EWMH _NET_CLOSE_WINDOW through wmctrl; no process signal')
            observe_interval(recorder, family, f'cycle-{cycle}-closed', args.quiet_seconds,
                             args.interval, closed)
        recorder.data['operationCount'] = operation_count(recorder.root)
        recorder.data['completeViewCycles'] = args.cycles
        recorder.data['allViews'] = args.views
        passed = True
        return 0
    except BaseException as error:
        recorder.data['errorCode'] = str(error) if isinstance(error, EvidenceError) else type(error).__name__
        raise
    finally:
        recorder.finish(family, passed)
        # This final cleanup is outside the measured lifecycle; it never counts as
        # successful last-window closure. Only our launched process group is owned.
        if resident is not None:
            stop_owned_process_group(resident)
        if log:
            log.close()


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--mode', choices=('retained', 'split'), required=True)
    result.add_argument('--app', required=True)
    result.add_argument('--cli')
    result.add_argument('--helper', help='Exact installed putgd-ui executable, required for split mode')
    result.add_argument('--source-revision', required=True, help='Full compiled gitCommit, not an inferred checkout HEAD')
    result.add_argument('--source-archive', help='Hash the exact clean source archive used for this build, if available')
    result.add_argument('--output', required=True, help='New private output directory; never reused')
    result.add_argument('--allow-dirty', action='store_true', help='Diagnostic only; do not score as a frozen-source comparison')
    result.add_argument('--acceptance-receipt', action='append', default=[], help='Hash an already completed native acceptance receipt')
    result.add_argument('--pid', type=int, help='Observe an existing resident without launching or interacting with any GUI')
    result.add_argument('--start-ticks', type=int, help='Required with --pid; expected field 22 from /proc/PID/stat')
    result.add_argument('--socket', help='Explicit resident session socket, only with --pid and --cli')
    result.add_argument('--label', default='existing-process')
    result.add_argument('--seconds', type=float, default=0, help='Existing-process observation duration; zero takes one sample')
    result.add_argument('--interval', type=float, default=5)
    result.add_argument('--fixture', help='Successful >=300-second private empty physical idle fixture')
    result.add_argument('--cycles', type=int, default=3)
    result.add_argument('--views', nargs='+', choices=VIEWS, default=None)
    result.add_argument('--warmup-seconds', type=float, default=5)
    result.add_argument('--visible-seconds', type=float, default=2)
    result.add_argument('--quiet-seconds', type=float, default=30)
    result.add_argument('--timeout', type=float, default=20)
    return result


def main():
    arguments = parser()
    args = arguments.parse_args()
    if not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', args.source_revision):
        arguments.error('--source-revision must be the full compiled commit')
    if args.mode == 'split' and not args.helper:
        arguments.error('--helper is required for split accounting')
    if args.mode == 'retained' and args.helper:
        arguments.error('retained mode has no helper')
    if args.pid is not None and (args.pid < 1 or args.start_ticks is None or args.start_ticks < 1):
        arguments.error('--pid requires its positive --start-ticks identity')
    if args.pid is None and (not args.cli or not args.fixture or args.socket or args.start_ticks is not None):
        arguments.error('view workload requires --cli and --fixture; --socket/--start-ticks are for existing processes')
    if args.socket and not args.cli:
        arguments.error('--socket requires --cli')
    if args.pid is not None and args.cli and not args.socket:
        arguments.error('--pid with --cli requires an explicit --socket for peer-PID validation')
    args.views = args.views or list(VIEWS if args.mode == 'split' else VIEWS[:3])
    if len(set(args.views)) != len(args.views):
        arguments.error('--views must not contain duplicates')
    if args.mode == 'retained' and 'about' in args.views:
        arguments.error('baseline bcbe63f has no ui.show about contract; compare matched three-view workloads')
    if not (2 <= args.cycles <= 20 and 0 <= args.seconds <= 3600 and .1 <= args.interval <= 60
            and 0 <= args.warmup_seconds <= 600 and 0 <= args.visible_seconds <= 60
            and 0 <= args.quiet_seconds <= 600 and 1 <= args.timeout <= 60):
        arguments.error('parameters exceed the bounded measurement ranges')
    expected_samples = (math.ceil(args.seconds / args.interval) + 1 if args.pid
                        else math.ceil(args.warmup_seconds / args.interval) + 2
                        + args.cycles * (len(args.views) * (math.ceil(args.visible_seconds / args.interval) + 1)
                                         + math.ceil(args.quiet_seconds / args.interval) + 1))
    if expected_samples > MAX_SAMPLES:
        arguments.error('parameters exceed the 4096-sample evidence bound')
    recorder = None
    try:
        recorder = Recorder(args)
        recorder.initialize(args)
        return observe_existing(args, recorder) if args.pid else exercise_views(args, recorder)
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        if recorder and not (recorder.root / 'results.json').exists():
            recorder.data['errorCode'] = str(error) if isinstance(error, EvidenceError) else type(error).__name__
            recorder.finish(None, False)
        print('UI process measurement failed; inspect its private evidence. No memory result was accepted.', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
