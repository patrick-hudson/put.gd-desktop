#!/usr/bin/env python3
"""Real Linux process negatives; synthetic children only, no product/display."""
import os
from pathlib import Path
import select
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
                run.call = mock.Mock(side_effect=[{'ui': {'lastExit': {'generation': 0}}},
                    {'shown': 'history'}, {'ui': {'lastExit': {'generation': 1, 'code': 0, 'status': 'normal'}}}])
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
