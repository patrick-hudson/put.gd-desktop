#!/usr/bin/env python3
"""Observe the whole resident family beside the unchanged physical cold workload.

Help/import performs no display query or process launch. This command is a
supplemental sampled observer, not compositor, leak, or rendered-tray proof.
"""

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from bench_ui_process import (EvidenceError, ProcessFamily, children, identity,
                              identity_key, same_identity, sha256, version, write_json)


REPO_ROOT = Path(__file__).resolve().parents[2]
PERFORMANCE = Path(__file__).resolve().parent
POLL_SECONDS = 1
LIMIT_BYTES = 40 * 1024**2
MAX_GAP_SECONDS = 3 * POLL_SECONDS
MAX_OWNED_GENERATIONS = 512


def same_generation(left, right):
    """Ownership survives exec but never PID reuse or a changed UID."""
    return all(left[key] == right[key] for key in ('pid', 'startTicks', 'uid'))


def require_pidfd_support():
    if not callable(getattr(os, 'pidfd_open', None)) or not callable(getattr(signal, 'pidfd_send_signal', None)):
        raise EvidenceError('pidfd_cleanup_unavailable')
    descriptor = os.pidfd_open(os.getpid())
    os.close(descriptor)


def signal_generation(expected, sig):
    """Bind the signal before verifying generation; never signal a bare PID."""
    if not callable(getattr(os, 'pidfd_open', None)) or not callable(getattr(signal, 'pidfd_send_signal', None)):
        raise EvidenceError('pidfd_cleanup_unavailable')
    try:
        descriptor = os.pidfd_open(expected['pid'])
    except ProcessLookupError:
        return False
    try:
        try:
            if not same_generation(identity(expected['pid']), expected):
                return False
            signal.pidfd_send_signal(descriptor, sig)
            return True
        except (ProcessLookupError, FileNotFoundError):
            return False
    finally:
        os.close(descriptor)


