"""Plasma guards and guest-only provisioning. Mocks are not desktop evidence."""
from pathlib import Path
import copy
import sys
import tempfile
import os
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as stage
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.fixtures import destinations
from tools.lab.surface_driver.api import RecipeBlocked
from tests.desktop.test_surface_fixtures import FixtureCase
from tests.desktop.test_surface_shell_health import ShellCrashDetector, shell


class PlasmaPick(unittest.TestCase):
    def setUp(self):
        self.node={'name':'put.gd','application':'plasmashell','role':'button','pid':70,
                   'rect':[962,754,22,38],'window_id':'panel','window':'Panel',
                   'a11y_bus':':1.7','a11y_path':'/tray'}
        self.window={'id':'panel','pid':70,'title':'Panel','content':[0,752,1280,48]}
        self.query={'desktop_node':self.node,'point':[973,773],'shell_application':'plasmashell*',
                    'compositor':{'kind':'kwin-script'},'desktop_hit_test':'kwin-shell-at-spi'}
        self.target=Mock();self.target.get_process_id.return_value=70
        self.top=Mock();self.top.name='Panel';self.top.queryComponent.return_value.getAccessibleAtPoint.return_value=self.target
        self.target.queryComponent.return_value.getAccessibleAtPoint.return_value=self.target
        self.app=Mock();self.app.get_process_id.return_value=70
        self.app.__iter__=Mock(return_value=iter([self.top]))
        self.patches=[patch.dict(sys.modules,pyatspi=SimpleNamespace(DESKTOP_COORDS=0)),
                      patch.object(probe,'bound_calls'),patch.object(probe,'plasma_pid',return_value=70),
                      patch.object(probe,'compositor_pick',return_value={'window':self.window}),
                      patch.object(probe,'RawNode',return_value=self.target),patch.object(probe,'a11y_bus'),
                      patch.object(probe,'accessible',side_effect=lambda q:[dict(self.node)]),
                      patch.object(probe,'desktop_root',return_value=[self.app]),
                      patch.object(probe,'extents',return_value=[0,0,1280,48])]
        for p in self.patches:p.start();self.addCleanup(p.stop)

    def test_surface_and_exact_component_pick_authorize_real_actor(self):
        self.assertTrue(probe.foreground(self.query)['ok'])
        self.top.queryComponent.return_value.getAccessibleAtPoint.assert_called_once_with(973,21,0)

    def test_foreign_or_different_same_process_surface_refuses_before_component_pick(self):
        for key,value in [('pid',71),('id','other-popup')]:
            with patch.object(probe,'compositor_pick',return_value={'window':dict(self.window,**{key:value})}):
                self.assertFalse(probe.foreground(self.query)['ok'])
        self.top.queryComponent.assert_not_called()

    def test_stale_bounds_name_bus_and_default_coordinate_guess_do_not_authorize(self):
        for row in [dict(self.node,rect=[962,750,22,38]),dict(self.node,a11y_path='/duplicate'),dict(self.node,window_id='other')]:
            with patch.object(probe,'accessible',return_value=[row]):
                self.assertFalse(probe.foreground(self.query)['ok'])
        with patch.object(probe,'plasma_pid',return_value=71):
            self.assertFalse(probe.foreground(self.query)['ok'])

    def test_same_process_cover_and_missing_pick_refuse(self):
        cover=Mock();cover.get_process_id.return_value=70
        cover.queryComponent.return_value.getAccessibleAtPoint.return_value=cover
        self.top.queryComponent.return_value.getAccessibleAtPoint.return_value=cover
        self.assertFalse(probe.foreground(self.query)['ok'])


class Provisioning(unittest.TestCase):
    def test_kde_profile_selects_kwin_shell_guard_and_crash_policy(self):
        profile=load_profile('kubuntu-kde-wayland')
        self.assertEqual(profile['focus']['desktop_hit_test'],'kwin-shell-at-spi')
        self.assertEqual(profile['shell_health']['process'],'plasmashell')
        # Plasma Qt extents must continue to be placed via clientGeometry.
        self.assertFalse(probe.WaylandPlacement(profile['compositor']).absolute('plasmashell'))

    def test_staging_authenticates_overlay_twice_and_never_enables_screen_reader(self):
        with patch.object(stage.lab,'verify_owned_guest') as verify,patch.object(stage.lab,'state',return_value={'profile':'kde-wayland'}),\
             patch.object(stage.lab,'load_profile',return_value={'session_process':'plasmashell','session':'wayland'}),\
             patch.object(stage.lab,'guest_session',return_value='metadata') as session:
            self.assertEqual(stage.stage('/owned'),'metadata');self.assertEqual(verify.call_count,2)
            self.assertIn("'IsEnabled',dbus.Boolean(True)",session.call_args.args[1])
            self.assertNotIn("'ScreenReaderEnabled',dbus.Boolean(True)",session.call_args.args[1])

    def test_foreign_profile_never_gets_mutated(self):
        with patch.object(stage.lab,'verify_owned_guest'),patch.object(stage.lab,'state',return_value={'profile':'other'}),\
             patch.object(stage.lab,'load_profile',return_value={'session_process':'gnome-shell','session':'wayland'}),\
             patch.object(stage.lab,'guest_session') as session:
            with self.assertRaises(stage.lab.LabError):stage.stage('/owned')
            session.assert_not_called()


