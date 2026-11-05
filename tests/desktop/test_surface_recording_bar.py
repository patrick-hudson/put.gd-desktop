"""Captionless recording layer guards. Mocks are not desktop evidence."""
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.api import RecipeFailed


class RecordingBarPlacement(unittest.TestCase):
    def setUp(self):
        self.compositor = {'kind': 'kwin-script', 'recording_bar_before': ['old']}
        self.bar = dict(id='bar', pid=42, title='', wm_class='putgd-record', surface_class='KWin::LayerShellV1Window',
                        frame=[250, 620, 780, 52], content=[250, 620, 780, 52], normal=True, popup=False)
        self.windows = [dict(self.bar, id='strip', frame=[317, 237, 646, 3], content=[317, 237, 646, 3]), self.bar]
        for name, value in [('owned', True), ('executable', ['putgd-record'])]:
            p = patch.object(probe, name, return_value=value)
            p.start(); self.addCleanup(p.stop)

    def place(self, *, title='put.gd recording controls', pid=42, rect=(0, 0, 780, 52)):
        placement = probe.WaylandPlacement(self.compositor)
        placement._windows = self.windows
        return placement.toplevel(pid, title, rect)

    def test_exact_new_owned_layer_uses_compositor_client_origin(self):
        self.assertEqual(self.place(), dict(dx=250, dy=620, window_id='bar', frame=self.bar['frame']))

    def test_no_baseline_old_foreign_wrong_size_or_surface_class_refuse(self):
        for changes in ({'id': 'old'}, {'pid': 43}, {'title': 'foreign'}, {'normal': False},
                        {'popup': True}, {'wm_class': 'foreign'}, {'surface_class': 'KWin::XdgToplevelWindow'},
                        {'content': [250, 620, 779, 52]}, {'frame': [250, 620, 779, 52]}):
            with self.subTest(changes=changes):
                self.windows = [dict(self.bar, **changes)]
                with self.assertRaisesRegex(RuntimeError, '0 matches'):
                    self.place()
        self.windows = [self.bar]
        self.compositor.pop('recording_bar_before')
        with self.assertRaisesRegex(RuntimeError, '0 matches'):
            self.place()

    def test_unowned_other_executable_and_selector_cover_do_not_get_fallback(self):
        for name, value in [('owned', False), ('executable', ['putgd-ui'])]:
            with patch.object(probe, name, return_value=value), self.assertRaisesRegex(RuntimeError, '0 matches'):
                self.place()
        with self.assertRaisesRegex(RuntimeError, '0 matches'):
            self.place(title='Recording region')

    def test_duplicate_same_pid_same_size_layers_remain_ambiguous(self):
        self.windows.append(dict(self.bar, id='duplicate'))
        with self.assertRaisesRegex(RuntimeError, '2 matches'):
            self.place()

    def test_refusal_diagnostic_includes_bar_after_twelve_bounds_and_dim_surfaces(self):
        self.windows = [dict(self.windows[0], id='strip%d' % i) for i in range(12)] + [self.bar]
        self.compositor.pop('recording_bar_before')
        with self.assertRaisesRegex(RuntimeError, r'\[250, 620, 780, 52\] type None \(KWin::LayerShellV1Window\)'):
            self.place()

    def test_foreground_requires_exact_surface_even_for_same_pid_cover(self):
        query = dict(compositor=self.compositor, geometry=dict(window_id='bar', pid=42), point=[300, 640])
        with patch.object(probe, 'compositor_pick', return_value={'window': self.bar}):
            self.assertTrue(probe.witness_foreground(query)['ok'])
        for cover in (dict(self.bar, id='cover'), dict(self.bar, id='foreign', pid=43), None):
            with patch.object(probe, 'compositor_pick', return_value={'window': cover}):
                self.assertFalse(probe.witness_foreground(query)['ok'])


class PrepareBaseline(unittest.TestCase):
    def test_snapshot_precedes_prepare_and_replaces_previous_baseline(self):
        ctx = DriverContext.__new__(DriverContext)
        ctx.profile = {'session': 'wayland', 'compositor': {'kind': 'kwin-script'}}
        ctx._check_time = Mock()
        ctx._deadline = time.monotonic() + 60
        ctx._recording_bar_before = ['previous']
        ctx._probe = Mock(return_value={'windows': [{'id': 'old'}]})
        ctx.cli = Mock(side_effect=RuntimeError('stop before sending prepare'))
        with self.assertRaisesRegex(RuntimeError, 'stop before sending prepare'):
            ctx.call('recording.prepare')
        self.assertEqual(ctx._recording_bar_before, ['old'])
        self.assertEqual(ctx.profile['compositor'], {'kind': 'kwin-script'})
        ctx._probe.assert_called_once_with('compositor_windows', {'compositor': {'kind': 'kwin-script'}})

    def test_placement_and_foreground_receive_baseline_without_mutating_profile(self):
        ctx = DriverContext.__new__(DriverContext)
        ctx.profile = {'session': 'wayland', 'compositor': {'kind': 'kwin-script'}}
        ctx._recording_bar_before = ['old']
        ctx._deadline = time.monotonic() + 60
        ctx._check_time = ctx._observed = Mock()
        ctx.target = Mock()
        ctx.target.probe.return_value = {'ok': True, 'result': {}}
        for name in ('accessible', 'geometry', 'control', 'foreground', 'windows'):
            ctx._probe(name, {})
            self.assertEqual(ctx.target.probe.call_args.args[1]['compositor'],
                             {'kind': 'kwin-script', 'recording_bar_before': ['old']})
        self.assertEqual(ctx.profile['compositor'], {'kind': 'kwin-script'})

    def test_failed_snapshot_revokes_old_baseline_and_sends_no_prepare(self):
        ctx = DriverContext.__new__(DriverContext)
        ctx.profile = {'compositor': {'kind': 'kwin-script'}}
        ctx._recording_bar_before = ['previous']
        ctx._check_time = Mock()
        ctx._probe = Mock(side_effect=RuntimeError('witness unavailable'))
        ctx.cli = Mock()
        with self.assertRaisesRegex(RuntimeError, 'witness unavailable'):
            ctx.call('recording.prepare')
        self.assertIsNone(ctx._recording_bar_before)
        ctx.cli.assert_not_called()

    def test_input_receipt_saves_placement_and_refused_foreground(self):
        ctx = DriverContext.__new__(DriverContext)
        ctx.profile = {'compositor': {'kind': 'kwin-script'}}
        ctx._recording_bar_before = ['old']
        ctx._input_poisoned = ctx._press_open = False
        ctx._popup = None
        ctx._check_time = ctx.note = Mock()
        ctx.shell_failure = Mock(return_value=None)
        ctx._probe = Mock(return_value={'ok': False, 'obscuring_window': 'foreign', 'obscuring_window_id': 'cover'})
        geometry = dict(pid=42, window_id='bar', x=250, y=620, width=780, height=52)
        with self.assertRaisesRegex(RecipeFailed, 'obscured by'):
            ctx._guard(window='bar-ready', geometry=geometry, point=[300, 640])
        note = ctx.note.call_args.args[0]
        self.assertIn('"window_id": "bar"', note)
        self.assertIn('"before_prepare": ["old"]', note)
        self.assertIn('"ok": false', note)


if __name__ == '__main__':
    unittest.main()
