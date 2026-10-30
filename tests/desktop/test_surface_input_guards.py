"""Input and observation regressions; fake desktops are never live evidence."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.target import Target


def context(profile=None):
    directory = tempfile.TemporaryDirectory()
    target = SimpleNamespace(physical=None, pointer=Mock(), release_pointer=Mock())
    ctx = DriverContext(target, dict(desktop='test', host='boat', placement='boat',
        package='deb', destination='local'), profile or {}, directory.name, directory.name)
    ctx._current_window = 'main'
    ctx.geometry = lambda _: dict(x=10, y=20, width=100, height=80, window_id=11, pid=50)
    ctx._dimensions = lambda _: (1920,1080)
    return ctx, directory


class FocusAndRoles(unittest.TestCase):
    def test_plain_kde_focus_verifies_without_requesting_unsupported_raise(self):
        ctx, directory = context({'session':'wayland','compositor':{'kind':'kwin-script'}})
        self.addCleanup(directory.cleanup)
        ctx._probe = Mock(return_value={'ok':True})
        ctx.focus('main')
        self.assertFalse(ctx._probe.call_args.args[1]['raise'])
        ctx.focus('main', raise_window=True)
        self.assertTrue(ctx._probe.call_args.args[1]['raise'])

    def test_raw_button_role_matches_both_spellings_and_catalog_class(self):
        row = {'role':'button'}
        for role in ('button','push button','push_button'):
            self.assertTrue(probe.matches(row, {'role':role}))
        self.assertEqual(probe.selector_matches(row,'class:QPushButton',[]),1)

    def test_accessibility_reports_the_server_role(self):
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_desktop import FakeNode
        button=FakeNode('Upload','button',[10,10,80,30])
        app=FakeNode('putgd','application',None,children=[button])
        desktop=FakeNode('main','desktop frame',None,children=[app])
        api=SimpleNamespace(Registry=SimpleNamespace(getDesktop=lambda _:desktop), DESKTOP_COORDS=0)
        with patch.dict(sys.modules,pyatspi=api),patch.object(probe,'popup_roots',return_value=[]):
            rows=probe.accessible({'name':'Upload','role':'push button'})
        self.assertEqual(rows[0]['role'],'button')


class EndpointChecks(unittest.TestCase):
    def target(self):
        target=Target.__new__(Target)
        target.physical=None
        target.run=Path('/owned/run')
        target.heartbeat=Mock()
        target._verify_endpoints=Mock()
        target.info={'heads':[{},{}]}
        target._outputs=[{'x':0,'y':0,'width':100,'height':80},
                         {'x':100,'y':0,'width':100,'height':80}]
        return target

    def test_one_identity_check_per_held_key_input(self):
        target=self.target()
        with patch('tools.lab.surface_driver.target.lab.input_events') as send:
            target.key_state(['shift'],True)
        target._verify_endpoints.assert_called_once()
        send.assert_called_once()

    def test_one_identity_check_per_multihead_pointer_input(self):
        target=self.target()
        with patch('tools.lab.surface_driver.target.lab.input_events') as send:
            target.pointer(20,20,100,80,head=1)
        target._verify_endpoints.assert_called_once()
        send.assert_called_once()

    def test_direct_events_still_verify(self):
        target=self.target()
        with patch('tools.lab.surface_driver.target.lab.input_events'):
            target.events([])
        target._verify_endpoints.assert_called_once()


class StartupRace(unittest.TestCase):
    def test_foreign_window_arriving_before_activation_is_not_displaced(self):
        reads=iter(['0','30','30'])
        with patch.object(probe,'x11_active',side_effect=lambda:next(reads)),\
             patch.object(probe,'run',return_value='99'),\
             patch.object(probe.subprocess,'run') as activate:
            self.assertFalse(probe.x11_settle({'window_id':'10','pid':7}))
        activate.assert_not_called()


class ResizeProof(unittest.TestCase):
    def test_resize_request_without_witness_blocks_before_press(self):
        ctx,directory=context({'session':'wayland'})
        self.addCleanup(directory.cleanup)
        ctx._guard=Mock(return_value={'ok':True})
        with self.assertRaisesRegex(RecipeBlocked,'resize.*witness'):
            ctx.drag((109,99),(80,70),require_resize=True)
        ctx.target.pointer.assert_not_called()


class ShellNotificationCover(unittest.TestCase):
    def test_nonreactive_notification_missed_by_stage_pick_still_refuses(self):
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_desktop import FakeNode, PickNode
        banner=FakeNode('Upload failed','notification',[1420,10,480,140])
        stage=PickNode('','window',[0,0,1920,1080],PickNode('','panel',[0,0,1920,1080]))
        stage.append(banner)
        app=FakeNode('cinnamon','application',None,children=[stage])
        api=SimpleNamespace(Registry=SimpleNamespace(getDesktop=lambda _:[app]),DESKTOP_COORDS=0,STATE_SHOWING='showing')
        with patch.dict(sys.modules,pyatspi=api):
            self.assertIn("notification 'Upload failed'",probe.shell_cover((1580,70),'cinnamon*'))
            self.assertIsNone(probe.shell_cover((100,500),'cinnamon*'))

    def test_covered_click_names_cover_and_dispatches_nothing(self):
        ctx,directory=context({'session':'x11','focus':{'desktop_hit_test':'shell-at-spi'},
                               'notifications':{'accessible':{'application':'cinnamon*'}}})
        self.addCleanup(directory.cleanup)
        ctx._probe=lambda name,query: probe.x11_foreground(query)
        with patch.object(probe,'shell_cover',return_value="cinnamon notification 'Upload failed' at [1420, 10, 480, 140]"):
            with self.assertRaisesRegex(RecipeFailed,"Upload failed"):
                ctx.click_at(1580,70)
        ctx.target.pointer.assert_not_called()


class FrameClientNames(unittest.TestCase):
    def test_foreign_titlebar_names_the_frames_client(self):
        def read(*args):
            if args == ('xprop','-root','_NET_CLIENT_LIST'):
                return '_NET_CLIENT_LIST(WINDOW): window id # 0x1e'
            if args[:2] == ('xdotool','getwindowname'):
                return 'Foreign editor' if args[2] == '30' else ''
            if args[:2] == ('xdotool','getwindowpid'):
                if args[2] == '30': return '99'
                raise subprocess.CalledProcessError(1,args)
        with patch.object(probe,'run',side_effect=read),patch.object(probe,'x11_ancestors',return_value=['30','20','1']):
            hit=probe.x11_hit_identity(['20','21'])
        self.assertEqual((hit['name'],hit['pid'],hit['client']),('Foreign editor',99,'30'))
        self.assertEqual(hit['path'],['20','21'])


class StageOverlay(unittest.TestCase):
    def test_real_desktop_and_golden_refused_before_any_install(self):
        from tools.lab.surface_driver.compositor import stage_gnome_witness as staging
        with patch.object(staging.lab,'verify_owned_guest',side_effect=staging.lab.LabError('not an owned overlay')),\
             patch.object(staging.lab,'guest_session') as install:
            with self.assertRaisesRegex(staging.lab.LabError,'owned overlay'):
                staging.stage('/real-desktop','/missing')
        install.assert_not_called()

    def test_overlay_revalidated_at_install_boundary(self):
        from tools.lab.surface_driver.compositor import stage_gnome_witness as staging
        source=Path(staging.__file__).with_name('gnome-witness')
        with patch.object(staging.lab,'verify_owned_guest',side_effect=[{},staging.lab.LabError('endpoint changed')]) as verify,\
             patch.object(staging.lab,'state',return_value={'profile':'gnome-wayland'}),\
             patch.object(staging.lab,'load_profile',return_value={'session_process':'gnome-shell'}),\
             patch.object(staging.lab,'guest_session') as install:
            with self.assertRaisesRegex(staging.lab.LabError,'endpoint changed'):
                staging.stage('/owned/run',source)
        self.assertEqual(verify.call_count,2)
        install.assert_not_called()


class PersistentBudgets(unittest.TestCase):
    def test_startup_does_not_renew_an_expired_request_budget(self):
        # Reuse the production transport against a local, bounded fake witness.
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_geometry import Hygiene
        target=Hygiene().probe_target()
        import time
        target.session=lambda *a,**kw: (time.sleep(.04) or '{}')
        source='def geometry(query): return {}'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                with self.assertRaisesRegex(RecipeBlocked,'geometry.*before dispatch; no request sent'):
                    target.probe('geometry',{},source,timeout=.02)
                self.assertFalse(target._probe_failed)
        finally:
            target._close_probe()

    def test_timeout_names_outstanding_probe_and_live_channel(self):
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_geometry import Hygiene
        target=Hygiene().probe_target()
        source='import time\ndef geometry(query):\n time.sleep(.08)\n return {}\n'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                with self.assertRaisesRegex(RecipeBlocked,'geometry; elapsed .*budget .*SSH process still running; reply outstanding'):
                    target.probe('geometry',{},source,timeout=.03)
                with self.assertRaisesRegex(RecipeBlocked,'start a new run'):
                    target.probe('geometry',{},source)
        finally:
            target._close_probe()

    def test_shell_cover_reads_are_fresh_without_restarting_channel(self):
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_geometry import Hygiene
        target=Hygiene().probe_target()
        source="""import json,os,sys