class KDEKeyring(FixtureCase):
    def test_kde_creates_unique_wallet_and_scopes_backend_to_private_resident(self):
        scope=self.ctx.manager.scope;scope.initialize();self.ctx.profile['compositor']={'kind':'kwin-script'}
        scripts=[]
        with patch.object(scope,'json',side_effect=lambda s:scripts.append(s) or {'collection':'/owned','salt_device':1,'salt_inode':2}),\
             patch.object(scope,'python',side_effect=lambda s:scripts.append(s) or ''),patch.object(scope,'restart') as restart:
            self.ctx.fixture('fake-keyring');self.ctx.manager.close()
            restart.assert_called_once_with({'QTKEYCHAIN_BACKEND':'libsecret'})
        source='\n'.join(scripts)
        self.assertIn('org.kde.secretservicecompat',source);self.assertIn("'pamOpen'",source)
        self.assertIn('putgd-fixture-'+scope.token,source)
        self.assertNotIn('CreateWithMasterPassword',source)
        self.assertNotIn('SetAlias',source);self.assertNotIn('GetSecret',source)
        self.assertIn('(s.st_dev,s.st_ino)==(1,2)',source)
        self.assertIn("'Delete'",source)

    def test_physical_fixture_refuses_before_service_activation(self):
        self.ctx.placement='native';scope=self.ctx.manager.scope
        with patch.object(scope,'json') as call:
            with self.assertRaises(RecipeBlocked):destinations.fake_keyring(scope)
            call.assert_not_called()


class WalletScript(unittest.TestCase):
    def run_create(self, *, collision=False, resolve=True):
        from tools.lab.surface_driver.fixtures.keyring import KDE_CREATE
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        home=Path(self.temp.name);self.wallet='putgd-fixture-'+'a'*32
        state={'wallets':[self.wallet] if collision else []};calls=[]
        def call(path,iface,method,params,reply):
            calls.append((path,method,params))
            if method=='wallets':return (state['wallets'],)
            if method=='pamOpen':state['wallets']=[self.wallet];return ()
            if method=='deleteWallet':state['wallets']=[];return (0,)
            if method=='Get':
                return (['/collection/owned'] if resolve else [],) if params[1]=='Collections' else (self.wallet,)
            raise AssertionError('unexpected service operation: '+method)
        namespace=dict(bus=Mock(),Gio=SimpleNamespace(DBusCallFlags=SimpleNamespace(NONE=0)),
                       GLib=SimpleNamespace(Variant=lambda signature,value:value),call=call,
                       LABEL=self.wallet,Path=Path,os=os,service='/org/freedesktop/secrets')
        with patch.object(Path,'home',return_value=home):
            try:exec(KDE_CREATE,namespace)
            except AssertionError:
                self.assertTrue(collision or not resolve)
            else:self.assertFalse(collision or not resolve)
        return home,namespace,calls,state

    def test_native_wallet_creation_uses_new_salt_and_only_owned_label(self):
        home,namespace,calls,state=self.run_create()
        salt=home/'.local/share/kwalletd'/(self.wallet+'.salt')
        self.assertEqual(len(salt.read_bytes()),56);self.assertEqual(salt.stat().st_mode&0o777,0o600)
        self.assertEqual(namespace['collection'],'/collection/owned')
        self.assertTrue(all(args[0]==self.wallet for _,m,args in calls if m=='pamOpen'))
        self.assertFalse(any(m in ('SetAlias','ReadSecret','GetSecret') for _,m,_ in calls))

    def test_collision_refuses_without_creating_salt_or_opening_wallet(self):
        home,_,calls,_=self.run_create(collision=True)
        self.assertFalse((home/'.local/share/kwalletd').exists())
        self.assertFalse(any(m=='pamOpen' for _,m,_ in calls))

    def test_partial_creation_deletes_exact_wallet_and_salt(self):
        home,_,calls,state=self.run_create(resolve=False)
        self.assertEqual(state['wallets'],[])
        self.assertIn(('/ksecretd','deleteWallet',(self.wallet,)),calls)
        self.assertFalse((home/'.local/share/kwalletd'/(self.wallet+'.salt')).exists())


class PlasmaCrash(unittest.TestCase):
    def setUp(self):
        ShellCrashDetector.setUp(self)
        self.addCleanup(self.temp.cleanup)
        self.ctx.profile=load_profile('kubuntu-kde-wayland')

    def test_plasma_crash_refuses_shell_pointer_before_dispatch(self):
        self.ctx.shell_baseline();self.state=shell(200)
        with self.assertRaisesRegex(RecipeBlocked,'plasmashell pid 100 ended'):
            self.ctx._desktop_guard({'pid':100},[20,20])


if __name__=='__main__':unittest.main()
