"""Targeted refusal regressions; these synthetic tests are not desktop proof."""
from pathlib import Path
import json
import io
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked, ReceiptRow
from tools.lab.surface_driver import run as runner
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as plasma
from tools.lab.surface_driver.fixtures import keyring
from tests.desktop.test_surface_live_evidence import Base


class SelectorMapping(unittest.TestCase):
    def test_appimage_helper_needs_the_verified_extracted_binary_bytes(self):
        with tempfile.TemporaryDirectory() as td:
            home=Path(td);binary=home/'putgd-lab/appimage/squashfs-root/usr/bin/putgd-kde-region'
            binary.parent.mkdir(parents=True);binary.write_bytes(b'synthetic trusted binary')
            with patch('pathlib.Path.home',return_value=home),patch('shutil.which',return_value=None):
                for data,expected in ((b'foreign same-name binary',False),(b'synthetic trusted binary',True)):
                    with patch('builtins.open',return_value=io.BytesIO(data)):
                        self.assertEqual(probe.selector_process(7),expected)

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

    def test_local_required_fields_exclude_fake_links_without_dropping_credentials(self):
        ctx=self.context()
        ctx.catalog.inventories={'destinations':{'controls':[
            {'name':'publicUrl','class':'QLineEdit'}, {'name':'credentialSecretKey','class':'QLineEdit'}]}}
        self.assertEqual(ctx._secret_names(),{'credentialSecretKey'})
        ctx.destination='s3'
        self.assertEqual(ctx._secret_names(),{'publicUrl','credentialSecretKey'})

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

    def test_final_local_receipt_and_coverage_need_no_ocr_even_after_a_link(self):
        ctx=self.context();ctx.sensitive('http://127.0.0.1:1234/synthetic.png')
        ctx._secret_regions=Mock(return_value=[])
        rows=[ReceiptRow('region.drag',self.env['desktop'],'deb','passed')]
        with patch('tools.lab.surface_driver.redact.shutil.which',return_value=None):
            ctx.frame('content')
            result=runner.finish_evidence(ctx.evidence,self.out,self.env,rows,'local',self.path/'coverage.json')
        self.assertTrue(result['passed'],result)
        self.assertEqual(rows[0].functional,'passed')
        self.assertTrue(json.loads((self.out/'receipt.json').read_text())['secret_scan']['passed'])


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
