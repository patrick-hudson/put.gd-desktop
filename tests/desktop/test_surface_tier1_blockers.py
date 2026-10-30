"""Targeted refusal regressions; these synthetic tests are not desktop proof."""
from pathlib import Path
import json
import os
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as plasma
from tools.lab.surface_driver.fixtures import keyring
from tests.desktop.test_surface_live_evidence import Base


class SelectorMapping(unittest.TestCase):
    def test_wayland_selector_checks_the_next_drag_point_before_motion(self):
        ctx=DriverContext.__new__(DriverContext)
        ctx._input_head=0;ctx._current_window='region';ctx._press_open=False;ctx.profile={}
        ctx.target=Mock(physical=None);ctx._dimensions=lambda _: (1280,800)
        ctx._check_time=Mock();ctx._press=Mock();ctx._release_pointer=Mock();ctx.review=Mock()
        ctx._guard=Mock(side_effect=[{'backend':'gnome-owned-selector-pick'},RecipeBlocked('covered')])
        with self.assertRaisesRegex(RecipeBlocked,'covered'):ctx.drag((20,20),(620,300),steps=2)
        self.assertEqual(ctx._guard.call_args.kwargs,dict(holding=True,point=(320,160)))
        ctx.target.pointer.assert_not_called()
        ctx._release_pointer.assert_called_once()

    def test_shell_selector_request_and_pointer_are_rechecked_before_input(self):
        selector=dict(request_id='request-1',client_pid=7,name='Capture region',rect=[0,0,1280,800],pointer=True,keyboard=True)
        query=dict(selector=True,accessible={'name':'Capture region'},compositor={'kind':'gnome-lab-witness'})
        with patch.object(probe,'gnome_witness',side_effect=lambda *args:json.dumps(selector)),patch.object(probe,'owned',return_value=True):
            target=probe.geometry(query)
            guarded=dict(geometry=target,point=[40,50],compositor=query['compositor'])
            self.assertTrue(probe.witness_foreground(guarded)['ok'])
            selector['pointer']=False
            self.assertFalse(probe.witness_foreground(guarded)['ok'])
            selector['pointer']=True;selector['request_id']='another-request'
            self.assertFalse(probe.witness_foreground(guarded)['ok'])
            with patch.object(probe,'owned',return_value=False):
                with self.assertRaisesRegex(RuntimeError,'0 matches'):probe.geometry(query)

    def test_native_cover_survives_selection_and_output_union(self):
        cover = dict(x=0, y=0, width=1280, height=800, pid=7, override_redirect=True)
        query = dict(title='.*', accessible={'name':'Capture region'}, selector_screens=[[0,0,1280,800]])
        with patch.dict('os.environ', XDG_SESSION_TYPE='x11'), patch.object(probe,'x11_windows',return_value=[cover]), \
             patch.object(probe,'native_capture_process',return_value=True):
            for selection in ([0,0,2304,800], [30,40,400,200]):
                with patch.object(probe,'accessible',return_value=[dict(pid=7,name='Capture region',rect=selection)]):
                    self.assertEqual(probe.geometry(query), cover)
            with patch.object(probe,'accessible',return_value=[dict(pid=8,name='Capture region',rect=[0,0,2304,800])]):
                with self.assertRaisesRegex(RuntimeError,'0 matches'):probe.geometry(query)
            with patch.object(probe,'accessible',return_value=[dict(pid=7,name='Capture region',rect=[30,40,400,200])]), \
                 patch.object(probe,'native_capture_process',return_value=False):
                with self.assertRaisesRegex(RuntimeError,'0 matches'):probe.geometry(query)

    def test_anonymous_wayland_cover_needs_exact_owned_helper_and_unique_size(self):
        window=dict(id=1,pid=7,title='',frame=[0,0,1280,800],type=0)
        placement=probe.WaylandPlacement({});placement._windows=[window]
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'selector_process',return_value=True):
            self.assertEqual(placement.toplevel(7,'Capture region',[0,0,1280,800])['window_id'],1)
            placement._cache.clear();placement._windows=[window,dict(window,id=2)]
            with self.assertRaisesRegex(RuntimeError,'2 matches'):placement.toplevel(7,'Capture region',[0,0,1280,800])
        for owned, helper in ((False,True),(True,False)):
            placement._cache.clear();placement._windows=[window]
            with patch.object(probe,'owned',return_value=owned),patch.object(probe,'selector_process',return_value=helper):
                with self.assertRaisesRegex(RuntimeError,'0 matches'):placement.toplevel(7,'Capture region',[0,0,1280,800])


