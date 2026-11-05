"""Targeted refusal regressions; these synthetic tests are not desktop proof."""
from pathlib import Path
from contextlib import redirect_stdout
import json
import io
import hashlib
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked, ReceiptRow
from tools.lab.surface_driver import run as runner
from tools.lab.surface_driver import redact
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as plasma
from tools.lab.surface_driver.fixtures import keyring
from tests.desktop.test_surface_live_evidence import Base


class SelectorMapping(unittest.TestCase):
    def test_native_appimage_capture_matches_verified_bytes_without_path_entry(self):
        """An owned mounted helper matches its independently extracted copy."""
        data=b'synthetic trusted capture binary'
        digests={'putgd-capture':hashlib.sha256(data).hexdigest()}
        with patch.object(probe,'VERIFIED_HELPER_SHA256',digests),patch('shutil.which',return_value=None), \
             patch.object(probe,'owned',return_value=True),patch('builtins.open',return_value=io.BytesIO(data)):
            self.assertTrue(probe.native_capture_process(7))

    def test_native_appimage_capture_rejects_lookalike_and_unowned_binary(self):
        """A matching name, foreign bytes or an unowned copy cannot authorize input."""
        trusted=b'synthetic trusted capture binary'
        digests={'putgd-capture':hashlib.sha256(trusted).hexdigest()}
        with patch.object(probe,'VERIFIED_HELPER_SHA256',digests):
            for data,owned in ((b'lookalike putgd-capture binary',True),(trusted,False)):
                with self.subTest(owned=owned),patch.object(probe,'owned',return_value=owned), \
                     patch('builtins.open',return_value=io.BytesIO(data)):
                    self.assertFalse(probe.native_capture_process(7))

    def test_swapped_extracted_helpers_cannot_replace_the_installed_digest(self):
        """Replacing both a guest file and its running copy cannot change the host receipt."""
        with tempfile.TemporaryDirectory() as td:
            home=Path(td);directory=home/'putgd-lab/appimage/squashfs-root/usr/bin';directory.mkdir(parents=True)
            original=b'synthetic installed helper';replacement=b'swapped same-name helper'
            digests={name:hashlib.sha256(original).hexdigest() for name in ('putgd-capture','putgd-kde-region','putgd-ui')}
            for name in digests:
                binary=directory/name;binary.write_bytes(original);binary.write_bytes(replacement)
            with patch('pathlib.Path.home',return_value=home),patch('shutil.which',return_value=None), \
                 patch.object(probe,'VERIFIED_HELPER_SHA256',digests,create=True),patch.object(probe,'owned',return_value=True):
                for check in (probe.native_capture_process,probe.selector_process):
                    with self.subTest(check=check.__name__),patch('builtins.open',return_value=io.BytesIO(replacement)):
                        self.assertFalse(check(7))

    def test_helper_roles_and_missing_receipts_cannot_authorize_a_capture(self):
        """A trusted UI helper is not a capture helper, and a pathname supplies no authority."""
        data=b'synthetic UI helper';digests={'putgd-ui':hashlib.sha256(data).hexdigest()}
        with patch.object(probe,'VERIFIED_HELPER_SHA256',digests),patch.object(probe,'owned',return_value=True):
            with patch('builtins.open',return_value=io.BytesIO(data)):
                self.assertTrue(probe.selector_process(7))
            with patch('builtins.open',return_value=io.BytesIO(data)):
                self.assertFalse(probe.native_capture_process(7))
        with patch.object(probe,'VERIFIED_HELPER_SHA256',{}),patch.object(probe,'owned',return_value=True), \
             patch('os.path.samefile',return_value=True),patch('shutil.which',return_value='/installed/putgd-capture'):
            self.assertFalse(probe.native_capture_process(7))
            self.assertFalse(probe.selector_process(7))

    def test_appimage_helper_needs_the_verified_extracted_binary_bytes(self):
        """The controller-provided inventory distinguishes installed bytes from a lookalike."""
        trusted=b'synthetic trusted binary';digests={'putgd-kde-region':hashlib.sha256(trusted).hexdigest()}
        with patch.object(probe,'VERIFIED_HELPER_SHA256',digests),patch.object(probe,'owned',return_value=True):
            for data,expected in ((b'foreign same-name binary',False),(trusted,True)):
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

    def test_native_pointer_cover_can_precede_accessibility_but_never_relaxes_keyboard(self):
        cover = dict(x=0, y=0, width=1280, height=800, pid=7, override_redirect=True, name='')
        query = dict(title='.*', selector=True, accessible={'name':'Capture region'})
        monitor = 'Monitors: 1\n 0: +*Virtual-1 1280/320x800/200+0+0 Virtual-1\n'
        with patch.dict('os.environ', XDG_SESSION_TYPE='x11'), \
             patch.object(probe, 'run', return_value=monitor), \
             patch.object(probe, 'x11_windows', return_value=[cover]), \
             patch.object(probe, 'accessible', return_value=[]), \
             patch.object(probe, 'owned', return_value=True), \
             patch.object(probe, 'native_capture_process', return_value=True):
            self.assertEqual(probe.geometry(query), cover)
            for application in ('putgd*', 'putgd-capture'):
                self.assertEqual(probe.geometry(dict(query, accessible={
                    'name': 'Capture region', 'application': application})), cover)
            # A missing node may mean its caller-supplied constraint rejected
            # it. Only the independently proved native application is allowed.
            for constraint in ({'pid': 8}, {'pid': 7}, {'role': 'panel'},
                               {'states': {'focused': True}}, {'application': 'foreign*'}):
                with self.subTest(constraint=constraint), self.assertRaisesRegex(RuntimeError, '0 matches'):
                    probe.geometry(dict(query, accessible={'name': 'Capture region', **constraint}))
            for changed in (dict(cover, override_redirect=False), dict(cover, name='foreign'),
                            dict(cover, width=1), dict(cover, x=20)):
                with patch.object(probe, 'x11_windows', return_value=[changed]):
                    with self.assertRaisesRegex(RuntimeError, '0 matches'):probe.geometry(query)
            for check in ('owned', 'native_capture_process'):
                with patch.object(probe, check, return_value=False):
                    with self.assertRaisesRegex(RuntimeError, '0 matches'):probe.geometry(query)
            with patch.object(probe, 'accessible', return_value=[dict(pid=8, name='Capture region', rect=[0,0,1280,800])]):
                with self.assertRaisesRegex(RuntimeError, '0 matches'):probe.geometry(query)
            with patch.object(probe, 'x11_windows', return_value=[cover, dict(cover, pid=8)]):
                with self.assertRaisesRegex(RuntimeError, '2 matches'):probe.geometry(query)
            with self.assertRaisesRegex(RuntimeError, '0 matches'):
                probe.geometry(dict(query, accessible={'name':'Recording region'}))
        # Empty/stale accessibility cannot authorize a keyboard grab: the
        # NativeGrab negative cases in test_surface_tray_input stay unchanged.


