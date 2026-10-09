#!/usr/bin/env python3
"""Stdlib observer checks; no putgd, GUI, display, cloud or physical workload."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import observe_cold_family as observer


FORK_FIXTURE = r'''
import os
from pathlib import Path
import sys
import time
root = Path(sys.argv[1])
child = os.fork()
if child == 0:
    grandchild = os.fork()
    if grandchild == 0:
        (root / 'grandchild.pid').write_text(str(os.getpid()))
        while not (root / 'exec').exists():
            time.sleep(.01)
        os.execv(sys.argv[2], [sys.argv[2], '60'])
    (root / 'child.pid').write_text(str(os.getpid()))
    while not (root / 'exit-parent').exists():
        time.sleep(.01)
    os._exit(0)
while True:
    try:
        os.waitpid(child, os.WNOHANG)
    except ChildProcessError:
        pass
    time.sleep(.01)
'''


def wait_until(predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.01)
    raise AssertionError('synthetic_fixture_deadline')


class ColdFamilyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        selected = os.environ.get('PUTGD_OBSERVER_TEST_RECEIPTS')
        if selected:
            requested = Path(selected).absolute()
            if (requested.exists() or any(path.is_symlink() for path in (requested, *requested.parents))
                    or not requested.parent.is_dir()):
                raise ValueError('test_receipts_require_new_nonsymlink_path_with_existing_parent')
            requested.mkdir(mode=0o700)
            cls.root = requested
        else:
            cls.root = Path(tempfile.mkdtemp(prefix='pgcf-unit-'))
        if cls.root.stat().st_uid != os.getuid() or cls.root.stat().st_mode & 0o777 != 0o700:
            raise ValueError('test_receipts_must_be_owned_private_directory')
        print(f'Private synthetic observer receipts: {cls.root}', flush=True)

    def retain(self, value):
        observer.write_json(self.root / (self._testMethodName + '.json'), value)

    def test_normal_root_disappearance_after_final_primary(self):
        base = self.root / 'normal-root-exit'
        base.mkdir(mode=0o700)
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        try:
            family = observer.ProcessFamily(process.pid, Path(sys.executable).resolve())
            process.terminate()
            process.wait(timeout=5)
            with self.assertRaises(FileNotFoundError):
                observer.sample_cold_family(family, base)
            observer.write_json(base / 'results.json', {
                'budgetPassed': True, 'elapsedSeconds': 300.1,
                'appSha256': family.expected_hashes[str(family.app)]})
            self.assertIsNone(observer.sample_cold_family(family, base))
            self.retain({'realRootExitObserved': True, 'normalAfterFinal': True,
                         'prematureExitRejected': True, 'applicationExecuted': False})
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)

    def test_terminal_exit_does_not_hide_other_sample_failures(self):
        base = self.root / 'terminal-validation'
        base.mkdir(mode=0o700)
        root = {'pid': 999999999, 'startTicks': 2, 'uid': os.getuid(),
                'executable': str(Path(sys.executable).resolve()), 'device': 3, 'inode': 4}
        family = SimpleNamespace(root=root, app=Path(root['executable']),
                                 expected_hashes={root['executable']: 'a' * 64})
        valid = {'budgetPassed': True, 'elapsedSeconds': 300.1, 'appSha256': 'a' * 64}
        observer.write_json(base / 'results.json', valid)
        raw = FileNotFoundError(2, 'synthetic disappearance', f"/proc/{root['pid']}/stat")
        def fail(): raise raw
        family.sample = fail
        with patch.object(observer, 'identity', return_value=root):
            with self.assertRaises(FileNotFoundError):
                observer.sample_cold_family(family, base)
        with patch.object(observer, 'identity', return_value={**root, 'startTicks': 3}):
            with self.assertRaises(observer.EvidenceError):
                observer.sample_cold_family(family, base)
        with patch.object(observer, 'identity', side_effect=FileNotFoundError()):
            for primary in [{**valid, 'budgetPassed': False},
                            {**valid, 'elapsedSeconds': 299},
                            {**valid, 'appSha256': 'b' * 64}]:
                observer.write_json(base / 'results.json', primary)
                with self.assertRaises(observer.EvidenceError):
                    observer.sample_cold_family(family, base)
        self.retain({'missingDescendantWithLiveRootRejected': True,
                     'reusedRootRejected': True, 'invalidFinalPrimaryRejected': True})

    def test_terminal_normalizer_rejects_unrelated_missing_files_and_live_proc(self):
        base = self.root / 'terminal-boundaries'
        base.mkdir(mode=0o700)
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
        try:
            real = observer.identity(process.pid)
            family = SimpleNamespace(root=real, app=Path(real['executable']),
                                     expected_hashes={real['executable']: 'a' * 64})
            valid = {'budgetPassed': True, 'elapsedSeconds': 300.1, 'appSha256': 'a' * 64}
            observer.write_json(base / 'results.json', valid)
            def missing_exe():
                raise FileNotFoundError(2, 'synthetic executable missing', f'/proc/{process.pid}/exe')
            family.sample = missing_exe
            with patch.object(observer, 'identity', side_effect=FileNotFoundError()):
                with self.assertRaises(observer.EvidenceError) as failure:
                    observer.sample_cold_family(family, base)
                self.assertEqual(str(failure.exception), 'resident_exit_not_confirmed')
            process.terminate()
            process.wait(timeout=5)
            family.root = {**real, 'pid': 999999999}
            def missing_child():
                raise FileNotFoundError(2, 'synthetic child missing', '/proc/999999998/stat')
            family.sample = missing_child
            with patch.object(observer, 'identity') as probe:
                with self.assertRaises(FileNotFoundError):
                    observer.sample_cold_family(family, base)
                probe.assert_not_called()
            def terminal(): raise observer.EvidenceError('resident_exited')
            family.sample = terminal
            self.assertIsNone(observer.sample_cold_family(family, base))
            with patch.object(Path, 'exists', side_effect=[False, True]):
                with self.assertRaises(observer.EvidenceError) as failure:
                    observer.sample_cold_family(family, base)
                self.assertEqual(str(failure.exception), 'resident_identity_changed')
            (base / 'results.json').write_text('{incomplete')
            with self.assertRaises(observer.EvidenceError) as failure:
                observer.sample_cold_family(family, base)
            self.assertEqual(str(failure.exception), 'invalid_final_primary_at_resident_exit')
            def other_failure(): raise observer.EvidenceError('incomplete_smaps_rollup')
            family.sample = other_failure
            with self.assertRaises(observer.EvidenceError) as failure:
                observer.sample_cold_family(family, base)
            self.assertEqual(str(failure.exception), 'incomplete_smaps_rollup')
            self.retain({'liveProcMissingExeRejected': True, 'missingChildRejected': True,
                         'typedNormalExitAccepted': True, 'postValidationReuseRejected': True,
                         'malformedPrimaryRejected': True, 'otherSamplingFailureRejected': True})
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)

    def test_coverage_rejects_sparse_or_gapped_samples(self):
        regular = [{'elapsedSeconds': value} for value in range(299)]
        sparse = [{'elapsedSeconds': 0}, {'elapsedSeconds': 300}]
        gapped = [row for row in regular if row['elapsedSeconds'] not in (1, 2, 3)]
        results = {name: observer.sample_coverage(rows, 300)
                   for name, rows in [('regular', regular), ('sparse', sparse), ('gapped', gapped)]}
        self.retain(results)
        self.assertTrue(results['regular']['adequate'])
        self.assertFalse(results['sparse']['adequate'])
        self.assertFalse(results['gapped']['adequate'])

    def test_generation_rejects_pid_reuse_and_uid_change(self):
        root = {'pid': 1, 'startTicks': 2, 'uid': 3, 'executable': '/before'}
        results = {
            'execRetainsOwnership': observer.same_generation(root, {**root, 'executable': '/after'}),
            'pidReuseRetainsOwnership': observer.same_generation(root, {**root, 'startTicks': 4}),
            'uidChangeRetainsOwnership': observer.same_generation(root, {**root, 'uid': 4}),
        }
        self.retain(results)
        self.assertEqual(results, {'execRetainsOwnership': True,
                                  'pidReuseRetainsOwnership': False,
                                  'uidChangeRetainsOwnership': False})

    def test_exited_or_fast_observed_descendant_rejects_single_resident(self):
        root = {'pid': 1, 'startTicks': 2}
        family = SimpleNamespace(root=root, exited=[])
        owned = SimpleNamespace(resident_descendants={})
        samples = [{'processCount': 1}]
        clean = observer.single_cold_process(samples, family, owned)
        family.exited = [{'pid': 3, 'startTicks': 4}]
        exited = observer.single_cold_process(samples, family, owned)
        family.exited = []
        owned.resident_descendants = {(3, 4): {'pid': 3, 'startTicks': 4}}
        fast = observer.single_cold_process(samples, family, owned)
        self.retain({'clean': clean, 'exitedChild': exited, 'fastObservedChild': fast})
        self.assertTrue(clean)
        self.assertFalse(exited)
        self.assertFalse(fast)

    def rejected_run(self, label, versions=None, long_socket=False):
        base = self.root / label
        base.mkdir(mode=0o700)
        # CI may select a descriptive, long receipt root. Runtime sockets use
        # an independent fresh private short root; only the negative socket
        # case intentionally exceeds the Unix-domain pathname limit.
        runtime = Path(tempfile.mkdtemp(prefix='pgcf-input-'))
        args = argparse.Namespace(app=sys.executable, cli=sys.executable,
                                  helper=sys.executable, output=str(runtime / ('x' * 140 if long_socket else 'run')),
                                  receipt=str(base / 'receipt'), seconds=300,
                                  source_revision='a' * 40)
        original_popen = subprocess.Popen
        def checked_popen(command, *positional, **keywords):
            if any(str(value).endswith('observe_idle.py') for value in command):
                raise AssertionError('unexpected_observer_launch')
            return original_popen(command, *positional, **keywords)
        with patch.object(observer, 'version', side_effect=versions) as version_probe, \
                patch.object(observer.subprocess, 'Popen', side_effect=checked_popen):
            with self.assertRaises(observer.EvidenceError):
                observer.run(args)
        result = json.loads((base / 'receipt/results.json').read_text())
        self.assertFalse(result['passed'])
        self.assertFalse(result['cold40MiBTargetEstablished'])
        self.assertNotIn('observerIdentity', result)
        self.assertFalse(Path(args.output).exists())
        self.assertEqual((base / 'receipt').stat().st_mode & 0o777, 0o700)
        self.assertEqual((base / 'receipt/results.json').stat().st_mode & 0o777, 0o600)
        return result, version_probe.call_count

    def test_preflight_precedes_binary_version_probe(self):
        result, probes = self.rejected_run('socket', long_socket=True)
        self.retain({'errorCode': result['errorCode'], 'versionProbeCount': probes,
                     'socketPreflight': result['socketPreflight']})
        self.assertEqual(result['errorCode'], 'socket_preflight_rejected')
        self.assertEqual(probes, 0)

    def test_dirty_mixed_and_wrong_source_versions_reject_before_launch(self):
        clean = '0.1.1-dev.51+g' + 'a' * 12
        cases = [('dirty', [clean + '.dirty'] * 3, 'dirty_binary_rejected'),
                 ('mixed', [clean, clean, '0.1.1-dev.52+g' + 'a' * 12], 'binary_versions_differ'),
                 ('wrong', ['0.1.1-dev.51+g' + 'b' * 12] * 3, 'binary_source_revision_mismatch')]
        results = {}
        for label, versions, expected in cases:
            result, probes = self.rejected_run(label, versions=versions)
            results[label] = {'errorCode': result['errorCode'], 'versionProbeCount': probes}
            self.assertEqual(result['errorCode'], expected)
            self.assertEqual(probes, 3)
        self.retain(results)

    @unittest.skipUnless(sys.platform == 'linux' and Path('/proc').is_dir(), 'Linux /proc and fork required')
    def test_real_fork_exec_reparent_cleanup(self):
        self.exercise_tree()

    @unittest.skipUnless(sys.platform == 'linux' and Path('/proc').is_dir(), 'Linux /proc and fork required')
    def test_preidentification_dead_parent_live_orphan(self):
        self.exercise_tree(pre_identification=True)

    @unittest.skipUnless(sys.platform == 'linux' and Path('/proc').is_dir(), 'Linux /proc and fork required')
    def test_cap_failure_still_cleans_known_generations(self):
        self.exercise_tree(cap_failure=True)

    def test_pidfd_binding_rejects_reused_generation_and_missing_api(self):
        expected = {'pid': os.getpid(), 'startTicks': 1, 'uid': os.getuid()}
        with patch.object(observer, 'identity', return_value={**expected, 'startTicks': 2}), \
                patch.object(observer.signal, 'pidfd_send_signal') as send:
            self.assertFalse(observer.signal_generation(expected, signal.SIGTERM))
            self.assertEqual(send.call_count, 0)
        with patch.object(observer.os, 'pidfd_open', None):
            with self.assertRaises(observer.EvidenceError):
                observer.signal_generation(expected, signal.SIGTERM)
        self.retain({'generationMismatchSignalCount': 0, 'missingApiRejected': True})

    def exercise_tree(self, pre_identification=False, cap_failure=False):
        sleep = shutil.which('sleep')
        self.assertIsNotNone(sleep, 'standard distro sleep utility required for executable transition')
        base = self.root / self._testMethodName
        base.mkdir(mode=0o700)
        # Own the orphan in this test process so it is reaped after cleanup,
        # rather than leaving a zombie to an unrelated container PID 1.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        process = owned = None
        grandchild = None
        result = {'status': 'starting', 'desktopProof': False}
        try:
            process = subprocess.Popen([sys.executable, '-c', FORK_FIXTURE, str(base), sleep],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       start_new_session=True)
            owned = observer.OwnedSubtree(process)
            if not pre_identification:
                owned.mark_resident(owned.root)
            wait_until(lambda: (base / 'grandchild.pid').is_file() and (base / 'child.pid').is_file())
            grandchild = int((base / 'grandchild.pid').read_text())
            child = int((base / 'child.pid').read_text())
            owned.discover()
            initial = observer.identity(grandchild)
            self.assertIn(observer.identity_key(initial), owned.known)
            (base / 'exec').touch()
            wait_until(lambda: observer.identity(grandchild)['executable'] == str(Path(sleep).resolve()))
            if not cap_failure:
                owned.discover()
            (base / 'exit-parent').touch()
            wait_until(lambda: not Path(f'/proc/{child}').exists())
            if pre_identification:
                owned.mark_resident(owned.root)
                # Must charge the live orphan immediately, before discovery.
                self.assertIn(observer.identity_key(initial), owned.resident_descendants)
            if cap_failure:
                with patch.object(observer, 'MAX_OWNED_GENERATIONS', 0):
                    with self.assertRaises(observer.EvidenceError) as failure:
                        owned.discover()
                    self.assertEqual(str(failure.exception), 'owned_exec_transition_limit')
                    with self.assertRaises(observer.EvidenceError) as cleanup:
                        owned.stop()
                    self.assertEqual(str(cleanup.exception), 'cleanup_discovery_failed')
                os.waitpid(grandchild, 0)
                grandchild = None
                self.assertFalse(owned.live(record=False))
                result.update(status='passed', originalError='owned_exec_transition_limit',
                              cleanupErrors=owned.cleanup_discovery_errors,
                              allOwnedLiveProcessesGone=True)
                return
            owned.discover()
            current = observer.identity(grandchild)
            self.assertTrue(observer.same_generation(initial, current))
            self.assertTrue(owned.exec_transitions)
            self.assertIn(observer.identity_key(current), owned.resident_descendants)
            self.assertIn(grandchild, [row['pid'] for row in owned.live()])
            owned.stop()
            os.waitpid(grandchild, 0)
            grandchild = None
            self.assertFalse(owned.live())
            result.update(status='passed', observerExitCode=process.returncode,
                          transitions=owned.exec_transitions,
                          observedResidentDescendants=list(owned.resident_descendants.values()),
                          allOwnedLiveProcessesGone=True)
        except BaseException as error:
            result.update(status='failed', errorType=type(error).__name__)
            raise
        finally:
            try:
                if owned and (process.poll() is None or owned.live(record=False)):
                    owned.stop()
                elif process and process.poll() is None:
                    observer.signal_generation(observer.identity(process.pid), signal.SIGKILL)
                    process.wait(timeout=5)
                if grandchild is not None:
                    os.waitpid(grandchild, 0)
            finally:
                libc.prctl(36, previous.value, 0, 0, 0)
                self.retain(result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