class LocalFrames(Base):
    destination='local'

    def test_observed_loopback_link_does_not_mask_browser_content(self):
        ctx=self.context();ctx.sensitive('http://127.0.0.1:1234/synthetic.png')
        ctx._secret_regions=Mock(return_value=[[1,2,3,4]])
        ctx._layout=Mock(side_effect=AssertionError('local does not use live layout policy'))
        frame=ctx.frame('content')
        metadata=ctx.evidence.frames[frame.path]
        self.assertTrue(metadata['visible_pixels'])
        self.assertEqual(metadata['regions'],[[1,2,3,4]])
        self.assertEqual(metadata['guard'],'local-secret-fields-only')
        ctx._secret_regions.assert_called_once()


class SnapOwnership(unittest.TestCase):
    def test_private_profile_lease_rejects_reused_pid_other_runtime_and_public_record(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);process=root/'proc'/'7';process.mkdir(parents=True)
            (process/'exe').symlink_to('/snap/firefox/current/firefox')
            profile=root/'putgd-surface-fixture';profile.mkdir(mode=0o700)
            (process/'cmdline').write_bytes(b'firefox\0--profile\0'+os.fsencode(profile)+b'\0')
            (process/'stat').write_text('7 (firefox) S '+' '.join(['0']*18)+' 100 0')
            boot=root/'boot';boot.write_text('boot')
            state=dict(directory=str(profile),runtime='/private/runtime',boot='boot',owners=[[7,'100']])
            stored=profile/'transaction.json';stored.write_text(json.dumps(state));stored.chmod(0o600)
            real_path=Path
            def mapped(path):
                if path=='/proc':return root/'proc'
                if path=='/proc/sys/kernel/random/boot_id':return boot
                return real_path(path)
            with patch('pathlib.Path',side_effect=mapped),patch.dict(os.environ,XDG_RUNTIME_DIR='/private/runtime'):
                self.assertTrue(probe.private_firefox(7))
                for key,value in (('runtime','/other/runtime'),('owners',[[7,'99']]),('boot','earlier-boot')):
                    stored.write_text(json.dumps(dict(state,**{key:value})))
                    self.assertFalse(probe.private_firefox(7))
                stored.write_text(json.dumps(state));stored.chmod(0o644)
                self.assertFalse(probe.private_firefox(7))


class PlasmaProperties(unittest.TestCase):
    def test_only_existing_is_enabled_changes_with_or_without_screen_reader_property(self):
        for properties in ({'IsEnabled':False}, {'IsEnabled':False,'ScreenReaderEnabled':True,'Unrelated':False}):
            values=dict(properties)
            status=Mock();status.GetAll.side_effect=lambda _:dict(values)
            status.Set.side_effect=lambda interface,key,value:values.__setitem__(key,value)
            module=Mock();module.SessionBus.return_value=Mock();module.Interface.return_value=status;module.Boolean=bool
            with patch.dict(sys.modules,dbus=module):
                exec(plasma.SCRIPT.split("\n",1)[1].rsplit('\nSD_PLASMA_A11Y',1)[0], {})
            self.assertEqual(values,dict(properties,IsEnabled=True))

    def test_existing_ksecretd_is_never_replaced_or_stopped(self):
        scope=Mock();scope.ctx.placement='boat';scope.ctx.profile={'guest':'owned'}
        scope.json.return_value=dict(owner=':1.7',pid=99,provider='ksecretd')
        self.assertEqual(keyring.isolated_service(scope)['pid'],99)
        scope.launch.assert_not_called();scope.stop.assert_not_called();scope.python.assert_not_called()


if __name__=='__main__':unittest.main()