class LocalFrames(Base):
    destination='local'

    def test_local_required_fields_exclude_fake_links_without_dropping_credentials(self):
        ctx=self.context()
        ctx.catalog.inventories={'destinations':{'controls':[
            {'name':'publicUrl','class':'QLineEdit'}, {'name':'credentialSecretKey','class':'QLineEdit'}]}}
        self.assertEqual(ctx._secret_names(),{'credentialSecretKey'})
        ctx.destination='s3'
        self.assertEqual(ctx._secret_names(),{'publicUrl','credentialSecretKey'})


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

    def keyring_scope(self, prior, current=None, names=None):
        scope=Mock();scope.ctx.placement='boat';scope.ctx.profile={'guest':'owned'}
        scope.root='/tmp/sd-unit/fixture';scope.ctx.shell.return_value={'returncode':0}
        scope.launch.return_value={'pid':100,'start':'22'}
        reads=iter([prior,current if current is not None else prior])
        scope.json.side_effect=lambda code: next(reads) if code==keyring.OWNER else (names or [':1.100'])
        scope.ctx.wait.side_effect=lambda ready,*a,**k:ready()
        return scope

    def test_existing_ksecretd_keeps_its_name_and_collections_while_private_client_is_pinned(self):
        scope=self.keyring_scope(dict(owner=':1.7',pid=99,provider='ksecretd'))
        self.assertEqual(keyring.isolated_service(scope)['bus_name'],':1.100')
        scope.restart.assert_called_once_with({'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':'',
                                              'SECRET_SERVICE_BUS_NAME':':1.100'})
        self.assertNotIn('--replace',scope.launch.call_args.args[0])
        self.assertTrue(scope.launch.call_args.kwargs['environment']['XDG_DATA_HOME'].startswith(scope.root+'/'))
        scope.stop.assert_not_called()

    def test_owner_change_or_ambiguous_connection_refuses_private_client_restart(self):
        prior=dict(owner=':1.7',pid=99,provider='ksecretd')
        for scope in (self.keyring_scope(prior,dict(prior,owner=':1.8')),
                      self.keyring_scope(prior,names=[':1.100',':1.101'])):
            with self.assertRaises(RecipeBlocked):keyring.isolated_service(scope)
            scope.restart.assert_not_called()

    def test_unowned_service_keeps_existing_non_aurora_fixture_behaviour(self):
        scope=self.keyring_scope(dict(owner=None,pid=None,provider=None),
                                 dict(owner=':1.100',pid=100,provider='gnome-keyring-d'))
        self.assertEqual(keyring.isolated_service(scope),{'pid':100,'start':'22'})
        scope.restart.assert_called_once_with({'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':''})


if __name__=='__main__':unittest.main()