counter=0
def foreground(query):
 global counter
 counter+=1
 return {'counter':counter,'pid':os.getpid()}
if __name__=='__main__':
 print(json.dumps({'ok':True,'result':foreground(json.loads(sys.argv[1])['query'])}))
"""
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                query={'shell_cover':'cinnamon*','point':[1580,70]}
                first=target.probe('foreground',query,source)['result']
                channel=target._probe_process.pid
                second=target.probe('foreground',query,source)['result']
                self.assertEqual((first['counter'],second['counter']),(1,1))
                self.assertNotEqual(first['pid'],second['pid'])
                self.assertEqual(target._probe_process.pid,channel)
        finally:
            target._close_probe()

    def test_recipe_deadline_identifies_the_probe_budget(self):
        import time
        ctx,directory=context()
        self.addCleanup(directory.cleanup)
        ctx._deadline=time.monotonic()+.02
        def timeout(*args,**kwargs):
            time.sleep(.03)
            raise RecipeBlocked('persistent desktop probe timed out; reply outstanding')
        ctx.target.probe=timeout
        with self.assertRaisesRegex(RecipeBlocked,'recipe deadline exhausted during geometry probe'):
            ctx._probe('geometry',{})

    def test_recipe_alarm_keeps_the_outstanding_request_cause(self):
        from test_surface_driver_geometry import Hygiene
        target=Hygiene().probe_target()
        source='import time\ndef geometry(query):\n time.sleep(.08)\n return {}\n'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']),\
                 patch('tools.lab.surface_driver.target.selectors.DefaultSelector.select',side_effect=RecipeFailed('recipe deadline exceeded')):
                with self.assertRaisesRegex(RecipeFailed,'recipe deadline exceeded'):
                    target.probe('geometry',{},source)
            with self.assertRaisesRegex(RecipeBlocked,'geometry; interrupted by RecipeFailed.*SSH process still running.*start a new run'):
                target.probe('geometry',{},source)
        finally:
            target._close_probe()

    def test_closed_channel_names_request_and_child_exit(self):
        from test_surface_driver_geometry import Hygiene
        target=Hygiene().probe_target()
        source='import os\ndef geometry(query): os._exit(9)\n'
        try:
            with patch('tools.lab.surface_driver.target.lab.ssh_args',return_value=['sh','-c']):
                with self.assertRaisesRegex(RecipeBlocked,'closed during geometry; SSH exit'):
                    target.probe('geometry',{},source)
                self.assertIn('geometry',target._probe_failure)
        finally:
            target._close_probe()


class GrabEvidence(unittest.TestCase):
    target={'window_id':11,'pid':50}
    before={'epoch':'a','serial':4,'events':[]}
    event={'serial':5,'window_id':11,'pid':50,'phase':'begin','kind':'resize','operation':'RESIZING_SE'}

    def test_requires_new_resize_begin_for_exact_client(self):
        for wrong in ({'serial':4},{'window_id':12},{'pid':51},{'phase':'end'},
                      {'kind':'move','operation':'MOVING'},{'operation':'KEYBOARD_RESIZING_SE'}):
            after={'epoch':'a','serial':5,'events':[dict(self.event,**wrong)]}
            self.assertIsNone(probe.resize_started(self.before,after,self.target))
        after={'epoch':'a','serial':5,'events':[self.event]}
        self.assertEqual(probe.resize_started(self.before,after,self.target),self.event)

    def test_restarted_or_overflowed_witness_cannot_prove_gesture(self):
        for after in ({'epoch':'b','serial':5,'events':[self.event]},
                      {'epoch':'a','serial':300,'events':[dict(self.event,serial=20)]}):
            with self.assertRaisesRegex(RuntimeError,'restarted|lost events'):
                probe.resize_started(self.before,after,self.target)

    def test_resize_drag_retains_mutter_proof_after_first_motion(self):
        ctx,directory=context({'session':'wayland','compositor':{'kind':'gnome-lab-witness'}})
        self.addCleanup(directory.cleanup)
        ctx._guard=Mock(return_value={'ok':True})
        ctx._probe=Mock(side_effect=[self.before,{'epoch':'a','serial':5,'events':[self.event]}])
        ctx.drag((109,99),(80,70),steps=2,require_resize=True)
        self.assertIn('resize-start',ctx.notes[0])
        self.assertEqual(ctx.target.pointer.call_count,3)
        ctx.target.release_pointer.assert_called_once()


class WitnessSignals(unittest.TestCase):
    def test_native_grab_signal_lifecycle_and_bounded_history(self):
        import shutil
        node=shutil.which('node')
        if not node:
            self.skipTest('Node is unavailable for the extension signal unit test')
        root=Path(__file__).resolve().parents[2]
        subprocess.run([node,str(Path(__file__).with_name('surface_gnome_witness.mjs')),
                        str(root/'tools/lab/surface_driver/compositor/gnome-witness/extension.js')],
                       check=True,capture_output=True,text=True,timeout=5)


if __name__=='__main__':
    unittest.main()
