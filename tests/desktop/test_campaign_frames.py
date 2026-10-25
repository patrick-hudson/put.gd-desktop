#!/usr/bin/env python3
"""Whole-frame privacy negatives; exact bytes delivered to image savers checked."""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'performance'))
import campaign_frames as frames
import campaign_ui as ui
import physical_campaign as campaign
from bench_ui_process import EvidenceError


def frame(width, height):
    return bytearray(bytes((100, 90, 80, 0)) * (width * height))


def notification(raw, width, x, y, w, h):
    for row in range(y, y + h):
        begin = (row * width + x) * 4
        raw[begin:begin + w * 4] = bytes((33, 77, 211, 0)) * w


class PointerParkingTests(unittest.TestCase):
    def owner(self):
        return {'pid': 123, 'startTicks': 456, 'uid': os.getuid(), 'device': 7,
                'inode': 8, 'executable': '/not-executed/putgd-ui'}

    def test_already_parked_reads_xy_once_without_any_move_or_sync(self):
        owner = self.owner()
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.subprocess, 'check_output', return_value='X=80\nY=80\nWINDOW=999\nSCREEN=0') as read, \
             mock.patch.object(ui.subprocess, 'run') as move:
            ui._park_pointer((80, 80), owner, (100, 100))
        read.assert_called_once()
        self.assertEqual(read.call_args.args[0], ['xdotool', 'getmouselocation', '--shell'])
        self.assertLessEqual(read.call_args.kwargs['timeout'], 5)
        move.assert_not_called()

    def test_non_sync_move_has_bounded_xy_readback_until_actual_target(self):
        owner = self.owner()
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.subprocess, 'check_output', side_effect=['X=1\nY=2', 'X=50\nY=50', 'X=80\nY=80']) as read, \
             mock.patch.object(ui.subprocess, 'run') as move, mock.patch.object(ui.time, 'sleep'):
            ui._park_pointer((80, 80), owner, (100, 100))
        self.assertEqual(read.call_count, 3)
        move.assert_called_once()
        self.assertEqual(move.call_args.args[0], ['xdotool', 'mousemove', '80', '80'])
        self.assertNotIn('--sync', move.call_args.args[0])
        self.assertLessEqual(move.call_args.kwargs['timeout'], 5)

    def test_malformed_unavailable_and_out_of_bounds_xy_never_move(self):
        owner = self.owner()
        for value in ('X=80', 'X=no\nY=80', 'X=-1\nY=80', 'X=100\nY=80',
                      'X=80\nX=80\nY=80', 'z' * 1025):
            with self.subTest(value=value[:20]), mock.patch.object(ui, 'identity', return_value=owner), \
                 mock.patch.object(ui.subprocess, 'check_output', return_value=value), \
                 mock.patch.object(ui.subprocess, 'run') as move:
                with self.assertRaisesRegex(EvidenceError, 'pointer_parking_invalid_coordinates'):
                    ui._park_pointer((80, 80), owner, (100, 100))
                move.assert_not_called()

    def test_absolute_five_second_budget_is_shared_by_read_move_and_readback(self):
        owner, clock, budgets = self.owner(), [0.], []
        original = None
        def read(command, **kwargs):
            nonlocal original
            budget = kwargs['timeout']; budgets.append(budget)
            if budget < 1:
                clock[0] += budget
                original = subprocess.TimeoutExpired(command, budget)
                raise original
            clock[0] += 1
            return 'X=1\nY=2'
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.time, 'monotonic', side_effect=lambda: clock[0]), \
             mock.patch.object(ui.time, 'sleep', side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)), \
             mock.patch.object(ui.subprocess, 'check_output', side_effect=read), \
             mock.patch.object(ui.subprocess, 'run', side_effect=lambda *a, **k: clock.__setitem__(0, clock[0] + 1)):
            with self.assertRaises(subprocess.TimeoutExpired) as failure:
                ui._park_pointer((80, 80), owner, (100, 100))
        self.assertIs(failure.exception, original)
        self.assertAlmostEqual(clock[0], 5)
        self.assertTrue(all(a > b for a, b in zip(budgets, budgets[1:])))

    def test_late_read_even_at_target_cannot_succeed_after_deadline(self):
        owner, clock = self.owner(), [0.]
        def read(*args, **kwargs):
            clock[0] = 5
            return 'X=80\nY=80'
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.time, 'monotonic', side_effect=lambda: clock[0]), \
             mock.patch.object(ui.subprocess, 'check_output', side_effect=read), \
             mock.patch.object(ui.subprocess, 'run') as move:
            with self.assertRaisesRegex(EvidenceError, 'pointer_parking_deadline'):
                ui._park_pointer((80, 80), owner, (100, 100))
            move.assert_not_called()

    def test_original_read_error_and_changed_generation_abort_without_move(self):
        owner = self.owner()
        original = OSError('do-not-export-private-read-text')
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.subprocess, 'check_output', side_effect=original), \
             mock.patch.object(ui.subprocess, 'run') as move:
            with self.assertRaises(OSError) as failure:
                ui._park_pointer((80, 80), owner, (100, 100))
            self.assertIs(failure.exception, original); move.assert_not_called()
        with mock.patch.object(ui, 'identity', side_effect=[owner, {**owner, 'startTicks': 999}]), \
             mock.patch.object(ui.subprocess, 'check_output', return_value='X=80\nY=80'), \
             mock.patch.object(ui.subprocess, 'run') as move:
            with self.assertRaisesRegex(EvidenceError, 'pointer_parking_owner_changed'):
                ui._park_pointer((80, 80), owner, (100, 100))
            move.assert_not_called()


