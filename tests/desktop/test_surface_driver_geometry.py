"""Geometry and cleanup regressions. Run on the lab host, never as desktop proof."""
import json
import math
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.catalog import Catalog, compare, control_point
from tools.lab.surface_driver.target import Target, input_release
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.run import run_recipes


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
        target.heartbeat = lambda **_: None
        target._outputs = [{'width': 1280, 'height': 800, 'x': 0, 'y': 0},
                           {'width': 1024, 'height': 768, 'x': 1280, 'y': 0}]
        sent = []
        target.run = Path('/owned/run')
        sending = patch('tools.lab.surface_driver.target.lab.input_events', side_effect=lambda run,*args:sent.append(args))
        sending.start()
        self.addCleanup(sending.stop)
        target.pointer(320, 200, 1280, 800, head=0)
        self.assertEqual(len(sent[0]), 1)  # no display/head-bound tablet
        self.assertEqual([e['data']['value'] for e in sent[0][0]], [math.ceil(320*32768/2304), 8192])
        target.pointer(100, 100, 1024, 768, head=1)
        self.assertEqual([e['data']['value'] for e in sent[1][0]],
                         [math.ceil(1380*32768/2304), 4096])


class PhysicalPointerSafety(unittest.TestCase):
    """Fake transports only: never connect to a desktop or an RFB listener."""
    def setUp(self):
        private = tempfile.TemporaryDirectory()
        self.addCleanup(private.cleanup)
        self.out = Path(private.name)
        self.held = json.dumps({'ok': True, 'state': 'held', 'expiresInSeconds': 600})
        self.expired = json.dumps({'ok': True, 'state': 'expired', 'expiresInSeconds': 0})
        self.target = Target.__new__(Target)
        self.target.run = self.out
        self.target.physical = SimpleNamespace(config={'name': 'f44', 'vnc_port': 59999},
                                               ssh=Mock(return_value=self.held))
        self.target._lock = ('f44', 'fake-session')
        self.target._last_heartbeat = self.target._lock_valid_until = 0
        self.target._verify_endpoints = Mock()
        self.target._rfb = Mock()
        self.target.info = {}
        self.environment = dict(desktop='mint-cinnamon-x11', host='f44', placement='native',
                                package='deb', destination='local', build='fake-transport-only')
        self.ctx = DriverContext(self.target, self.environment, {}, self.out, self.out)
        self.ctx._dimensions = lambda head: (100, 100)
        self.ctx._guard = lambda **query: {'backend': 'x11'}
        self.ctx.frame = lambda *args: None
        sleep = patch('tools.lab.surface_driver.context.time.sleep')
        sleep.start()
        self.addCleanup(sleep.stop)

    def click(self):
        self.ctx._pointer_click(10, 10, 'right', guarded=True)

    def drag(self):
        self.ctx.drag((10, 10), (30, 30), button='right', steps=2, head=0)

    def refused(self, action, reason):
        with self.assertRaisesRegex(RecipeBlocked, reason):
            action()
        self.target._rfb.pointer_button.assert_not_called()

    def test_expired_lock_refuses_click_without_release(self):
        self.target.physical.ssh.return_value = self.expired
        self.refused(self.click, 'lock is no longer held')

    def test_expired_lock_refuses_drag_without_release(self):
        self.target.physical.ssh.return_value = self.expired
        self.refused(self.drag, 'lock is no longer held')

    def test_changed_endpoint_refuses_click_without_release(self):
        self.target._verify_endpoints.side_effect = RecipeBlocked('endpoint identity changed')
        self.refused(self.click, 'endpoint identity changed')

    def test_changed_endpoint_refuses_drag_without_release(self):
        self.target._verify_endpoints.side_effect = RecipeBlocked('endpoint identity changed')
        self.refused(self.drag, 'endpoint identity changed')

    def test_refused_press_never_opens_a_lazy_rfb_connection(self):
        self.target._rfb = None
        self.target._verify_endpoints.side_effect = RecipeBlocked('endpoint identity changed')
        with patch('tools.lab.surface_driver.target.rfb.RFB') as connect:
            for action in (self.click, self.drag):
                with self.assertRaisesRegex(RecipeBlocked, 'endpoint identity changed'):
                    action()
            connect.assert_not_called()

    def test_lock_lost_mid_drag_withholds_release_and_names_host_and_button(self):
        self.target.physical.ssh.side_effect = [self.held, self.held, self.expired, self.expired]
        with self.assertRaisesRegex(RecipeBlocked, 'f44.*right.*may still be pressed'):
            self.drag()
        self.assertEqual(self.target._rfb.pointer_button.call_args_list,
                         [call(10, 10, 'right', True), call(20, 20, 'left', None)])
        self.assertTrue(self.ctx._input_poisoned)

    def test_endpoint_lost_mid_drag_withholds_release_and_names_host_and_button(self):
        self.target._verify_endpoints.side_effect = [None, None,
            RecipeBlocked('endpoint identity changed'), RecipeBlocked('endpoint identity changed')]
        with self.assertRaisesRegex(RecipeBlocked, 'f44.*right.*may still be pressed'):
            self.drag()
        self.assertEqual(self.target._rfb.pointer_button.call_args_list,
                         [call(10, 10, 'right', True), call(20, 20, 'left', None)])
        self.assertTrue(self.ctx._input_poisoned)

    def test_held_lock_releases_dispatched_click_after_fresh_checks(self):
        self.click()
        self.assertEqual(self.target._rfb.pointer_button.call_args_list,
                         [call(10, 10, 'right', True), call(10, 10, 'right', False)])
        self.assertEqual(self.target._verify_endpoints.call_count, 2)
        self.assertEqual(self.target.physical.ssh.call_count, 2)

    def test_held_lock_releases_dispatched_drag_after_fresh_checks(self):
        self.drag()
        self.assertEqual(self.target._rfb.pointer_button.call_args_list,
                         [call(10, 10, 'right', True), call(20, 20, 'left', None),
                          call(30, 30, 'left', None), call(30, 30, 'right', False)])
        self.assertEqual(self.target._verify_endpoints.call_count, 4)
        self.assertEqual(self.target.physical.ssh.call_count, 4)

    def test_release_forces_a_heartbeat_even_with_a_cached_lease(self):
        self.target._last_heartbeat = time.monotonic()
        self.target._lock_valid_until = time.monotonic() + 600
        self.target.physical.ssh.return_value = self.expired
        with self.assertRaisesRegex(RecipeBlocked, 'f44.*right.*may still be pressed'):
            self.target.release_pointer(10, 10, 100, 100, 'right')
        self.target._rfb.pointer_button.assert_not_called()

    def test_release_endpoint_failure_never_opens_a_lazy_rfb_connection(self):
        self.target._rfb = None
        self.target._verify_endpoints.side_effect = RecipeBlocked('endpoint identity changed')
        with patch('tools.lab.surface_driver.target.rfb.RFB') as connect:
            with self.assertRaisesRegex(RecipeBlocked, 'f44.*right.*may still be pressed'):
                self.target.release_pointer(10, 10, 100, 100, 'right')
            connect.assert_not_called()

    def test_deadline_after_physical_press_dispatch_still_releases_with_checks(self):
        def dispatch(x, y, button, down):
            if down:
                signal.raise_signal(signal.SIGALRM)
        self.target._rfb.pointer_button.side_effect = dispatch
        def deadline(*_):
            raise RecipeFailed('forced press deadline')
        previous = signal.signal(signal.SIGALRM, deadline)
        try:
            with self.assertRaisesRegex(RecipeFailed, 'forced press deadline'):
                self.click()
        finally:
            signal.signal(signal.SIGALRM, previous)
        self.assertEqual(self.target._rfb.pointer_button.call_args_list,
                         [call(10, 10, 'right', True), call(10, 10, 'right', False)])
        self.assertEqual(self.target._verify_endpoints.call_count, 2)

    def guest_drag_always_releases(self, failure):
        self.target.physical = self.target._lock = None
        def pointer(*args):
            if failure == ('press' if args[5] is True else 'move'):
                raise RecipeFailed('guest dispatch interrupted')
        with patch('tools.lab.surface_driver.target.lab.pointer', side_effect=pointer), \
             patch('tools.lab.surface_driver.target.lab.input_events') as send:
            if failure:
                with self.assertRaisesRegex(RecipeFailed, 'guest dispatch interrupted'):
                    self.drag()
            else:
                self.drag()
        send.assert_called_once_with(self.target.run,
            [{'type': 'btn', 'data': {'button': 'right', 'down': False}}])
        # One press, zero/two move attempts, then a fresh owned-endpoint
        # check before the button-up-only QMP packet.
        self.assertEqual(self.target._verify_endpoints.call_count,
                         {'press': 2, 'move': 3, None: 4}[failure])

    def test_guest_uncertain_press_always_releases_after_owned_endpoint_check(self):
        self.guest_drag_always_releases('press')

    def test_guest_interrupted_drag_always_releases_after_owned_endpoint_check(self):
        self.guest_drag_always_releases('move')

    def test_guest_completed_drag_releases_after_owned_endpoint_check(self):
        self.guest_drag_always_releases(None)

    def test_lost_lock_is_recorded_as_blocked_with_host_and_button(self):
        self.target.physical.ssh.side_effect = [self.held, self.expired, self.expired]
        rows = run_recipes(self.ctx, [Recipe(('editor.undo',), lambda ctx: self.drag())],
                           self.environment, self.out, 'physical-pointer-fakes', self.out/'coverage.json')
        self.assertEqual(rows[0].functional, 'blocked')
        receipt = json.loads((self.out/'receipt.json').read_text())['interactions'][0]
        self.assertRegex(receipt['note'], 'f44.*right.*may still be pressed')
        self.assertEqual(self.target._rfb.pointer_button.call_args_list, [call(10, 10, 'right', True)])


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
        target._lock = None
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

    def probe_target(self):
        target = Target.__new__(Target)
        target.physical = None
        target.run = Path('/owned/run')
        target.heartbeat = lambda **_:None
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
        target.heartbeat = lambda **_:None
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
