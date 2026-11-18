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
from tools.lab.surface_driver.fixtures import recordings
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
    def test_review_fixture_forces_owned_chooser_before_recording(self):
        scope = Mock()
        recordings.copy_recording(scope)
        self.assertEqual([call.args[0] for call in scope.ctx.fixture.call_args_list],
                         ['file-dialog-fallback', 'recording'])

    def test_review_copy_observes_exact_owned_chooser_result(self):
        ctx = Mock()
        ctx.profile = {'session': 'wayland'}
        ctx.shell.return_value = {'returncode': 0}
        ctx.wait.side_effect = lambda predicate, *_args, **_kwargs: predicate()
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        choice = {'open': False, 'last': {'status': 'chosen', 'backend': 'fallback', 'path': path}}
        ctx.call.return_value = {'fileDialog': choice}
        with patch.object(dialogs, '_ready', return_value={'title': 'Save a copy'}) as ready, \
             patch.object(dialogs, '_enter_review_path') as enter:
            dialogs.choose_review_copy(ctx, 'current', path, output_dir='/tmp/private/out')
        self.assertEqual(ready.call_args.args[2:], ('fallback', 'save'))
        enter.assert_called_once_with(ctx, dialogs.PurePosixPath(path), 'Save a copy')

    def test_foreign_portal_is_refused_without_review_input(self):
        ctx = Mock()
        ctx.shell.return_value = {'returncode': 0}
        ctx.expect.side_effect = self.assertTrue
        ctx.wait.return_value = {'open': True, 'backend': 'portal'}
        ctx.blocked.side_effect = RecipeBlocked('foreign chooser')
        ctx.call.return_value = {'fileDialog': {'open': True, 'backend': 'portal'}}
        with self.assertRaisesRegex(RecipeBlocked, 'foreign chooser'):
            dialogs.choose_review_copy(ctx, 'current', '/tmp/private/out/a.mp4', output_dir='/tmp/private/out')
        ctx.keys.assert_not_called()
        ctx.type_text.assert_not_called()
        ctx.click.assert_not_called()

    def test_copy_path_requires_clipboard_to_change(self):
        ctx = Mock()
        ctx.wait.side_effect = lambda predicate, *_args: self.assertTrue(predicate())
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        ctx.clipboard_text.side_effect = [None, path]
        with patch.object(review, '_saved_copy_actions', return_value={'copyPath', 'showInFolder', 'dismissSavedCopy'}), \
             patch.object(review, '_drive') as drive:
            review._check_saved_actions(ctx, 'current', path)
        self.assertEqual([call.args[2] for call in drive.call_args_list], ['copyPath', 'dismissSavedCopy'])

    def test_copy_path_refuses_preexisting_clipboard_value(self):
        ctx = Mock()
        ctx.wait.side_effect = lambda predicate, *_args: self.assertTrue(predicate())
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        ctx.clipboard_text.return_value = path
        with patch.object(review, '_saved_copy_actions', return_value={'copyPath', 'showInFolder', 'dismissSavedCopy'}), \
             patch.object(review, '_drive') as drive:
            with self.assertRaises(AssertionError):
                review._check_saved_actions(ctx, 'current', path)
        drive.assert_not_called()

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
