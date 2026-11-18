#!/usr/bin/env python3
"""Review recipe contracts: kept-time arithmetic and the target-side verifier. Not acceptance evidence."""
from __future__ import annotations

from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.fixtures import clipboard, recordings
from tools.lab.surface_driver.recipes import dialogs, review


class KeptSpans(unittest.TestCase):
    D = 4_500_000

    def test_table(self):
        cut = lambda a, b: {'start': a, 'end': b}
        cases = [
            ({'trimStart': 0, 'trimEnd': None, 'cuts': []}, [[0, self.D]]),
            ({'trimStart': 100, 'trimEnd': 900, 'cuts': []}, [[100, 900]]),
            ({'trimStart': 0, 'trimEnd': None, 'cuts': [cut(1_000_000, 2_000_000)]}, [[0, 1_000_000], [2_000_000, self.D]]),
            # trim and two cuts, one given out of order and one overlapping the trim start
            ({'trimStart': 500_000, 'trimEnd': 4_000_000, 'cuts': [cut(3_000_000, 3_500_000), cut(0, 700_000)]},
             [[700_000, 3_000_000], [3_500_000, 4_000_000]]),
            ({'trimStart': 0, 'trimEnd': 1000, 'cuts': [cut(0, 1000)]}, []),
        ]
        for edit, expected in cases:
            with self.subTest(edit=edit):
                self.assertEqual(review.kept_spans(edit, self.D), expected)


class Verifier(unittest.TestCase):
    def test_target_script_compiles(self):
        compile(review._MEDIA_CHECK, 'review-media-check', 'exec')

    def test_off_grid_cover_time_snaps_to_nearest_frame(self):
        self.assertEqual(review._nominal_frame_time(1_912_000, 30), 1_900_000)
        self.assertEqual(review._nominal_frame_time(1_912_000, 30, step=1), 1_933_333)

    def test_saved_copy_actions_ignore_failure_actions(self):
        state = {'statusFacts': [{'actions': [{'drive': {'action': 'retryExport'}}, {'drive': {'action': 'dismissStatus'}}]},
                                 {'actions': [{'drive': {'action': 'copyPath'}}, {'drive': {'action': 'dismissSavedCopy'}}]}]}
        self.assertEqual(review._saved_copy_actions(state), {'copyPath', 'dismissSavedCopy'})
        state['statusFacts'].pop()
        self.assertEqual(review._saved_copy_actions(state), set())


class ChooserSave(unittest.TestCase):
    def test_profile_can_select_owned_chooser_before_recording(self):
        scope = Mock()
        scope.ctx.profile = {'session': 'wayland', 'recording': {'review_copy_chooser': 'fallback'}}
        scope.ctx.fixture.return_value = recordings.Fixture(id='current')
        fixture = recordings.copy_recording(scope)
        self.assertTrue(callable(fixture['stage_save_path']))
        self.assertEqual([call.args[0] for call in scope.ctx.fixture.call_args_list],
                         ['file-dialog-fallback', 'recording'])

    def test_native_chooser_paste_observes_exact_focused_entry(self):
        ctx = Mock()
        ctx.geometry.return_value = {'pid': 937}
        ctx.wait.side_effect = lambda predicate, *_args: self.assertTrue(predicate())
        path = '/tmp/private/out/retry.mp4'
        entry = {'role': 'text', 'window': 'Save a copy'}
        ctx.accessible.side_effect = [[entry], [dict(entry, text=path)]]
        stage = Mock()
        dialogs._paste_review_path(ctx, path, {'title': 'Save a copy'}, stage)
        stage.assert_called_once_with(path)
        self.assertEqual([call.args[0] for call in ctx.keys.call_args_list], ['Ctrl+A', 'Ctrl+V', 'Return'])

    def test_chooser_clipboard_refuses_unowned_paths_or_missing_snapshot(self):
        scope = Mock(root='/tmp/private')
        scope.ctx.profile = {'session': 'wayland'}
        for path, ready in (('/tmp/peer/out/retry.mp4', True), ('/tmp/private/out/retry.mp4', False)):
            with self.subTest(path=path, ready=ready):
                scope.ctx.target.clipboard_report = {'ready': ready}
                with self.assertRaises(RecipeBlocked):
                    clipboard.stage_chooser_path(scope, path)
        scope.write.assert_not_called()

    def test_review_fixture_selects_exact_path_through_native_chooser(self):
        ctx = Mock()
        ctx.wait.side_effect = lambda predicate, *_args, **_kwargs: predicate()
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        choice = {'open': False, 'last': {'status': 'chosen', 'path': path}}
        states = iter(({'open': True, 'backend': 'portal'}, choice, choice))

        def call(method, params):
            self.assertEqual(method, 'recording.review')
            self.assertEqual(params['id'], 'current')
            if params['action'] == 'inspect':
                return {'fileDialog': next(states)}
            self.assertEqual(params['action'], 'saveCopyDialog')

        ctx.call.side_effect = call
        with patch.object(dialogs, '_ready') as ready, patch.object(dialogs, '_input') as enter:
            dialogs.choose_review_copy(ctx, 'current', path)
        self.assertEqual(ready.call_args.args[2:], ('portal', 'save'))
        self.assertEqual(enter.call_args.args[1:4], ('portal', path, 'save'))
        ctx.view.assert_called_once_with('recording.review', {'id': 'current', 'action': 'inspect'})

    def test_review_chooser_rebinds_after_portal_timeout_before_input(self):
        ctx = Mock()
        ctx.wait.side_effect = lambda predicate, *_args, **_kwargs: predicate()
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        state = {'open': True, 'backend': 'portal'}

        def call(method, params):
            self.assertEqual(method, 'recording.review')
            if params['action'] == 'inspect':
                return {'fileDialog': state.copy()}

        def ready(_ctx, _inspect, backend, _mode, **_options):
            if backend == 'portal':
                state.update(backend='fallback', fallbackReason='NoReply')
                raise dialogs.RecipeBlocked('portal changed backend')

        def enter(_ctx, backend, target, _mode, _inspect):
            self.assertEqual((backend, target), ('fallback', path))
            state.update(open=False, last={'status': 'chosen', 'path': path})

        ctx.call.side_effect = call
        with patch.object(dialogs, '_ready', side_effect=ready) as prepare, \
             patch.object(dialogs, '_input', side_effect=enter) as choose:
            dialogs.choose_review_copy(ctx, 'current', path)
        self.assertEqual([call.args[2] for call in prepare.call_args_list], ['portal', 'fallback'])
        choose.assert_called_once()

    def test_export_fault_restores_owned_working_file_on_input_error(self):
        scope = Mock()
        scope.ctx.fixture.return_value = recordings.Fixture(id='current', output_dir='/tmp/private/out')
        restore = Mock()
        with patch.object(recordings, '_replace_working', return_value=restore), \
             patch.object(dialogs, 'choose_review_copy', side_effect=RuntimeError('chooser failed')):
            with self.assertRaisesRegex(RuntimeError, 'chooser failed'):
                recordings.export_failure(scope)
        restore.assert_called_once_with()


if __name__ == '__main__':
    unittest.main()