class OwnedSubtree:
    """Track owned generations and measured-resident ancestry across exec/reparent."""

    def __init__(self, process):
        self.process = process
        self.root = identity(process.pid)
        self.known = {identity_key(self.root): self.root}
        self.parents = {}
        self.exec_transitions = []
        self.resident_keys = set()
        self.resident_root = None
        self.resident_descendants = {}
        self.cleanup_discovery_errors = []
        self.cleanup_discovery_error_count = 0

    def current(self, expected, record=True):
        current = identity(expected['pid'])
        if not same_generation(current, expected):
            return None
        if not same_identity(current, expected):
            if record:
                if len(self.exec_transitions) >= MAX_OWNED_GENERATIONS:
                    raise EvidenceError('owned_exec_transition_limit')
                self.exec_transitions.append({'before': expected, 'after': current})
            self.known[identity_key(current)] = current
        return current

    def mark_resident(self, root):
        self.resident_root = root
        self.resident_keys = {identity_key(root)}
        # Preserve already observed descendants still alive when the primary
        # measured resident is identified, even if they have already reparented.
        self.mark_descendants({identity_key(row) for row in self.live()})

    def mark_descendants(self, eligible):
        changed = True
        while changed:
            changed = False
            for key, parents in self.parents.items():
                if key in self.resident_keys:
                    continue
                if parents & self.resident_keys:
                    self.resident_keys.add(key)
                    changed = True
        # Ancestry traverses dead intermediaries, while only live-at-identification
        # or actually observed post-identification generations affect the gate.
        for key in eligible & self.resident_keys - {identity_key(self.resident_root)}:
            self.resident_descendants.setdefault(key, self.known[key])

    def discover(self):
        queue = list(self.known.values())
        seen = set()
        observed = set()
        while queue:
            expected = queue.pop()
            key = identity_key(expected)
            if key in seen:
                continue
            seen.add(key)
            try:
                current = self.current(expected)
                if current is None:
                    continue
                observed.add(key)
                descendants = children(current['pid'])
                if not same_generation(identity(current['pid']), current):
                    continue
                for pid in descendants:
                    try:
                        child = identity(pid)
                    except FileNotFoundError:
                        continue
                    if child['uid'] != os.getuid():
                        raise EvidenceError('observer_descendant_uid_mismatch')
                    child_key = identity_key(child)
                    observed.add(child_key)
                    if child_key not in self.known:
                        if len(self.known) >= MAX_OWNED_GENERATIONS:
                            raise EvidenceError('owned_generation_limit')
                        self.known[child_key] = child
                    self.parents.setdefault(child_key, set()).add(key)
                    queue.append(self.known[child_key])
            except FileNotFoundError:
                continue
        if self.resident_root:
            self.mark_descendants(observed)

    def live(self, record=True):
        result = []
        for expected in list(self.known.values()):
            try:
                current = self.current(expected, record=record)
                if current:
                    result.append(current)
            except FileNotFoundError:
                continue
        return result

    def cleanup_discover(self):
        try:
            self.discover()
        except Exception as error:
            self.cleanup_discovery_error_count += 1
            if len(self.cleanup_discovery_errors) < 16:
                self.cleanup_discovery_errors.append({
                    'errorType': type(error).__name__,
                    'errorCode': str(error) if isinstance(error, EvidenceError) else None})

    def stop(self):
        # Discovery/cap failures must not prevent signaling already known owned
        # generations. Keep failures bounded and report them after safe cleanup.
        self.cleanup_discover()
        if self.process.poll() is None:
            signal_generation(self.root, signal.SIGINT)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.cleanup_discover()
            if not self.live(record=False):
                break
            time.sleep(.1)
        for sig in (signal.SIGTERM, signal.SIGKILL):
            self.cleanup_discover()
            for expected in reversed(self.live(record=False)):
                signal_generation(expected, sig)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and self.live(record=False):
                time.sleep(.1)
        self.process.wait(timeout=5)
        if self.live(record=False):
            raise EvidenceError('owned_observer_cleanup_incomplete')
        if self.cleanup_discovery_error_count:
            raise EvidenceError('cleanup_discovery_failed')


def sample_coverage(samples, primary_seconds):
    span = samples[-1]['elapsedSeconds'] - samples[0]['elapsedSeconds'] if samples else 0
    gaps = [right['elapsedSeconds'] - left['elapsedSeconds']
            for left, right in zip(samples, samples[1:])]
    required_span = max(0, primary_seconds - 2 * POLL_SECONDS)
    minimum_count = math.ceil(required_span / MAX_GAP_SECONDS) + 1
    maximum_gap = max(gaps, default=0)
    return {'sampleSpanSeconds': span, 'maximumSampleGapSeconds': maximum_gap,
            'maximumAllowedGapSeconds': MAX_GAP_SECONDS,
            'minimumSampleCount': minimum_count,
            'adequate': (len(samples) >= minimum_count and span >= required_span
                         and maximum_gap <= MAX_GAP_SECONDS)}


def single_cold_process(samples, family, owned):
    return (bool(samples) and all(row['processCount'] == 1 for row in samples)
            and not owned.resident_descendants
            and not any(identity_key(row) != identity_key(family.root)
                        for row in family.exited))


