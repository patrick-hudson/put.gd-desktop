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
    def test_editor_press_is_released_if_x11_delivery_hold_is_interrupted(self):
        ctx, directory = context({'session':'x11'})
        self.addCleanup(directory.cleanup)
        ctx._current_window = 'editor'
        ctx._guard = Mock(return_value={'backend':'x11'})
        with patch('tools.lab.surface_driver.context.time.sleep', side_effect=RecipeFailed('interrupted hold')):
            with self.assertRaisesRegex(RecipeFailed, 'interrupted hold'):
                ctx.drag((20,30), (240,90))
        ctx.target.pointer.assert_called_once_with(20,30,1920,1080,True,'left',0)
        ctx.target.release_pointer.assert_called_once_with(20,30,1920,1080,'left',0)
        self.assertFalse(ctx._press_open)

    def test_live_control_refuses_any_part_clipped_by_a_viewport(self):
        sys.path.insert(0, str(Path(__file__).parent))
        from test_surface_driver_desktop import row
        control = row('Upload clipboard', objectName='recordClipboardShortcut', rect=[1140, 903, 150, 36])
        control['ancestors'] = [
            {'role': 'desktop frame', 'rect': [0, 0, 1024, 768]},
            {'role': 'application', 'rect': [0, 0, 1024, 768]},
            {'role': 'frame', 'rect': [300, 73, 1320, 880]},
            {'role': 'filler', 'rect': [516, 181, 1092, 732]}]
        query = {'selector': 'recordClipboardShortcut', 'pid': 7, 'title': '^put\\.gd$'}
        with patch.object(probe, 'accessible', return_value=[control]):
            with self.assertRaisesRegex(RuntimeError, 'clipped by ancestor'):
                probe.control_geometry(query)
            control['rect'][1] = 850
            self.assertEqual(probe.control_geometry(query), control)
            control['rect'] = [1590, 850, 150, 36]  # horizontal clipping also refuses
            with self.assertRaisesRegex(RuntimeError, 'clipped by ancestor'):
                probe.control_geometry(query)

    def test_popup_is_clipped_by_its_surface_not_its_logical_owner(self):
        sys.path.insert(0, str(Path(__file__).parent))
        from test_surface_driver_desktop import row
        control = row('Submenu item', objectName='item', rect=[500, 160, 100, 30],
                      popup={'role': 'menu', 'rect': [500, 150, 180, 180]})
        control['ancestors'] = [{'role': 'menu item', 'rect': [300, 150, 180, 30]},
                                {'role': 'menu', 'rect': [500, 150, 180, 180]}]
        with patch.object(probe, 'accessible', return_value=[control]):
            self.assertEqual(probe.control_geometry({'selector': 'item'}), control)
            control['rect'][0] = 490
            with self.assertRaisesRegex(RuntimeError, 'clipped by ancestor'):
                probe.control_geometry({'selector': 'item'})

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
        api=SimpleNamespace(DESKTOP_COORDS=0)
        with patch.dict(sys.modules,pyatspi=api),patch.object(probe,'desktop_root',return_value=desktop),\
             patch.object(probe,'popup_roots',return_value=[]):
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
    def setUp(self):
        patcher = patch.object(probe, 'executable', return_value=['cinnamon'])
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_nonreactive_notification_missed_by_stage_pick_still_refuses(self):
        sys.path.insert(0,str(Path(__file__).parent))
        from test_surface_driver_desktop import FakeNode, PickNode
        banner=FakeNode('Upload failed','notification',[1420,10,480,140])
        stage=PickNode('','window',[0,0,1920,1080],PickNode('','panel',[0,0,1920,1080]))
        stage.append(banner)
        app=FakeNode('cinnamon','application',None,children=[stage])
        api=SimpleNamespace(DESKTOP_COORDS=0,STATE_SHOWING='showing')
        with patch.dict(sys.modules,pyatspi=api),patch.object(probe,'desktop_root',return_value=[app]):
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
    def test_staging_persists_the_uuid_without_losing_other_extensions(self):
        from tools.lab.surface_driver.compositor import stage_gnome_witness as staging
        source = Path(staging.__file__).with_name('gnome-witness')
        settings = Mock()
        settings.get_strv.return_value = ['putgd@put.gd', 'other@lab']
        gio = SimpleNamespace(Settings=SimpleNamespace(new=Mock(return_value=settings), sync=Mock()))
        scripts = []
        with patch.object(staging.lab, 'verify_owned_guest', return_value={}), \
             patch.object(staging.lab, 'state', return_value={'profile': 'gnome-wayland'}), \
             patch.object(staging.lab, 'load_profile', return_value={'session_process': 'gnome-shell'}), \
             patch.object(staging.lab, 'guest_session', side_effect=lambda run, script: scripts.append(script)):
            staging.stage('/owned/run', source)
        body = scripts[0].split("<<'SD_ENABLE'\n", 1)[1].split('\nSD_ENABLE', 1)[0]
        with patch.dict(sys.modules, {'gi.repository': SimpleNamespace(Gio=gio)}):
            exec(body, {})
        settings.set_strv.assert_called_once_with('enabled-extensions',
            ['putgd@put.gd', 'other@lab', 'putgd-lab-witness@put.gd'])
        gio.Settings.sync.assert_called_once()

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
                # A deadline-cut wait leaves one reply outstanding (#834): the
                # next request drains it and is answered by its own reply.
                self.assertEqual(target._probe_stale,1)
                self.assertEqual(target.probe('geometry',{},source),{'ok':True,'result':{}})
                self.assertEqual((target._probe_stale,target._probe_failed),(0,False))
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
            # The alarm hit after the whole request went out: its reply is
            # outstanding, not lost (#834). The next request drains it first.
            self.assertEqual((target._probe_stale,target._probe_failed),(1,False))
            self.assertEqual(target.probe('geometry',{},source),{'ok':True,'result':{}})
            self.assertEqual(target._probe_stale,0)
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


