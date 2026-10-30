"""Geometry and cleanup regressions. Run on the lab host, never as desktop proof."""
import json
import math
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.catalog import Catalog, compare, control_point
from tools.lab.surface_driver.target import Target, input_release
from tools.lab.surface_driver.context import DriverContext


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
    def test_mid_drag_failure_releases_at_last_reached_position(self):
        with tempfile.TemporaryDirectory() as td:
            class Input:
                physical = None
                def pointer(self, *args, **kw):
                    events.append(args)
            events, guards = [], []
            ctx = DriverContext(Input(), dict(desktop='test',host='boat',placement='boat',package='deb',destination='local'),
                                {},td,td)
            ctx._dimensions = lambda head:(100,100)
            def guard(**query):
                guards.append(query)
                if len(guards) == 3:
                    raise RecipeFailed('new window covers the drag')
            ctx._guard = guard
            with self.assertRaisesRegex(RecipeFailed,'new window'):
                ctx.drag((10,10),(90,90),steps=4)
            self.assertEqual(events[1][:2],(30,30))
            self.assertEqual(events[-1][:2],(30,30))
            self.assertFalse(events[-1][4])
            self.assertFalse(any(row[:2] == (90,90) for row in events))

    def test_guest_release_sends_no_absolute_movement(self):
        target = Target.__new__(Target)
        target.physical = None
        target.run = Path('/owned/run')
        with patch('tools.lab.surface_driver.target.lab.input_events') as send:
            target.release_pointer(10,20,100,100)
        self.assertEqual(send.call_args.args[1],[{'type':'btn','data':{'button':'left','down':False}}])

    def test_unmanaged_drag_checks_each_intended_point_before_moving(self):
        with tempfile.TemporaryDirectory() as td:
            events = []
            class Input:
                physical = None
                def pointer(self,*args,**kw):events.append(('pointer',args[:2]))
            ctx = DriverContext(Input(),dict(desktop='test',host='boat',placement='boat',package='deb',destination='local'),{},td,td)
            ctx._dimensions = lambda head:(100,100)
            def guard(**query):
                events.append(('guard',query['point']))
                return {'ok':True,'backend':'x11-unmanaged-hit-test'}
            ctx._guard = guard
            ctx.drag((10,10),(30,30),steps=2)
            self.assertEqual(events[:6],[('guard',(10,10)),('pointer',(10,10)),
                                        ('guard',(20,20)),('pointer',(20,20)),
                                        ('guard',(30,30)),('pointer',(30,30))])

    def test_persistent_probe_keeps_channel_but_observes_each_request(self):
        target = self.probe_target()
        source = 'counter=0\ndef geometry(query):\n global counter\n counter+=1\n return counter\n'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                self.assertEqual(target.probe('geometry',{},source)['result'],1)
                pid = target._probe_process.pid
                self.assertEqual(target.probe('geometry',{},source)['result'],2)
                self.assertEqual(target._probe_process.pid,pid)
        finally:
            target._close_probe()

    def test_timed_out_probe_cannot_consume_a_late_witness_on_retry(self):
        target = self.probe_target()
        source = 'import time\ndef geometry(query):\n time.sleep(.2)\n return {}\n'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                with self.assertRaisesRegex(RecipeBlocked,'timed out'):
                    target.probe('geometry',{},source,timeout=.01)
                with self.assertRaisesRegex(RecipeBlocked,'start a new run'):
                    target.probe('geometry',{},source)
        finally:
            target._close_probe()

    def test_accessibility_queries_get_fresh_interpreters_on_the_same_channel(self):
        target = self.probe_target()
        source = '''import json,os,sys
counter=0
def control_geometry(query):
 global counter
 counter+=1
 return {'counter':counter,'pid':os.getpid()}
def main():
 print(json.dumps({'ok':True,'result':control_geometry(json.loads(sys.argv[1])['query'])}))
if __name__=='__main__':main()
'''
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                first = target.probe('control',{},source)['result']
                channel = target._probe_process.pid
                second = target.probe('control',{},source)['result']
                self.assertEqual((first['counter'],second['counter']),(1,1))
                self.assertNotEqual(first['pid'],second['pid'])
                self.assertEqual(target._probe_process.pid,channel)
        finally:
            target._close_probe()

    def test_popup_aware_probe_explicitly_opts_into_persistent_accessibility(self):
        target = self.probe_target()
        source = '''import os
PERSISTENT_ACCESSIBILITY=True
counter=0
def control_geometry(query):
 global counter
 counter+=1
 return {'counter':counter,'pid':os.getpid()}
'''
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                first = target.probe('control',{},source)['result']
                second = target.probe('control',{},source)['result']
                self.assertEqual((first['counter'],second['counter']),(1,2))
                self.assertEqual(first['pid'],second['pid'])
        finally:
            target._close_probe()

    def probe_target(self):
        target = Target.__new__(Target)
        target.physical = None
        target.run = Path('/owned/run')
        target.heartbeat = lambda:None
        target.session = lambda *a,**kw:'{}'
        target.prefix = lambda:'export PATH=/usr/bin:/bin\n'
        target._probe_process = target._probe_source = None
        target._probe_failed = False
        target._probe_buffer = b''
        return target

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

    def test_vnc_scroll_pulse_finishes_release_before_a_forced_deadline(self):
        events = []
        class Vnc:
            def pointer_button(self,x,y,button,down):
                if not down:
                    signal.setitimer(signal.ITIMER_REAL,.01)
                    time.sleep(.03)
                events.append((button,down))
        target = Target.__new__(Target)
        target.heartbeat = lambda:None
        target.vnc = lambda:Vnc()
        def deadline(*_):raise RecipeFailed('forced scroll deadline')
        previous = signal.signal(signal.SIGALRM,deadline)
        try:
            with self.assertRaisesRegex(RecipeFailed,'forced scroll deadline'):
                target.scroll_vnc(10,10,1)
            self.assertEqual(events,[('wheel-up',True),('wheel-up',False)])
        finally:
            signal.setitimer(signal.ITIMER_REAL,0)
            signal.signal(signal.SIGALRM,previous)

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