def sample_cold_family(family, output):
    """Recognize final root exit without hiding missing children or proc data.

    The frozen sampler's initial root lookup can raise raw FileNotFoundError.
    This supplemental boundary alone normalizes that terminal race. Complete
    sidecar, observer exit, coverage and unchanged-hash checks still follow.
    """
    try:
        return family.sample()
    except (FileNotFoundError, EvidenceError) as error:
        root = Path('/proc') / str(family.root['pid'])
        if isinstance(error, EvidenceError):
            if str(error) != 'resident_exited':
                raise
        elif error.filename not in {str(root), str(root / 'stat'), str(root / 'exe')}:
            raise
        final = output / 'results.json'
        if not final.is_file():
            raise
        try:
            current = identity(family.root['pid'])
        except FileNotFoundError:
            # An unreadable executable alone can be a live/zombie process.
            # Require the entire original proc entry to have disappeared.
            if root.exists():
                raise EvidenceError('resident_exit_not_confirmed') from error
        else:
            if not same_identity(current, family.root):
                raise EvidenceError('resident_identity_changed') from error
            raise
        try:
            primary = read_json(final)
            valid = (primary['budgetPassed'] is True and primary['elapsedSeconds'] >= 300
                     and primary['appSha256'] == family.expected_hashes[str(family.app)])
        except (OSError, ValueError, KeyError, TypeError):
            raise EvidenceError('invalid_final_primary_at_resident_exit') from error
        if not valid:
            raise EvidenceError('invalid_final_primary_at_resident_exit') from error
        if root.exists():
            raise EvidenceError('resident_identity_changed') from error
        return None


def read_json(path):
    with path.open() as source:
        return json.load(source)


def executable(value):
    path = Path(value).resolve(strict=True)
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ValueError('binary_must_be_executable_regular_file')
    return path


def find_family(output, sidecar, owned, app, helper):
    # bench_idle first starts an offscreen configuration bootstrap. Waiting for
    # its primary progress excludes that process from the measured cold family.
    if not (output / 'progress.json').is_file() or not sidecar.is_file():
        return None
    primary = read_json(output / 'progress.json')
    if not primary.get('samples'):
        return None
    observed = read_json(sidecar)
    owned.discover()
    candidates = []
    live = {row['pid']: row for row in owned.live()}
    for row in observed['observations']:
        expected = live.get(row['residentPid'])
        if expected and Path(expected['executable']) == app:
            candidates.append(expected)
    unique = {identity_key(row): row for row in candidates}
    if len(unique) > 1:
        raise EvidenceError('multiple_measured_residents')
    if not unique:
        return None
    expected = next(iter(unique.values()))
    return ProcessFamily(expected['pid'], app, helper, expected['startTicks'])


