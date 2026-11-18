#!/usr/bin/env python3
"""Review recipe contracts: kept-time arithmetic and the target-side verifier. Not acceptance evidence."""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
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


def run_target_python(script, *, timeout):
    source = script.split('\n', 1)[1].rsplit('\n', 2)[0]
    stdout, stderr = StringIO(), StringIO()
    code = 0
    with redirect_stdout(stdout), redirect_stderr(stderr):
        try:
            exec(source, {})
        except SystemExit as error:
            code = error.code
    return {'returncode': code, 'stdout': stdout.getvalue(), 'stderr': stderr.getvalue()}


class TargetObservations(unittest.TestCase):
    def test_clipboard_comparison_returns_only_a_boolean_or_fixed_failure(self):
        expected = '/tmp/private/out/a.mp4'
        unrelated = b'unrelated native clipboard text'
        cases = (
            ('x11', 0, expected.encode(), b'', 'true\n'),
            ('wayland', 0, expected.encode() + b'\n', b'', 'false\n'),
            ('wayland', 0, unrelated, b'', 'false\n'),
            ('wayland', 1, b'', b'Nothing is copied\n', 'false\n'),
            ('x11', 1, b'', b'Error: target STRING not available\n', 'false\n'),
            ('x11', 1, unrelated, unrelated, ''),
        )
        for session, code, payload, err, output in cases:
            with self.subTest(session=session, code=code, output=output):
                ctx = DriverContext.__new__(DriverContext)
                ctx.profile = {'session': session}
                transported = []

                def shell(script, *, timeout):
                    result = run_target_python(script, timeout=timeout)
                    transported.append(result)
                    return result

                ctx.shell = shell
                with patch('subprocess.run', return_value=subprocess.CompletedProcess([], code, payload, err)):
                    if output:
                        self.assertIs(ctx.clipboard_matches(expected), output == 'true\n')
                    else:
                        with self.assertRaises(RecipeBlocked):
                            ctx.clipboard_matches(expected)
                self.assertEqual(transported[0]['stdout'], output)
                self.assertEqual(transported[0]['stderr'], '')

    def test_playback_sample_brackets_inspect_on_the_target(self):
        ctx = Mock(shell=run_target_python)
        result = {'result': {'playheadUs': 123456, 'playing': True, 'unused': 'some other data'}}
        with patch.dict(os.environ, {'XDG_RUNTIME_DIR': '/tmp/private/runtime'}), \
             patch('time.monotonic_ns', side_effect=[90, 100]), \
             patch('subprocess.run', return_value=subprocess.CompletedProcess([], 0, json.dumps(result))) as cli:
            self.assertEqual(review._playback_sample(ctx, 'current'), [123456, 90, 100])
        self.assertEqual(cli.call_args.args[0][-1], 'recording.review')
        self.assertEqual(json.loads(cli.call_args.kwargs['input']), {'id': 'current', 'action': 'inspect'})

    def test_rate_bounds_cover_observation_uncertainty(self):
        first = [0, 0, 20_000_000]
        lower, upper = review._playback_rate(first, [2_000_000, 1_020_000_000, 1_040_000_000])
        self.assertAlmostEqual(lower, 2 / 1.04)
        self.assertEqual(upper, 2)
        lower, upper = review._playback_rate(first, [1_000_000, 1_020_000_000, 1_040_000_000])
        self.assertLess(lower, 1)
        self.assertEqual(upper, 1)


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
        ctx.clipboard_matches.side_effect = [False, True]
        with patch.object(review, '_saved_copy_actions', return_value={'copyPath', 'showInFolder', 'dismissSavedCopy'}), \
             patch.object(review, '_drive') as drive:
            review._check_saved_actions(ctx, 'current', path)
        self.assertEqual([call.args[2] for call in drive.call_args_list], ['copyPath', 'dismissSavedCopy'])
        ctx.clipboard_text.assert_not_called()

    def test_copy_path_refuses_preexisting_clipboard_value(self):
        ctx = Mock()
        ctx.wait.side_effect = lambda predicate, *_args: self.assertTrue(predicate())
        ctx.expect.side_effect = self.assertTrue
        path = '/tmp/private/out/retry.mp4'
        ctx.clipboard_matches.return_value = True
        with patch.object(review, '_saved_copy_actions', return_value={'copyPath', 'showInFolder', 'dismissSavedCopy'}), \
             patch.object(review, '_drive') as drive:
            with self.assertRaises(AssertionError):
                review._check_saved_actions(ctx, 'current', path)
        drive.assert_not_called()

    def test_directory_wait_prevents_unneeded_open_and_requires_exact_folder_for_retry(self):
        path = Path('/tmp/private/out/test.mp4')
        folder = str(path.parent)
        marker = [{'showing': True, 'ancestors': [{'name': 'Files', 'role': 'tree'}]}]
        for load, filename, expected_opens in (('delayed', folder, 1), ('retry', folder, 2), ('retry', '/tmp', 1)):
            with self.subTest(load=load, filename=filename):
                ctx = Mock()
                ctx.shell.return_value = {'returncode': 0}
                ctx.geometry.return_value = {'pid': 937}
                ctx.expect.side_effect = lambda value, why: value or self.fail(why)

                def wait(predicate, why, timeout=10):
                    if 'directory witness' in why and timeout == 5:
                        self.assertFalse(predicate())
                        if load == 'retry':
                            raise RecipeFailed('timeout: ' + why)
                    self.assertTrue(predicate())

                ctx.wait.side_effect = wait
                ctx.accessible.side_effect = [[], marker, marker] if load == 'delayed' else [[], [], marker]
                with patch.object(dialogs, '_type_review_filename'), \
                     patch.object(dialogs, '_review_filename', side_effect=[{'text': folder}, {'text': filename}]):
                    if filename == folder:
                        dialogs._enter_review_path(ctx, path, 'Save a copy')
                    else:
                        with self.assertRaisesRegex(AssertionError, 'exact private folder'):
                            dialogs._enter_review_path(ctx, path, 'Save a copy')
                self.assertEqual(sum(call.args[0] == 'text:Open' for call in ctx.click.call_args_list), expected_opens)

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