class DragGuards(unittest.TestCase):
    def drag_with_real_guard(self, require_resize):
        ctx,directory=context({'session':'wayland','compositor':{'kind':'gnome-lab-witness'}})
        self.addCleanup(directory.cleanup)
        observations,dispatched=[],[]
        def observe(name,query):
            observations.append((name,ctx._press_open,len(dispatched)))
            if name=='foreground':
                return {'ok':True}
            self.assertEqual(name,'resize')
            return ({'epoch':'a','serial':5,'events':[GrabEvidence.event]} if ctx._press_open
                    else GrabEvidence.before)
        def pointer(*args,**kwargs):
            self.assertTrue(ctx._press_open)
            dispatched.append((args[:2],args[4] if len(args)>4 else None))
        def release(*args):
            self.assertTrue(ctx._press_open)
        ctx._probe=Mock(side_effect=observe)
        ctx.target.pointer.side_effect=pointer
        ctx.target.release_pointer.side_effect=release
        ctx.drag((20,30),(50,60),steps=3,require_resize=require_resize)
        self.assertEqual(dispatched,[((20,30),True),((30,40),None),((40,50),None),((50,60),None)])
        self.assertEqual([holding for name,holding,_ in observations if name=='foreground'],
                         [False,True,True,True])
        self.assertEqual([(holding,sent) for name,holding,sent in observations if name=='resize'],
                         [(False,0),(True,2)] if require_resize else [])
        if require_resize:
            self.assertEqual(sum(note.startswith('resize-start ') for note in ctx.notes),1)
        ctx.target.release_pointer.assert_called_once_with(50,60,1920,1080,'left',0)
        self.assertFalse(ctx._press_open)
        self.assertFalse(ctx._input_poisoned)
        ctx._guard()  # A confirmed release also permits the next guarded input.

    def test_drag_steps_pass_real_guard_while_press_is_open(self):
        self.drag_with_real_guard(False)

    def test_resize_drag_steps_pass_real_guard_while_press_is_open(self):
        self.drag_with_real_guard(True)


class WitnessSignals(unittest.TestCase):
    def test_native_grab_signal_lifecycle_and_bounded_history(self):
        import shutil
        node=shutil.which('node')
        if not node:
            self.skipTest('Node is unavailable for the extension signal unit test')
        subprocess.run([node,'--test',str(Path(__file__).with_name('surface_gnome_witness.mjs'))],
                       check=True,capture_output=True,text=True,timeout=5)


if __name__=='__main__':
    unittest.main()
