"""The driver clears what is in the way, and an app failure stays visible."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.recipes import outcomes

ROWS = [{'id': 1, 'title': 'desktop icons', 'pid': 10, 'process': 'nemo-desktop', 'owned': False, 'stack': 1},
        {'id': 2, 'title': 'pattern', 'pid': 20, 'process': 'python3', 'owned': True, 'stack': 2},
        {'id': 3, 'title': 'old window', 'pid': 30, 'process': 'kate', 'owned': False, 'stack': 3},
        {'id': 4, 'title': 'banner', 'pid': 40, 'process': 'plasmashell', 'owned': False, 'stack': 4}]


class ClearScreen(unittest.TestCase):
    def clear(self, **query):
        with patch.object(probe, 'close_notifications', return_value=7) as banners, \
             patch.object(probe, 'layout', return_value={'windows': ROWS}), \
             patch.object(probe, 'stop_window') as stop:
            reply = probe.clear_screen(dict(query, shell=['plasmashell']))
        return reply, banners, [call.args[0]['id'] for call in stop.call_args_list]

    def test_the_obscuring_window_goes_and_nothing_else(self):
        reply, banners, stopped = self.clear(obscuring=3)
        banners.assert_called_once()
        self.assertEqual(stopped, [3])
        self.assertEqual(reply['stopped'], [{'title': 'old window', 'process': 'kate'}])

    def test_windows_over_the_pattern_go_but_not_those_below_it(self):
        self.assertEqual(self.clear(above=2)[2], [3])

    def test_the_shell_and_this_runs_windows_always_stay(self):
        self.assertEqual(self.clear(obscuring=4)[2], [])
        self.assertEqual(self.clear(obscuring=2)[2], [])

    def test_a_banner_being_clicked_can_stay(self):
        _, banners, stopped = self.clear(obscuring=3, notifications=False)
        banners.assert_not_called()
        self.assertEqual(stopped, [3])

    def test_banners_close_back_from_the_newest_id(self):
        with patch.object(probe, 'run', return_value='(uint32 70,)') as notify, patch.object(probe.subprocess, 'run') as close:
            self.assertEqual(probe.close_notifications(), 70)
        self.assertIn('org.freedesktop.Notifications.Notify', notify.call_args.args)
        identities = [call.args[0][-1] for call in close.call_args_list]
        self.assertEqual(identities, [str(value) for value in range(70, 6, -1)])


class Dispatch(unittest.TestCase):
    def test_both_probe_entry_points_know_clear_screen(self):
        root = Path(__file__).resolve().parents[2]/'tools/lab/surface_driver'
        self.assertIn("'clear_screen':'clear_screen'", (root/'target.py').read_text())
        self.assertIn("'clear_screen': clear_screen", (root/'probe.py').read_text())


class Guard(unittest.TestCase):
    def context(self, replies):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        ctx = DriverContext.__new__(DriverContext)
        ctx.profile, ctx._popup = {'session': 'x11'}, None
        ctx._input_poisoned, ctx._press_open, ctx._current_window = False, False, 'history'
        ctx.geometry = lambda role: {'x': 0, 'y': 0, 'width': 10, 'height': 10, 'pid': 1, 'window_id': 2}
        calls = []
        def fake(name, query):
            calls.append((name, query))
            return replies.pop(0) if name == 'foreground' else {'stopped': [{'title': 'old window', 'process': 'kate'}]}
        ctx._probe, ctx.notes = fake, []
        ctx.note = ctx.notes.append
        ctx._check_time = lambda: None
        ctx.shell_failure = lambda full=False: None
        return ctx, calls

    def test_input_continues_once_the_cover_is_gone(self):
        ctx, calls = self.context([{'ok': False, 'obscuring_window': 'kate', 'obscuring_window_id': 3}, {'ok': True}])
        with patch('tools.lab.surface_driver.context.time.sleep'):
            self.assertTrue(ctx._guard()['ok'])
        self.assertEqual([query['obscuring'] for name, query in calls if name == 'clear_screen'], [3])
        self.assertIn('cleared screen: banners closed; windows closed: old window (kate)', ctx.notes)

    def test_a_cover_that_stays_still_fails_and_is_named(self):
        ctx, _ = self.context([{'ok': False, 'obscuring_window': 'stuck', 'obscuring_window_id': 3} for _ in range(4)])
        with patch('tools.lab.surface_driver.context.time.sleep'), self.assertRaisesRegex(RecipeFailed, 'stuck'):
            ctx._guard()


class AppErrorBanner(unittest.TestCase):
    def context(self, rows):
        ctx = DriverContext.__new__(DriverContext)
        ctx.notifications = lambda: rows
        def expect(condition, message):
            if not condition:
                raise RecipeFailed(message)
        ctx.expect = expect
        return ctx

    def test_a_failure_banner_fails_the_cell_and_is_named(self):
        ctx = self.context([{'summary': 'put.gd', 'body': 'Uploaded', 'hints': {'urgency': 1}},
                            {'summary': 'put.gd - file manager menu', 'body': 'Cannot configure', 'hints': {'urgency': 2}}])
        with self.assertRaisesRegex(RecipeFailed, 'file manager menu: Cannot configure'):
            outcomes.no_app_error_banner(ctx)

    def test_normal_banners_pass(self):
        outcomes.no_app_error_banner(self.context([{'summary': 'put.gd', 'body': 'Uploaded', 'hints': {'urgency': 1}}]))


if __name__ == '__main__':
    unittest.main()