def run(args):
    output = Path(args.output).resolve()
    receipt = Path(args.receipt).resolve()
    sidecar = output.with_name(output.name + '-windows.json')
    if (output == receipt or output in receipt.parents or receipt in output.parents
            or receipt == sidecar or output.exists() or receipt.exists()
            or sidecar.exists() or not output.parent.is_dir()
            or not receipt.parent.is_dir()):
        raise ValueError('output_receipt_and_sidecar_must_be_distinct_new_paths_with_existing_parents')
    receipt.mkdir(mode=0o700)
    started = time.monotonic()
    data = {
        'schemaVersion': 1, 'startedUtc': datetime.now(timezone.utc).isoformat(),
        'scope': 'supplemental physical cold whole-family observation; frozen workload unchanged',
        'command': [sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]],
        'workingDirectory': str(REPO_ROOT), 'seconds': args.seconds,
        'familyPollSeconds': POLL_SECONDS, 'sourceRevision': args.source_revision,
        'observerSha256': sha256(__file__),
        'harnesses': {name: sha256(PERFORMANCE / name) for name in
                      ('observe_idle.py', 'bench_idle.py', 'bench_ui_process.py')},
        'samples': [], 'status': 'starting', 'passed': False,
        'cold40MiBTargetEstablished': False,
        'limitations': [
            'Sequential sampled /proc snapshots can miss descendants born and exited between polls.',
            'Family tracking begins after the frozen five-second warmup and first primary progress.',
            'Observed escaped/reparented descendants remain charged until their recorded identity exits.',
            'Summed RSS double-counts shared pages; PSS and private memory are reported separately.',
            'Desktop services outside the observed descendant family are not charged.',
            'The observer adds host work; compare candidates using this same observer.',
            'No rendered-tray, clipboard, capture, compositor, leak or post-use acceptance is established.'],
    }
    process = owned = family = None
    failed = False
    def persist(final=False):
        write_json(receipt / ('results.json' if final else 'progress.json'), data)
    try:
        persist()
        checker = REPO_ROOT / 'tools/diagnostics/check_socket_paths.py'
        check_command = [sys.executable, str(checker), '--socket',
                         str(output / 'runtime/putgd/session.sock')]
        checked = subprocess.run(check_command, capture_output=True, text=True, timeout=10)
        data['socketPreflight'] = {'command': check_command, 'checkerSha256': sha256(checker),
                                  'exitCode': checked.returncode,
                                  'result': json.loads(checked.stdout)}
        persist()
        if checked.returncode != 0 or not data['socketPreflight']['result']['passed']:
            raise EvidenceError('socket_preflight_rejected')
        binaries = {name: executable(getattr(args, name)) for name in ('app', 'cli', 'helper')}
        data['binaries'] = {name: {'path': str(path), 'sha256': sha256(path),
                                  'version': version(path)}
                            for name, path in binaries.items()}
        persist()
        versions = {row['version'] for row in data['binaries'].values()}
        if any(text.endswith('.dirty') for text in versions):
            raise EvidenceError('dirty_binary_rejected')
        if len(versions) != 1:
            raise EvidenceError('binary_versions_differ')
        if not all(text.endswith('+g' + args.source_revision[:12]) for text in versions):
            raise EvidenceError('binary_source_revision_mismatch')
        argv = [sys.executable, str(PERFORMANCE / 'observe_idle.py'),
                '--app', str(binaries['app']), '--cli', str(binaries['cli']),
                '--output', str(output), '--seconds', str(args.seconds)]
        require_pidfd_support()
        data['primaryCommand'] = argv
        data['status'] = 'running'
        persist()
        with (receipt / 'observer.stdout').open('x') as stdout, (receipt / 'observer.stderr').open('x') as stderr:
            os.chmod(stdout.name, 0o600)
            os.chmod(stderr.name, 0o600)
            process = subprocess.Popen(argv, cwd=REPO_ROOT, stdout=stdout, stderr=stderr,
                                       start_new_session=True)
            owned = OwnedSubtree(process)
            data['observerIdentity'] = owned.root
            last_sample = 0
            while process.poll() is None:
                owned.discover()
                if family is None:
                    family = find_family(output, sidecar, owned, binaries['app'], binaries['helper'])
                    if family:
                        data['residentIdentity'] = family.root
                        owned.mark_resident(family.root)
                now = time.monotonic()
                if family and now - last_sample >= POLL_SECONDS:
                    row = sample_cold_family(family, output)
                    if row is None:
                        data['residentExitAfterPrimaryResults'] = True
                        break
                    data['samples'].append({'elapsedSeconds': now - started, **row})
                    last_sample = now
                    persist()
                time.sleep(.1)
            data['observerExitCode'] = process.wait(timeout=30)
        owned.discover()
        primary = read_json(output / 'results.json')
        observed = read_json(sidecar)
        data['primaryEvidence'] = {'resultsSha256': sha256(output / 'results.json'),
                                   'sidecarSha256': sha256(sidecar),
                                   'legacyBudgetPassed': primary['budgetPassed'],
                                   'elapsedSeconds': primary['elapsedSeconds'],
                                   'idleCpuPercentOneCore': primary['idleCpuPercentOneCore'],
                                   'operationCount': observed['operationCount'],
                                   'coldIdleUninterrupted': observed['coldIdleUninterrupted']}
        samples = data['samples']
        if not samples or family is None:
            raise EvidenceError('whole_family_samples_missing')
        data['exitedProcesses'] = family.exited
        coverage = sample_coverage(samples, primary['elapsedSeconds'])
        data['summary'] = {'sampleCount': len(samples),
                            'coverage': coverage,
                            'peakSampledTotals': {metric: max(row['totals'][metric] for row in samples)
                                                  for metric in ('rssBytes', 'pssBytes', 'privateBytes')},
                            'oneProcessThroughout': single_cold_process(samples, family, owned),
                            'finalTotals': samples[-1]['totals']}
        data['summary']['peakSampledRssBytesIncludingPrimary'] = max(
            data['summary']['peakSampledTotals']['rssBytes'], primary['peakSampledRssBytes'])
        unchanged = (all(sha256(PERFORMANCE / name) == digest
                         for name, digest in data['harnesses'].items())
                     and all(sha256(path) == data['binaries'][name]['sha256']
                             for name, path in binaries.items())
                     and primary['appSha256'] == data['binaries']['app']['sha256']
                     and observed['appSha256'] == data['binaries']['app']['sha256']
                     and observed['cliSha256'] == data['binaries']['cli']['sha256'])
        data['identitiesUnchanged'] = unchanged
        data['passed'] = (unchanged and data['observerExitCode'] == 0 and primary['budgetPassed']
                          and primary['elapsedSeconds'] >= 300
                          and observed['coldIdleUninterrupted'] and observed['operationCount'] == 0
                          and data['summary']['oneProcessThroughout']
                          and coverage['adequate']
                          and data['summary']['peakSampledRssBytesIncludingPrimary'] <= LIMIT_BYTES
                          and primary['idleCpuPercentOneCore'] < .5)
        data['cold40MiBTargetEstablished'] = data['passed']
        data['status'] = 'complete' if data['passed'] else 'failed-gate'
    except BaseException as error:
        failed = True
        data['status'] = 'failed-or-interrupted'
        data['errorType'] = type(error).__name__
        # Fixed EvidenceError labels are safe; arbitrary exceptions may contain
        # user paths or data, so only their class is exported.
        if isinstance(error, EvidenceError):
            data['errorCode'] = str(error)
        raise
    finally:
        try:
            if owned and (process.poll() is None or owned.live(record=False)):
                owned.stop()
            elif process and process.poll() is None:
                # Constructor failure still owns this unreaped direct child.
                signal_generation(identity(process.pid), signal.SIGTERM)
                process.wait(timeout=5)
        except BaseException as error:
            data['cleanupErrorType'] = type(error).__name__
            data['passed'] = data['cold40MiBTargetEstablished'] = False
            data['status'] = 'cleanup-failed'
            if not failed:
                raise
        finally:
            if owned:
                data['ownershipMetadataLimit'] = MAX_OWNED_GENERATIONS
                data['ownedObserverGenerations'] = list(owned.known.values())
                data['cleanupDiscoveryErrorCount'] = owned.cleanup_discovery_error_count
                data['cleanupDiscoveryErrors'] = owned.cleanup_discovery_errors
                data['ownedExecTransitions'] = owned.exec_transitions
                data['observedResidentDescendants'] = list(owned.resident_descendants.values())
            if family:
                data['exitedProcesses'] = family.exited
                if data['passed'] and not single_cold_process(data['samples'], family, owned):
                    data['passed'] = data['cold40MiBTargetEstablished'] = False
                    data['status'] = 'failed-gate'
            data['finishedUtc'] = datetime.now(timezone.utc).isoformat()
            persist(final=True)
    print(json.dumps({'receipt': str(receipt), 'status': data['status'],
                      'cold40MiBTargetEstablished': data['cold40MiBTargetEstablished']}))
    return 0 if data['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('app', 'cli', 'helper', 'output', 'receipt'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--source-revision', required=True,
                        help='Full clean compiled source commit, 40 or 64 lowercase hex digits')
    parser.add_argument('--seconds', type=int, default=300,
                        help='Unchanged primary workload duration; acceptance requires at least 300')
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{40}|[0-9a-f]{64}', args.source_revision):
        parser.error('--source-revision must be a full lowercase commit hash')
    if not 300 <= args.seconds <= 3600:
        parser.error('--seconds must be between 300 and 3600')
    try:
        return run(args)
    except KeyboardInterrupt:
        print('Cold-family observation interrupted; retained private failure receipt.', file=sys.stderr)
        return 130
    except Exception as error:
        print(f'Cold-family observation failed ({type(error).__name__}); inspect private receipt.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
