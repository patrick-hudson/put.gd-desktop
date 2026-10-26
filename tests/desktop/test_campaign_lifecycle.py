#!/usr/bin/env python3
"""Real Linux process negatives; synthetic children only, no product/display."""
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import campaign_lifecycle as lifecycle
from bench_ui_process import EvidenceError, identity
import physical_campaign


class CleanupTests(unittest.TestCase):
    def child(self, code=0, ignore=False):
        script = ('import sys,time; print("ready",flush=True); sys.stdin.readline(); '
                  + ('time.sleep(30)' if ignore else f'sys.exit({code})'))
        process = subprocess.Popen([sys.executable, '-c', script], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True, start_new_session=True)
        self.assertEqual(process.stdout.readline().strip(), 'ready')
        self.addCleanup(self.finish, process)
        return process

    def finish(self, process):
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        process.stdin.close(); process.stdout.close()

    def cleanup(self, process, root, **options):
        known = [identity(process.pid)]
        def quit_request():
            process.stdin.write('\n'); process.stdin.flush()
        return lifecycle.cleanup(process, known, root / 'runtime', root / 'data',
                                 root / 'runtime/putgd/session.sock', quit_request, grace=.15, drain=.1, **options)

    def test_graceful_exit_zero_and_absence_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.cleanup(self.child(), Path(directory))
            self.assertTrue(result['passed'])
            self.assertEqual(result['exitCode'], 0)
            self.assertFalse(result['forced'])
            self.assertEqual(result['remaining'], [])

    def test_graceful_request_followed_by_exit_seven_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.cleanup(self.child(code=7), Path(directory))
            self.assertFalse(result['passed'])
            self.assertEqual(result['exitCode'], 7)

    def test_forced_termination_cannot_be_reported_as_normal_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.cleanup(self.child(ignore=True), Path(directory))
            self.assertFalse(result['passed'])
            self.assertTrue(result['forced'])
            self.assertNotEqual(result['exitCode'], 0)

    def test_socket_left_behind_invalidates_exit_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            socket = root / 'runtime/putgd/session.sock'
            socket.parent.mkdir(parents=True)
            socket.touch()
            result = self.cleanup(self.child(), root)
            self.assertEqual(result['exitCode'], 0)
            self.assertFalse(result['passed'])
            self.assertFalse(result['socketAbsent'])
            self.assertTrue(socket.exists(), 'Never unlink an unexplained socket to obtain a pass')

    def test_escaped_observed_generation_blocks_otherwise_normal_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.child()
            escaped = self.child(ignore=True)
            known = [identity(root.pid), identity(escaped.pid)]
            def quit_request():
                root.stdin.write('\n'); root.stdin.flush()
            result = lifecycle.cleanup(root, known, Path(directory) / 'r', Path(directory) / 'd',
                                       Path(directory) / 's', quit_request, grace=.15, drain=.1)
            self.assertFalse(result['passed'])
            self.assertTrue(result['forced'])

    def test_repeated_inventory_fault_terminates_only_known_owned_generations(self):
        # Real children; only the unrelated /proc inventory failure is injected.
        for error in (EvidenceError('product_cleanup_inventory_permission_denied'), PermissionError()):
            with self.subTest(error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                owned, unrelated = self.child(ignore=True), self.child()
                unrelated_identity = identity(unrelated.pid)
                with mock.patch.object(lifecycle, 'profile_inventory', side_effect=error) as inventory, \
                     mock.patch.object(lifecycle.os, 'pidfd_open', wraps=os.pidfd_open) as pidfd:
                    result = self.cleanup(owned, Path(directory))
                self.assertFalse(result['passed'])
                self.assertTrue(result['gracefulRequest'])
                self.assertTrue(result['forced'])
                self.assertFalse(result['inventoryComplete'])
                self.assertFalse(result['absenceVerified'])
                self.assertNotEqual(result['exitCode'], 0)
                self.assertEqual(result['remaining'], [])
                self.assertTrue(result['socketAbsent'])
                self.assertGreaterEqual(inventory.call_count, 3)
                self.assertEqual({call.args[0] for call in pidfd.call_args_list}, {owned.pid})
                self.assertIsNotNone(owned.poll())
                with self.assertRaises(ChildProcessError):
                    os.waitpid(owned.pid, os.WNOHANG)
                self.assertIsNone(unrelated.poll())
                self.assertEqual(identity(unrelated.pid), unrelated_identity)
                expected = str(error) if isinstance(error, EvidenceError) else 'PermissionError'
                self.assertIn(expected, result['errors'])
                self.assertIn('resident_graceful_exit_timeout', result['errors'])

    def test_initial_retry_and_final_inventory_faults_never_claim_absence(self):
        for stage, fault_index in (('initial', 0), ('retry', 2), ('final', 3)):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as directory:
                process = self.child()
                scans = [[], [], [], []]
                scans[fault_index] = EvidenceError('product_cleanup_inventory_permission_denied')
                with mock.patch.object(lifecycle, 'profile_inventory', side_effect=scans) as inventory:
                    result = self.cleanup(process, Path(directory))
                self.assertEqual(inventory.call_count, 4)
                self.assertTrue(result['gracefulRequest'])
                self.assertEqual(result['exitCode'], 0)
                self.assertFalse(result['forced'])
                self.assertEqual(result['remaining'], [])
                self.assertTrue(result['socketAbsent'])
                self.assertFalse(result['passed'])
                self.assertFalse(result['inventoryComplete'])
                self.assertFalse(result['absenceVerified'])
                self.assertEqual(result['errors'], ['product_cleanup_inventory_permission_denied'])

    def test_stale_known_generation_never_signals_reused_unrelated_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            root, unrelated = self.child(), self.child()
            before = identity(unrelated.pid)
            stale = {**before, 'startTicks': before['startTicks'] + 1}
            def quit_request():
                root.stdin.write('\n'); root.stdin.flush()
            with mock.patch.object(lifecycle, 'profile_inventory', return_value=[]), \
                 mock.patch.object(lifecycle.signal, 'pidfd_send_signal') as send:
                result = lifecycle.cleanup(root, [identity(root.pid), stale], Path(directory) / 'r',
                    Path(directory) / 'd', Path(directory) / 's', quit_request, grace=.15, drain=.1)
            self.assertTrue(result['passed'])
            send.assert_not_called()
            self.assertIsNone(unrelated.poll())
            self.assertEqual(identity(unrelated.pid), before)


class LaunchOwnershipTests(unittest.TestCase):
    def run_state(self, root):
        run = physical_campaign.Campaign.__new__(physical_campaign.Campaign)
        run.root, run.result = root, {}
        run.args = SimpleNamespace(app=sys.executable, helper=sys.executable, cli='/not-executed')
        run.environment = {'XDG_RUNTIME_DIR': str(root / 'r'), 'XDG_DATA_HOME': str(root / 'd')}
        run.socket = str(root / 'missing-session.sock')
        run.resident = run.family = run.log = run.launch_identity = run.resident_pidfd = None
        run.event, run.persist = mock.Mock(), mock.Mock()
        return run

    def child(self):
        return subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(30)'], start_new_session=True)

    def finish(self, run, child):
        if child.poll() is None:
            child.terminate()
        child.wait(timeout=5)
        if run.log:
            run.log.close()
        if run.resident_pidfd is not None:
            try:
                os.close(run.resident_pidfd)
            except OSError:
                pass

    def test_unready_launch_and_spawn_interrupt_keep_authenticated_ownership(self):
        for phase in ('during_spawn', 'family_authentication'):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory:
                run, child = self.run_state(Path(directory)), self.child()
                interrupted = KeyboardInterrupt('synthetic-launch-interrupt')
                def interrupt(_signum, _frame):
                    raise interrupted
                previous = signal.signal(signal.SIGTERM, interrupt)
                try:
                    def spawn(*_args, **_options):
                        if phase == 'during_spawn':
                            os.kill(os.getpid(), signal.SIGTERM)
                        return child
                    with mock.patch.object(physical_campaign.subprocess, 'Popen', side_effect=spawn), \
                         mock.patch.object(physical_campaign, 'ProcessFamily', side_effect=interrupted):
                        with self.assertRaises(KeyboardInterrupt) as failure:
                            run.start()
                    self.assertIs(failure.exception, interrupted)
                    self.assertIs(run.resident, child)
                    self.assertIsNone(run.family)
                    self.assertEqual(run.launch_identity, identity(child.pid))
                    os.fstat(run.resident_pidfd)
                    with mock.patch.object(lifecycle, 'profile_inventory', side_effect=EvidenceError('synthetic-inventory-fault')):
                        self.assertFalse(run.stop())
                    self.assertIsNotNone(child.poll())
                    with self.assertRaises(ChildProcessError):
                        os.waitpid(child.pid, os.WNOHANG)
                    self.assertIsNone(run.resident)
                    self.assertFalse(run.result['residentCleanup'][-1]['absenceVerified'])
                finally:
                    signal.signal(signal.SIGTERM, previous)
                    self.finish(run, child)

    def test_family_unset_socket_unavailable_inventory_fault_still_reaps_child(self):
        with tempfile.TemporaryDirectory() as directory:
            run, child = self.run_state(Path(directory)), self.child()
            run.resident, run.log = child, (Path(directory) / 'resident.log').open('x')
            run.resident_pidfd = os.pidfd_open(child.pid)
            try:
                with mock.patch.object(lifecycle, 'profile_inventory', side_effect=EvidenceError('synthetic-inventory-fault')):
                    self.assertFalse(run.stop())
                self.assertIsNotNone(child.poll())
                self.assertIsNone(run.resident)
                self.assertEqual(run.result['residentLaunches'][0]['identity']['pid'], child.pid)
                self.assertTrue(run.result['residentCleanup'][0]['forced'])
                self.assertFalse(run.result['residentCleanup'][0]['absenceVerified'])
            finally:
                self.finish(run, child)

    def test_failed_cleanup_return_with_live_root_retains_owner_and_blocks_relaunch(self):
        for alleged_pass in (False, True):
            with self.subTest(alleged_pass=alleged_pass), tempfile.TemporaryDirectory() as directory:
                run, child = self.run_state(Path(directory)), self.child()
                run.resident, run.log = child, (Path(directory) / 'resident.log').open('x')
                run.resident_pidfd = os.pidfd_open(child.pid)
                try:
                    with mock.patch.object(lifecycle, 'cleanup', return_value={'passed': alleged_pass, 'errors': [],
                                          'absenceVerified': alleged_pass, 'remaining': []}):
                        self.assertFalse(run.stop())
                    self.assertIs(run.resident, child)
                    self.assertIsNone(child.poll())
                    self.assertFalse(run.log.closed)
                    os.fstat(run.resident_pidfd)
                    receipt = run.result['residentCleanup'][-1]
                    self.assertFalse(receipt['passed'])
                    self.assertFalse(receipt['absenceVerified'])
                    self.assertTrue(receipt['ownershipRetained'])
                    self.assertTrue(receipt['ownedRootAlive'])
                    self.assertEqual(receipt['knownOwned'][0]['startTicks'], identity(child.pid)['startTicks'])
                    with mock.patch.object(physical_campaign.subprocess, 'Popen') as spawn:
                        with self.assertRaisesRegex(EvidenceError, 'ownership_not_released'):
                            run.start()
                        spawn.assert_not_called()
                finally:
                    self.finish(run, child)

    def test_launch_authentication_requires_direct_parent_and_uid_not_name(self):
        with tempfile.TemporaryDirectory() as directory:
            run, child = self.run_state(Path(directory)), self.child()
            run.resident = child
            try:
                fields = lifecycle.stat_fields(Path(f'/proc/{child.pid}/stat').read_text())
                with mock.patch.object(physical_campaign, 'stat_fields', return_value={**fields, 'ppid': os.getpid() + 1}), \
                     mock.patch.object(physical_campaign.os, 'pidfd_open') as opened:
                    with self.assertRaisesRegex(EvidenceError, 'launch_not_owned_child'):
                        run.capture_launch()
                    opened.assert_not_called()
                self.assertIsNone(run.launch_identity)
                self.assertIsNone(child.poll())
            finally:
                self.finish(run, child)


class HistoryLifetimeTests(unittest.TestCase):
    def setUp(self):
        self.process = subprocess.Popen([sys.executable, '-c',
            'import sys;print("ready",flush=True);sys.stdin.readline()'], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, text=True)
        self.assertEqual(self.process.stdout.readline().strip(), 'ready')
        self.guard = lifecycle.HistoryGuard.__new__(lifecycle.HistoryGuard)
        self.guard.owner = identity(self.process.pid)
        self.guard.resident_pid = os.getpid()
        self.guard.pidfd = os.pidfd_open(self.process.pid)
        self.guard.checks = 0
        self.guard.events = lambda: []  # Only the native event source is synthetic.

    def tearDown(self):
        if self.process.poll() is None:
            self.process.stdin.write('\n'); self.process.stdin.flush()
        self.process.wait(timeout=5)
        self.process.stdin.close(); self.process.stdout.close()
        os.close(self.guard.pidfd)

    def test_live_identity_is_required_at_each_guard_check(self):
        self.guard.check()
        self.process.terminate(); self.process.wait(timeout=5)
        self.assertTrue(select.select([self.guard.pidfd], [], [], 1)[0])
        with self.assertRaisesRegex(EvidenceError, 'helper_exited_during_interval'):
            self.guard.check()

    def test_unmap_then_remap_between_samples_is_still_a_failure(self):
        self.guard.check()
        self.guard.events = lambda: [18, 19]
        with self.assertRaisesRegex(EvidenceError, 'window_vanished'):
            self.guard.check()

    def test_replacement_helper_generation_is_not_accepted(self):
        changed = {**self.guard.owner, 'startTicks': self.guard.owner['startTicks'] + 1}
        with mock.patch.object(lifecycle, 'identity', return_value=changed):
            with self.assertRaisesRegex(EvidenceError, 'generation_changed'):
                self.guard.check()

    def test_crash_nonzero_and_old_exit_receipts_are_rejected(self):
        self.assertTrue(lifecycle.normal_helper_exit({'code': 0, 'status': 'normal', 'generation': 2}, 1))
        for value in ({'code': 1, 'status': 'normal', 'generation': 2},
                      {'code': 0, 'status': 'crash', 'generation': 2},
                      {'code': 0, 'status': 'normal', 'generation': 1}):
            self.assertFalse(lifecycle.normal_helper_exit(value, 1))


class FinalizationTests(unittest.TestCase):
    def test_unexpected_cleanup_retains_owned_process_pidfd_and_failure_context(self):
        for export_fails in (False, True):
            with self.subTest(export_fails=export_fails), tempfile.TemporaryDirectory() as directory:
                child = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(30)'], start_new_session=True)
                pidfd = os.pidfd_open(child.pid)
                log = (Path(directory) / 'resident.log').open('x')
                try:
                    run = physical_campaign.Campaign.__new__(physical_campaign.Campaign)
                    owner = identity(child.pid)
                    run.resident, run.log, run.resident_pidfd = child, log, pidfd
                    run.family = SimpleNamespace(known={child.pid: owner}, exited=[])
                    run.environment = {'XDG_RUNTIME_DIR': directory + '/r', 'XDG_DATA_HOME': directory + '/d'}
                    run.socket, run.result = directory + '/s', {}
                    run.event, run.persist = mock.Mock(), mock.Mock()
                    if export_fails:
                        run.event.side_effect = run.persist.side_effect = OSError('synthetic-export-failure')
                    original = RuntimeError('synthetic-unexpected-cleanup-failure')
                    with mock.patch.object(lifecycle, 'cleanup', side_effect=original):
                        with self.assertRaises(RuntimeError) as failure:
                            run.stop()
                    self.assertIs(failure.exception, original)
                    self.assertIs(run.resident, child)
                    self.assertIs(run.log, log)
                    os.fstat(pidfd)  # Ownership descriptor remains usable.
                    self.assertIsNone(child.poll())
                    receipt = run.result['residentCleanup'][0]
                    self.assertFalse(receipt['passed'])
                    self.assertFalse(receipt['absenceVerified'])
                    self.assertIsNone(receipt['remaining'])
                    self.assertTrue(receipt['ownershipRetained'])
                    self.assertEqual(receipt['knownOwned'][0]['startTicks'], owner['startTicks'])
                    self.assertEqual(receipt['residentPid'], child.pid)
                    run.event.assert_called_once(); run.persist.assert_called_once()
                    if export_fails:
                        self.assertEqual(len(original.__notes__), 2)
                finally:
                    child.terminate(); child.wait(timeout=5)
                    try:
                        os.close(pidfd)
                    except OSError:
                        pass  # Before-control source may have closed it prematurely.
                    log.close()

    def test_unexpected_stop_failure_still_cleans_fixtures_and_preserves_cause(self):
        arguments = ['memory', '--output', '/not-created', '--package', '/not-opened',
            '--package-sha256', 'a' * 64, '--source-revision', 'b' * 40, '--version', 'test',
            '--desktop-session', 'not-used', '--lock-session', 'not-used']
        for export_fault in ('none', 'summarize', 'persist', 'both'):
            with self.subTest(export_fault=export_fault):
                run = mock.Mock()
                run.started, run.result = time.monotonic(), {'gates': {}, 'residentCleanup': [{'passed': False}]}
                original = RuntimeError('synthetic-unexpected-cleanup-failure')
                run.stop.side_effect = original
                run.close_fixtures.return_value = True
                if export_fault in ('summarize', 'both'):
                    run.summarize_processes.side_effect = OSError('synthetic-summary-failure')
                if export_fault in ('persist', 'both'):
                    run.persist.side_effect = OSError('synthetic-persist-failure')
                with mock.patch.object(physical_campaign, 'Campaign', return_value=run), \
                     mock.patch.object(physical_campaign.signal, 'signal'), mock.patch.object(physical_campaign.os, 'umask'):
                    with self.assertRaisesRegex(EvidenceError, 'cleanup_not_normal_complete') as failure:
                        physical_campaign.main(arguments)
                self.assertIs(failure.exception.__cause__, original)
                run.close_fixtures.assert_called_once()
                run.summarize_processes.assert_called_once(); run.persist.assert_called_once()
                if export_fault != 'none':
                    self.assertTrue(original.__notes__)
                self.assertEqual(run.result['status'], 'failed-or-interrupted')
                self.assertEqual(run.result['gates']['cleanup']['status'], 'fail')

    def test_primary_workload_error_survives_cleanup_and_both_export_faults(self):
        run = mock.Mock()
        run.started, run.cover = time.monotonic(), None
        run.result = {'gates': {}, 'residentCleanup': []}
        primary = ValueError('synthetic-primary-workload-failure')
        run.memory.side_effect = primary
        run.stop.side_effect = RuntimeError('synthetic-cleanup-failure')
        run.close_fixtures.return_value = True
        run.summarize_processes.side_effect = OSError('synthetic-summary-failure')
        run.persist.side_effect = OSError('synthetic-persist-failure')
        arguments = ['memory', '--output', '/not-created', '--package', '/not-opened',
            '--package-sha256', 'a' * 64, '--source-revision', 'b' * 40, '--version', 'test',
            '--desktop-session', 'not-used', '--lock-session', 'not-used']
        with mock.patch.object(physical_campaign, 'Campaign', return_value=run), \
             mock.patch.object(physical_campaign.signal, 'signal'), mock.patch.object(physical_campaign.os, 'umask'):
            with self.assertRaises(ValueError) as failure:
                physical_campaign.main(arguments)
        self.assertIs(failure.exception, primary)
        self.assertEqual(len(primary.__notes__), 2)
        run.close_fixtures.assert_called_once(); run.persist.assert_called_once()
        self.assertEqual(run.result['status'], 'failed-or-interrupted')

    def test_export_failure_alone_fails_and_still_attempts_both_exports(self):
        for stage in ('summarize', 'persist'):
            with self.subTest(stage=stage):
                run = mock.Mock()
                run.started, run.result = time.monotonic(), {'gates': {}, 'residentCleanup': []}
                run.stop.return_value = run.close_fixtures.return_value = True
                original = OSError('synthetic-export-failure')
                (run.summarize_processes if stage == 'summarize' else run.persist).side_effect = original
                arguments = ['memory', '--output', '/not-created', '--package', '/not-opened',
                    '--package-sha256', 'a' * 64, '--source-revision', 'b' * 40, '--version', 'test',
                    '--desktop-session', 'not-used', '--lock-session', 'not-used']
                with mock.patch.object(physical_campaign, 'Campaign', return_value=run), \
                     mock.patch.object(physical_campaign.signal, 'signal'), mock.patch.object(physical_campaign.os, 'umask'):
                    with self.assertRaises(OSError) as failure:
                        physical_campaign.main(arguments)
                self.assertIs(failure.exception, original)
                run.summarize_processes.assert_called_once(); run.persist.assert_called_once()
                self.assertEqual(run.result['status'], 'failed-or-interrupted')
                self.assertEqual(run.result['gates']['evidenceExport']['status'], 'fail')

    def test_memory_caller_requires_30_measured_open_seconds_before_assessment(self):
        budget = 60 * physical_campaign.MIB
        for requested, measured, peak, expected in ((1, 1, budget, 'unassessed'),
                (30, 1, budget + 1, 'unassessed'), (30, 29.999, budget, 'unassessed'),
                (30, 30, budget, 'pass'), (30, 30, budget + 1, 'fail')):
            with self.subTest(requested=requested, measured=measured, peak=peak):
                run = physical_campaign.Campaign.__new__(physical_campaign.Campaign)
                run.args = SimpleNamespace(open_seconds=requested)
                run.resident = SimpleNamespace(pid=1)
                run.result = {'metrics': {}, 'gates': {}}
                run.visual, run.persist, run.event = mock.Mock(), mock.Mock(), mock.Mock()
                rows = [(10, {'totals': {'pssBytes': peak}, 'processes': [{'pid': 2, 'role': 'ui'}]}),
                        (10 + measured, {'totals': {'pssBytes': peak}, 'processes': [{'pid': 2, 'role': 'ui'}]})]
                run.interval = mock.Mock(return_value=rows)
                run.sample = mock.Mock(side_effect=[{'processes': [{'pid': 2, 'role': 'ui'}]}, {'processCount': 1}])
                # Source-derived production contracts, not a recorded response:
                # diagnostics has no ui; ui.snapshot nests it under diagnostics.
                exits = iter([{}, {'generation': 1, 'code': 0, 'status': 'normal'}])
                def response(method, params=None):
                    if method == 'diagnostics':
                        return {'journalReady': True, 'activeOperations': 0}
                    if method == 'ui.snapshot':
                        return {'diagnostics': {'journalReady': True, 'ui': {
                            'state': 'stopped', 'pid': 0, 'checkpointBytes': 0, 'lastExit': next(exits)}}}
                    self.assertEqual((method, params), ('ui.show', {'view': 'history'}))
                    return {'shown': 'history'}
                run.call = mock.Mock(side_effect=response)
                with mock.patch.object(physical_campaign.time, 'sleep'), \
                     mock.patch.object(physical_campaign, 'owned_windows', side_effect=lambda pid: ['0x2'] if pid == 2 else []), \
                     mock.patch.object(physical_campaign.subprocess, 'run'), \
                     mock.patch.object(physical_campaign, 'memory_summary', return_value={
                         'seconds': 300, 'residentCpuPercentOneCore': 0, 'peakFamilyRssBytes': 0, 'peakFamilyPssBytes': 0}), \
                     mock.patch.object(lifecycle, 'HistoryGuard'):
                    run.memory()
                gate = run.result['gates']['historyOpenPss']
                self.assertEqual(gate['status'], expected)
                self.assertEqual(gate['minimumSeconds'], 30)
                self.assertEqual(gate['limitBytes'], budget)
                self.assertAlmostEqual(gate['measuredSeconds'], measured)
                self.assertEqual(run.result['metrics']['historyOpenPeakFamilyPssBytes'], peak)
                self.assertAlmostEqual(run.result['metrics']['historyOpenMeasuredSeconds'], measured)
                self.assertEqual(run.call.call_args_list, [mock.call('ui.snapshot'),
                    mock.call('ui.show', {'view': 'history'}), mock.call('ui.snapshot')])
                self.assertEqual([call.args[:2] for call in run.interval.call_args_list],
                    [('cold-closed', 300), ('history-open', requested), ('history-closed', 300)])

    def test_ui_exit_receipt_requires_nested_production_snapshot_evidence(self):
        run = physical_campaign.Campaign.__new__(physical_campaign.Campaign)
        for malformed in (None, {}, {'ui': {'lastExit': {}}}, {'diagnostics': None},
                {'diagnostics': {'ui': None}}, {'diagnostics': {'ui': {}}},
                {'diagnostics': {'ui': {'lastExit': None}}},
                {'diagnostics': {'ui': {'lastExit': {'generation': 1}}}},
                {'diagnostics': {'ui': {'lastExit': {'generation': '1', 'code': 0, 'status': 'normal'}}}}):
            with self.subTest(snapshot=malformed):
                run.call = mock.Mock(return_value=malformed)
                with self.assertRaisesRegex(EvidenceError, 'ui_snapshot_exit_receipt_missing_or_invalid'):
                    run.ui_last_exit()
                run.call.assert_called_once_with('ui.snapshot')
        for receipt in ({}, {'generation': 7, 'code': 0, 'status': 'normal'},
                        {'generation': 8, 'code': 1, 'status': 'crash'}):
            with self.subTest(receipt=receipt):
                run.call = mock.Mock(return_value={'diagnostics': {'ui': {'lastExit': receipt}}})
                self.assertEqual(run.ui_last_exit(), receipt)

    def test_timed_capture_caller_releases_mouse_after_child_and_keyboard_timeouts(self):
        # Exercise Campaign.capture itself, not just its mouse-release helper.
        # No subprocess is launched and no native input is sent.
        for termination_fails in (False, True):
            with self.subTest(termination_fails=termination_fails), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                driver = root / 'synthetic-driver'
                driver.write_bytes(b'unit-test instrumentation identity')
                run = physical_campaign.Campaign.__new__(physical_campaign.Campaign)
                run.root, run.environment, run.result = root, {}, {}
                run.resident, run.event = SimpleNamespace(pid=123), mock.Mock()
                run.args = SimpleNamespace(driver=str(driver), capture_driver_manifest=None,
                                           capture_helper='/not-executed')
                process = mock.Mock()
                process.poll.return_value = None
                process.wait.side_effect = subprocess.TimeoutExpired('capture-driver', 240)
                def input_command(command, **_options):
                    if command[1] == 'keyup':
                        raise subprocess.TimeoutExpired('xdotool-keyup', 5)
                    self.assertEqual(command, ['xdotool', 'mouseup', '1'])
                with mock.patch.object(physical_campaign, 'operation_ids', return_value=set()), \
                     mock.patch.object(physical_campaign.subprocess, 'Popen', return_value=process), \
                     mock.patch.object(physical_campaign.subprocess, 'run', side_effect=input_command) as input_run, \
                     mock.patch.object(physical_campaign, 'stop_owned_process_group',
                                       side_effect=subprocess.TimeoutExpired('terminate', 5) if termination_fails else None) as stop:
                    with self.assertRaises(subprocess.TimeoutExpired) as failure:
                        run.capture({'source': {'shortcuts': {'bindings': {'region': {'bound': True}}}}})
                    self.assertEqual(failure.exception.cmd, 'xdotool-keyup')
                    self.assertIsInstance(failure.exception.__context__, subprocess.TimeoutExpired)
                stop.assert_called_once_with(process)
                self.assertEqual([call.args[0] for call in input_run.call_args_list],
                                 [['xdotool', 'keyup', 'Control_L', 'Shift_L', 'F12'], ['xdotool', 'mouseup', '1']])

    def test_dead_visual_helper_does_not_skip_owned_button_release(self):
        runner = mock.Mock()
        with mock.patch.object(physical_campaign, 'identity', side_effect=FileNotFoundError):
            physical_campaign.cancel_visual_input({'pid': 999999}, True, runner)
        self.assertEqual(runner.call_args.args[0], ['xdotool', 'mouseup', '1'])
        self.assertEqual(runner.call_count, 1)

    def test_escape_failure_still_releases_owned_mouse_button(self):
        runner = mock.Mock(side_effect=[subprocess.TimeoutExpired('xdotool', 5), None])
        with mock.patch.object(physical_campaign, 'identity', return_value=identity(os.getpid())):
            with self.assertRaises(subprocess.TimeoutExpired):
                physical_campaign.cancel_visual_input(identity(os.getpid()), True, runner)
        self.assertEqual(runner.call_args.args[0], ['xdotool', 'mouseup', '1'])

    def test_successful_workload_cannot_return_success_before_failed_cleanup(self):
        run = mock.Mock()
        run.started = time.monotonic()
        run.result = {'gates': {}, 'residentCleanup': []}
        run.stop.return_value = False
        run.close_fixtures.return_value = True
        arguments = ['memory', '--output', '/not-created', '--package', '/not-opened',
                     '--package-sha256', 'a' * 64, '--source-revision', 'b' * 40,
                     '--version', 'test', '--desktop-session', 'not-used', '--lock-session', 'not-used']
        with mock.patch.object(physical_campaign, 'Campaign', return_value=run), \
             mock.patch.object(physical_campaign.signal, 'signal'), \
             mock.patch.object(physical_campaign.os, 'umask'):
            with self.assertRaisesRegex(EvidenceError, 'cleanup_not_normal_complete'):
                physical_campaign.main(arguments)
        self.assertEqual(run.result['status'], 'failed-or-interrupted')
        self.assertEqual(run.result['gates']['cleanup']['status'], 'fail')
        run.persist.assert_called()


if __name__ == '__main__':
    unittest.main()
