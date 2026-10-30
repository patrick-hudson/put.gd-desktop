"""Tray state/lifecycle contracts. No app, desktop, account or live credentials."""
from pathlib import Path
import sys
from unittest.mock import Mock,patch
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tests.desktop import test_surface_fixtures as fixture_tests
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.fixtures import FACTORIES,resident,tray
from tools.lab.surface_driver.fixtures.base import Fixture,Scope


class TrayFixtures(fixture_tests.Fixtures):
    def test_incomplete_alias_waits_for_real_automatic_cleanup(self):
        automatic=Mock(return_value={'trigger':'automatic'})
        value=Fixture(operationId='owned',itemId='owned-item',automatic_cleanup=automatic)
        with patch.dict(FACTORIES,{'events.expired-multipart':lambda s:value}):
            result=self.ctx.fixture('upload-incomplete')
        automatic.assert_called_once_with()
        self.assertTrue(result.automatic)
        self.assertEqual(result.cleanup_result,{'trigger':'automatic'})
        self.assertEqual(result.operationId,'owned')
        self.assertFalse(any('Notify' in method for method,params in self.ctx.calls))

    def test_resident_launch_refuses_duplicates_and_restores_after_quit(self):
        self.ctx.fixture('app-profile');scope=self.ctx.manager.scope
        socket=Path(self.ctx.target.socket);socket.parent.mkdir();socket.touch()
        def launched():socket.touch()
        with patch.object(resident,'identity',return_value={'pid':42,'start':'owned'}),patch.object(scope,'_relaunch',side_effect=launched) as launch:
            value=self.ctx.fixture('resident-lifecycle')
            with self.assertRaisesRegex(RecipeBlocked,'duplicate'):value.launch()
            socket.unlink();self.assertTrue(value.stopped())
            self.ctx.manager.close()
            launch.assert_called_once_with()
        self.assertTrue(socket.exists())

    def test_qt_fallback_uses_real_platform_flag_and_restores_executable(self):
        original=self.ctx.target.app_executable
        value=self.ctx.fixture('tray-qt-fallback')
        wrapper=Path(self.ctx.target.app_executable)
        self.assertIn('-platform xcb',wrapper.read_text())
        self.assertNotIn('XDG_CURRENT_DESKTOP',wrapper.read_text())
        self.assertEqual(value.native_session,'x11')
        self.ctx.manager.close()
        self.assertEqual(self.ctx.target.app_executable,original)
        self.assertFalse(wrapper.exists())

    def test_two_uploads_have_independent_operation_ids(self):
        values=[Fixture(operation_id='op-a'),Fixture(operation_id='op-b')]
        with patch.object(tray,'submit',side_effect=values) as submit:
            value=self.ctx.fixture('tray-two-uploads')
        self.assertEqual(value.operation_ids,['op-a','op-b'])
        self.assertEqual(self.ctx.document['limits']['concurrentUploads'],2)
        self.assertTrue(all(call.kwargs=={'remote':True,'active':True} for call in submit.call_args_list))

    def test_attention_keeps_a_real_failed_local_save_and_retry_identity(self):
        value=self.ctx.fixture('tray-attention')
        self.assertEqual(value.state['state'],'failed')
        self.assertTrue(Path(self.ctx.document['archive']['root']).is_dir())
        value.retry()
        self.assertEqual(self.ctx.calls[-1],('operation.retryLocalSave',{'id':value.operation_id}))

    def test_active_recording_keeps_same_id_through_controls(self):
        self.ctx.fixture('app-profile')
        capture=Fixture(prepare_recording=lambda:{'recordingId':'owned-recording'})
        original=self.ctx.call
        self.ctx.call=lambda method,params=None:{'state':'ready'} if method=='recording.inspect' else original(method,params)
        with patch.dict(FACTORIES,{'recording-lifecycle':lambda s:capture}):value=self.ctx.fixture('tray-active-recording')
        value.pause();value.resume();value.stop()
        methods=[(m,p) for m,p in self.ctx.calls if m in ('recording.start','recording.pause','recording.resume','recording.stop')]
        self.assertEqual([m for m,p in methods],['recording.start','recording.pause','recording.resume','recording.stop'])
        self.assertTrue(all(p=={'id':'owned-recording'} for m,p in methods))

    def test_other_popup_retains_explicit_missing_grab_witness(self):
        self.ctx.fixture('app-profile');scope=self.ctx.manager.scope
        previous={'settings':{'title':'put.gd'}};self.ctx.profile['windows']=previous
        self.ctx.accessible=lambda query:[{'pid':42,'role':'popup menu','showing':True}]
        self.ctx.focus=Mock(return_value={})
        with patch.object(scope,'launch',return_value={'pid':42}):value=self.ctx.fixture('shortcut-other-menu')
        self.assertFalse(value.keyboard_grab_verified)
        value.focus();self.assertEqual(self.ctx.focus.call_args.kwargs,{'raise_window':False})
        self.ctx.manager.close();self.assertIs(self.ctx.profile['windows'],previous)

    def test_shortcut_recorder_uses_real_control_contract(self):
        observed=Fixture(recording_status=lambda:{'text':'Press shortcut… Release all keys'})
        self.ctx.view=Mock();self.ctx.use_catalog=Mock();self.ctx.click=Mock()
        original=self.ctx.call
        self.ctx.call=lambda method,params=None:{'settings':{'section':2}} if method=='ui.viewState' else original(method,params)
        with patch.dict(FACTORIES,{'settings-observations':lambda scope:observed}):value=self.ctx.fixture('shortcut-settings-recorder')
        self.assertIn('Press shortcut',value.start()['text'])
        self.assertEqual(self.ctx.click.call_args.args,('recordRegionShortcut',))

    def test_wayland_layout_change_blocks_without_xwayland_or_mutation(self):
        self.ctx.profile['session']='wayland'
        self.ctx.fixture('app-profile')
        with patch.object(self.ctx,'shell',wraps=self.ctx.shell) as shell:
            with self.assertRaisesRegex(RecipeBlocked,'no XWayland'):self.ctx.fixture('shortcut-keyboard-layout')
        # Cleanup may use shell after refusal; no layout command was sent.
        self.assertFalse(any('setxkbmap' in call.args[0] for call in shell.call_args_list))


if __name__=='__main__':unittest.main()