class VisibilityDiagnosticsTests(unittest.TestCase):
    INFO = ('Absolute upper-left X: 10\nAbsolute upper-left Y: 10\nWidth: 40\nHeight: 40\n'
            'Map State: IsViewable\nWindow title: never retain this text')

    def guard(self, root, mode, *, read_fault=None, exit_fault=None, close_fault=False,
              diagnostic=True, parking_fault=None):
        from PIL import Image
        first, other = (50, 60, 70), (200, 100, 90)
        native = Image.new('RGB', (16, 16), first)
        after = Image.new('RGB', (16, 16), other if mode == 'changing' else first)
        composite = Image.new('RGB', (100, 100), other if mode == 'mismatch' else first)
        draw = mock.Mock()
        draw.pixels.side_effect = [value for _ in range(12) for value in (native, after)]
        client = mock.Mock(width=100, height=100)
        client.frame.return_value = (100, 100, composite.tobytes('raw', 'BGRX'))
        if read_fault:
            client.frame.side_effect = read_fault
        if close_fault:
            draw.close.side_effect = OSError('do-not-export-drawable-close-text')
            client.close.side_effect = RuntimeError('do-not-export-vnc-close-text')
        owner = {'pid': 123, 'startTicks': 456, 'uid': os.getuid(), 'device': 7,
                 'inode': 8, 'executable': '/not-executed/putgd-ui'}
        state = {'geometry': [10, 10, 40, 40], 'mapped': True, 'generationMatches': True,
                 'ownerPidMatches': True, 'activeIsTarget': True, 'probeMs': 1}
        diagnostics = {} if diagnostic else None
        path = root / 'owned.png'
        with mock.patch.dict(sys.modules, {'rfb': SimpleNamespace(RFB=lambda port: client)}), \
             mock.patch.object(ui, 'Drawable', return_value=draw), \
             mock.patch.object(ui.subprocess, 'run'), \
             mock.patch.object(ui.subprocess, 'check_output', return_value=self.INFO), \
             mock.patch.object(ui, '_park_pointer', side_effect=parking_fault), \
             mock.patch.object(ui, '_visibility_target', side_effect=[state, exit_fault or state]) as metadata, \
             mock.patch.object(ui.time, 'sleep'):
            try:
                return ui.save_visible_window('0x2', owner, path, 5900, diagnostics_out=diagnostics)
            finally:
                self.diagnostics, self.draw, self.client, self.metadata = diagnostics, draw, client, metadata

    def numeric_only(self, value):
        if isinstance(value, dict):
            for item in value.values(): self.numeric_only(item)
        elif isinstance(value, list):
            for item in value: self.numeric_only(item)
        else:
            self.assertTrue(value is None or type(value) in (str, int, float, bool), type(value))
        encoded = json.dumps(value)
        for forbidden in ('never retain this text', 'do-not-export', 'sha256', 'base64'):
            self.assertNotIn(forbidden, encoded)

    def test_changing_native_or_stable_composite_mismatch_never_saves_any_attempt(self):
        for mode in ('changing', 'mismatch'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with self.assertRaisesRegex(EvidenceError, 'owned_ui_not_proven_visible_on_physical_wall'):
                    self.guard(root, mode)
                d = self.diagnostics
                self.assertEqual(d['attemptCount'], 12)
                self.assertEqual(d['completedComparisonCount'], 12)
                self.assertEqual(d['exactMatchCount'], 0)
                self.assertEqual(d['nativeChangedCount'], 12 if mode == 'changing' else 0)
                self.assertEqual(d['stableNativeCompositeMismatchCount'], 12 if mode == 'mismatch' else 0)
                self.assertEqual(list(root.iterdir()), [])
                self.assertEqual(self.draw.pixels.call_count, 24)
                self.assertEqual(self.client.frame.call_count, 12)
                self.assertEqual(self.metadata.call_count, 2)
                self.numeric_only(d)

    def test_exact_match_saves_only_same_owned_crop_and_reports_one_comparison(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            proof = self.guard(root, 'exact')
            with Image.open(root / 'owned.png') as image:
                self.assertEqual(image.size, (16, 16))
                self.assertEqual(image.tobytes(), bytes((50, 60, 70)) * 256)
            self.assertTrue(proof['equalsStableOwnedDrawable'])
            self.assertEqual(self.diagnostics['exactMatchCount'], 1)
            self.assertEqual(self.diagnostics['attemptCount'], 1)
            self.assertTrue(self.diagnostics['metadataComplete'])
            self.numeric_only(self.diagnostics)

    def test_read_fault_retains_original_no_pixels_and_incomplete_attempt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = OSError('do-not-export-read-payload')
            with self.assertRaises(OSError) as failure:
                self.guard(root, 'exact', read_fault=original)
            self.assertIs(failure.exception, original)
            self.assertEqual(self.diagnostics['errorStage'], 'vnc-frame')
            self.assertEqual(self.diagnostics['attemptCount'], 1)
            self.assertEqual(self.diagnostics['completedComparisonCount'], 0)
            self.assertEqual(list(root.iterdir()), [])
            self.draw.close.assert_called_once(); self.client.close.assert_called_once()
            self.numeric_only(self.diagnostics)

    def test_pointer_read_or_timeout_fault_is_labeled_before_pixels_and_saves_nothing(self):
        for original in (OSError('do-not-export-pointer-read-text'),
                         subprocess.TimeoutExpired(['xdotool', 'getmouselocation', '--shell'], 1),
                         EvidenceError('pointer_parking_invalid_coordinates')):
            with self.subTest(error=type(original).__name__), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                with self.assertRaises(type(original)) as failure:
                    self.guard(root, 'exact', parking_fault=original)
                self.assertIs(failure.exception, original)
                self.assertEqual(self.diagnostics['errorStage'], 'pointer-parking')
                self.assertEqual(self.diagnostics['attemptCount'], 0)
                self.draw.pixels.assert_not_called(); self.client.frame.assert_not_called()
                self.draw.close.assert_called_once(); self.client.close.assert_called_once()
                self.assertEqual(list(root.iterdir()), [])
                self.numeric_only(self.diagnostics)

    def test_exit_metadata_fault_cannot_mask_refusal_or_be_treated_as_expected_only(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(EvidenceError, 'owned_ui_not_proven_visible_on_physical_wall') as failure:
                self.guard(Path(directory), 'mismatch', exit_fault=OSError('do-not-export-metadata-text'))
            self.assertTrue(failure.exception.visibility_abort)
            self.assertIn('visibility_exit_metadata: OSError', failure.exception.__notes__)
            self.assertFalse(self.diagnostics['metadataComplete'])
            self.numeric_only(self.diagnostics)

    def test_both_close_faults_preserve_read_failure_and_close_every_resource(self):
        with tempfile.TemporaryDirectory() as directory:
            original = OSError('do-not-export-read-text')
            with self.assertRaises(OSError) as failure:
                self.guard(Path(directory), 'exact', read_fault=original, close_fault=True)
            self.assertIs(failure.exception, original)
            self.assertTrue(original.visibility_abort)
            self.draw.close.assert_called_once(); self.client.close.assert_called_once()
            self.assertEqual(len(self.diagnostics['cleanupErrors']), 2)
            self.numeric_only(self.diagnostics)

    def test_exact_match_exit_probe_fault_is_not_retried_and_cannot_save(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = OSError('do-not-export-exit-query-text')
            with self.assertRaises(OSError) as failure:
                self.guard(root, 'exact', exit_fault=original)
            self.assertIs(failure.exception, original)
            self.assertEqual(self.metadata.call_count, 2)
            self.assertEqual(list(root.iterdir()), [])
            self.draw.close.assert_called_once(); self.client.close.assert_called_once()
            self.assertFalse(self.diagnostics['metadataComplete'])

    def test_default_path_has_no_optional_metadata_queries(self):
        with tempfile.TemporaryDirectory() as directory:
            self.guard(Path(directory), 'exact', diagnostic=False)
            self.metadata.assert_not_called()
            self.assertEqual(self.draw.pixels.call_count, 2)
            self.client.frame.assert_called_once()

    def test_target_metadata_exports_only_owned_numeric_fields_and_refuses_wrong_owner(self):
        owner = {'pid': 123, 'startTicks': 456, 'uid': os.getuid(), 'device': 7,
                 'inode': 8, 'executable': '/not-executed/putgd-ui'}
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.subprocess, 'check_output', side_effect=['_NET_WM_PID(CARDINAL) = 123',
                 '_NET_ACTIVE_WINDOW(WINDOW): window id # 0x99']):
            value = ui._visibility_target('0x2', owner, self.INFO)
        self.assertFalse(value['activeIsTarget'])
        self.numeric_only(value)
        with mock.patch.object(ui, 'identity', return_value=owner), \
             mock.patch.object(ui.subprocess, 'check_output', return_value='_NET_WM_PID(CARDINAL) = 999'):
            with self.assertRaisesRegex(EvidenceError, 'visibility_window_owner_changed'):
                ui._visibility_target('0x2', owner, self.INFO)


class FramePrivacyTests(unittest.TestCase):
    def cover_readiness(self, root, replies):
        run = campaign.Campaign.__new__(campaign.Campaign)
        run.root, run.cover, run.result = root, None, {}
        run.args, run.event, run.persist = SimpleNamespace(vnc_port=5900), mock.Mock(), mock.Mock()
        process = mock.Mock()
        process.poll.return_value = None
        with mock.patch.object(campaign.subprocess, 'Popen', return_value=process), \
             mock.patch.object(campaign, 'read_line', return_value={'ok': True, 'state': 'covered'}), \
             mock.patch.object(campaign.subprocess, 'run', side_effect=replies) as precheck, \
             mock.patch.object(campaign.time, 'sleep'), \
             mock.patch.object(frames, 'save_rgb') as saver:
            try:
                run.static_cover()
            finally:
                saver.assert_not_called()
                self.readiness_run, self.readiness_precheck = run, precheck
        return run

    def test_cover_placement_waits_for_later_visible_frame_without_saving_any_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self.cover_readiness(Path(directory), [
                SimpleNamespace(returncode=3, stdout='{"ok":false,"fraction":0.0}'),
                SimpleNamespace(returncode=0, stdout='{"ok":true,"fraction":1.0}')])
            self.assertEqual(len(run.result['staticCover']['visibilityAttempts']), 2)
            self.assertEqual(run.result['staticCover']['visibilityAttempts'][0]['precheck']['fraction'], 0)
            for call in self.readiness_precheck.call_args_list:
                self.assertEqual(call.args[0][-2:], ['--min-fraction', '0.98'])
                self.assertLessEqual(call.kwargs['timeout'], 5)

    def test_cover_never_visible_has_bounded_retries_and_retains_each_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(EvidenceError, 'static_cover_not_visible_on_wall'):
                self.cover_readiness(Path(directory), [
                    SimpleNamespace(returncode=3, stdout='{"ok":false,"fraction":0.95}') for _ in range(10)])
            self.assertEqual(self.readiness_precheck.call_count, 10)
            self.assertEqual(len(self.readiness_run.result['staticCover']['visibilityAttempts']), 10)
            self.assertEqual(self.readiness_run.persist.call_count, 10)

    def test_99_percent_cover_with_ten_thousand_notification_pixels_never_saves(self):
        raw = frame(1000, 1000)
        notification(raw, 1000, 100, 100, 100, 100)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
            frames.save_cover('not-written.png', 1000, 1000, bytes(raw), saver=saver)
        saver.assert_not_called()

    def test_small_panel_is_not_a_tolerated_percentage(self):
        raw = frame(100, 100)
        notification(raw, 100, 0, 0, 100, 1)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
            frames.save_cover('not-written.png', 100, 100, bytes(raw), saver=saver)
        saver.assert_not_called()

    def test_cover_encoder_receives_only_exact_verified_rgb(self):
        saver = mock.Mock()
        frames.save_cover('cover.png', 80, 60, bytes(frame(80, 60)), saver=saver)
        saver.assert_called_once_with('cover.png', 80, 60, frames.BASE * (80 * 60))

    def selector(self):
        raw = frame(100, 100)
        rgba = bytearray(100 * 100 * 4)
        expected = bytearray(frames.BASE * (100 * 100))
        for y, value in ((20, 0), (21, 255)):
            for x in range(10, 90):
                index = y * 100 + x
                rgba[index * 4:index * 4 + 4] = bytes((value, value, value, 255))
                raw[index * 4:index * 4 + 4] = bytes((value, value, value, 0))
                expected[index * 3:index * 3 + 3] = bytes((value, value, value))
        return raw, [{'x': 0, 'y': 0, 'width': 100, 'height': 100, 'rgba': bytes(rgba)}], bytes(expected)

    def test_capture_encoder_never_receives_unattributed_notification_or_panel_bytes(self):
        raw, overlays, expected = self.selector()
        notification(raw, 100, 0, 0, 100, 1)
        notification(raw, 100, 60, 60, 10, 10)
        saver = mock.Mock()
        proof = frames.save_selector('selector.png', 100, 100, bytes(raw), overlays, None, saver=saver)
        saver.assert_called_once_with('selector.png', 100, 100, expected)
        self.assertTrue(proof['allOtherPixelsReplacedWithSyntheticBackground'])
        self.assertFalse(proof['rawFramebufferRetained'])

    def test_notification_obscuring_verified_selector_refuses_save(self):
        raw, overlays, _ = self.selector()
        notification(raw, 100, 30, 20, 10, 2)
        saver = mock.Mock()
        with self.assertRaisesRegex(EvidenceError, 'selector_pixels_occluded'):
            frames.save_selector('not-written.png', 100, 100, bytes(raw), overlays, None, saver=saver)
        saver.assert_not_called()

    def test_capture_failure_path_does_not_persist_a_99_percent_cover(self):
        raw = frame(1000, 1000)
        notification(raw, 1000, 0, 0, 1000, 10)
        client = SimpleNamespace(frame=lambda: (1000, 1000, bytes(raw)), close=lambda: None)
        with tempfile.TemporaryDirectory() as directory:
            run = campaign.Campaign.__new__(campaign.Campaign)
            run.root = Path(directory)
            run.family = run.resident = None
            run.cover = mock.Mock()
            run.cover.poll.return_value = None
            run.args = SimpleNamespace(vnc_port=5900)
            with mock.patch.dict(sys.modules, {'rfb': SimpleNamespace(RFB=lambda port: client)}), \
                 mock.patch.object(frames, 'save_rgb') as saver:
                with self.assertRaisesRegex(EvidenceError, 'unattributed_pixels'):
                    run.visual('failed-workload')
                saver.assert_not_called()
            self.assertEqual(list(run.root.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
