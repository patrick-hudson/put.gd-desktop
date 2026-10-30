"""Geometry and cleanup regressions. Run on the lab host, never as desktop proof."""
import json
import math
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.catalog import Catalog, compare, control_point
from tools.lab.surface_driver.target import Target, input_release


class Geometry(unittest.TestCase):
    def test_equal_aspect_is_not_a_layout_anchor(self):
        with self.assertRaisesRegex(RecipeBlocked, 'explicit layout anchors'):
            control_point({'width': 1440, 'height': 960}, {'rect': [900, 30, 40, 24]},
                          {'x': 0, 'y': 0, 'width': 1320, 'height': 880})

    def test_fixed_control_tracks_window_edges_without_scaling(self):
        control = {'rect': [900, 30, 40, 24], 'anchors': {'x': 'end', 'y': 'start'}}
        self.assertEqual(control_point({'width': 1440, 'height': 960}, control,
                                      {'x': 10, 'y': 20, 'width': 1320, 'height': 880}), (810, 62))

    def test_visible_and_accessible_names_select_same_object(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)
            (p/'01-history.controls.json').write_text(json.dumps({'width': 1440, 'height': 960,
                'controls': [{'name': 'historyListMode', 'text': 'List', 'rect': [900, 30, 40, 24]}]}))
            catalog = Catalog(p, {})
            for selector in ('historyListMode', 'text:List', 'text:Show history as a list'):
                self.assertEqual(catalog.candidates(selector, 'history')[0]['name'], 'historyListMode')

    def test_image_comparison_normalizes_hidpi_and_window_size(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as td:
            a, b = Path(td)/'live.png', Path(td)/'catalog.png'
            Image.new('RGB', (1320, 880), 'white').save(a)
            Image.new('RGB', (2880, 1920), 'white').save(b)
            self.assertEqual(compare(a, b, .025), 0)
            Image.new('RGB', (2880, 1920), 'black').save(b)
            self.assertEqual(compare(a, b, .025), 1)

    def test_qmp_combined_framebuffer_offsets_and_absolute_axes(self):
        target = Target.__new__(Target)
        target.physical = None
        target.info = {'heads': [{}, {}]}
        target.heartbeat = lambda: None
        target._outputs = [{'width': 1280, 'height': 800, 'x': 0, 'y': 0},
                           {'width': 1024, 'height': 768, 'x': 1280, 'y': 0}]
        sent = []
        target.events = lambda *args: sent.append(args)
        target.pointer(320, 200, 1280, 800, head=0)
        self.assertEqual(len(sent[0]), 1)  # no display/head-bound tablet
        self.assertEqual([e['data']['value'] for e in sent[0][0]], [math.ceil(320*32768/2304), 8192])
        target.pointer(100, 100, 1024, 768, head=1)
        self.assertEqual([e['data']['value'] for e in sent[1][0]],
                         [math.ceil(1380*32768/2304), 4096])


class Hygiene(unittest.TestCase):
    def test_preflight_identifies_every_missing_tool_before_launch(self):
        target = Target.__new__(Target)
        target.execute = lambda *a, **k: {'returncode': 0, 'stdout': json.dumps(
            {'session': 'x11', 'missing': ['xdotool', 'xclip', 'python3-pyatspi', 'dbus-monitor']})}
        with self.assertRaisesRegex(RecipeBlocked, 'xdotool, xclip, python3-pyatspi, dbus-monitor'):
            target.preflight()

    def test_deadline_inside_release_is_delivered_after_release_completes(self):
        completed = []
        def deadline(*_):
            raise RecipeFailed('forced deadline')
        previous = signal.signal(signal.SIGALRM, deadline)
        try:
            with self.assertRaisesRegex(RecipeFailed, 'forced deadline'):
                with input_release():
                    signal.setitimer(signal.ITIMER_REAL, .01)
                    time.sleep(.03)
                    completed.append('button up')
            self.assertEqual(completed, ['button up'])
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, previous)

    def test_root_cleanup_requires_created_root_and_successful_quit(self):
        target = Target.__new__(Target)
        target._rfb = None
        target.monitor_pid = target.clipboard_pid = None
        target.started = target._created_root = True
        target.root = '/tmp/sd-owned'
        calls = []
        target.cli = lambda *a: {'returncode': 0}
        target.execute = lambda script, **kw: calls.append(script) or {'returncode': 0}
        self.assertEqual(target.close(), [])
        self.assertIn('shutil.rmtree(root)', calls[0])
        self.assertFalse(target._created_root)
        calls.clear()
        self.assertEqual(target.close(), [])
        self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
