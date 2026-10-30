"""Plasma guards and guest-only provisioning. Mocks are not desktop evidence."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as stage
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.fixtures import destinations
from tools.lab.surface_driver.api import RecipeBlocked
from tests.desktop import test_surface_shell_health as shell_tests


class PlasmaPick(unittest.TestCase):
    def setUp(self):
        self.pid_api=probe.plasma_pid
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
                      patch.object(probe,'desktop_root',return_value=[self.app]),patch.object(probe,'popup_roots',return_value=[]),
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
        for row in [dict(self.node,name='changed'),dict(self.node,role='label'),dict(self.node,rect=[962,750,22,38]),dict(self.node,a11y_path='/duplicate'),dict(self.node,window_id='other')]:
            with patch.object(probe,'accessible',return_value=[row]):
                self.assertFalse(probe.foreground(self.query)['ok'])
        with patch.object(probe,'plasma_pid',return_value=71):
            self.assertFalse(probe.foreground(self.query)['ok'])

    def test_same_process_cover_and_missing_pick_refuse(self):
        cover=Mock();cover.get_process_id.return_value=70
        cover.queryComponent.return_value.getAccessibleAtPoint.return_value=cover
        self.top.queryComponent.return_value.getAccessibleAtPoint.return_value=cover
        self.assertFalse(probe.foreground(self.query)['ok'])


    def test_shell_pid_uses_the_bounded_bus_daemon_api(self):
        daemon=Mock();daemon.GetConnectionUnixProcessID.return_value=70
        dbus=SimpleNamespace(SessionBus=Mock(return_value=Mock()),Interface=Mock(return_value=daemon))
        with patch.dict(sys.modules,dbus=dbus):
            self.assertEqual(self.pid_api(),70)
        daemon.GetConnectionUnixProcessID.assert_called_once_with('org.kde.plasmashell',timeout=3)


    def test_plasma_showing_popups_require_the_exact_live_shell_pid(self):
        query={'compositor':{'kind':'kwin-script'}}
        self.assertTrue(probe.plasma_popup_sender(70,'showing',query))
        self.assertFalse(probe.plasma_popup_sender(71,'showing',query))
        self.assertFalse(probe.plasma_popup_sender(70,'focused',query))
        self.assertFalse(probe.plasma_popup_sender(70,'showing',{}))

    def test_out_of_tree_menu_pick_still_requires_exact_actor_and_surface(self):
        with patch.object(probe,'desktop_root',return_value=[]),patch.object(probe,'popup_roots',return_value=[((70,'/menu'),(self.top,2,'','plasmashell',None,[]))]):
            self.assertTrue(probe.foreground(self.query)['ok'])


    def test_popup_root_is_bound_by_identity_when_its_title_is_empty(self):
        self.node.update(window='',popup={'a11y_bus':':1.7','a11y_path':'/menu'})
        self.top.name='plasmashell';self.top.bus=':1.7';self.top.path='/menu'
        with patch.object(probe,'desktop_root',return_value=[]),patch.object(probe,'popup_roots',return_value=[((70,'/menu'),(self.top,2,'','plasmashell',None,[]))]):
            self.assertTrue(probe.foreground(self.query)['ok'])
        self.top.path='/other-menu'
        with patch.object(probe,'popup_roots',return_value=[((70,'/other-menu'),(self.top,2,'','plasmashell',None,[]))]):
            self.assertFalse(probe.foreground(self.query)['ok'])


class Provisioning(unittest.TestCase):
    def test_kde_profile_selects_kwin_shell_guard_and_crash_policy(self):
        profile=load_profile('kubuntu-kde-wayland')
        self.assertEqual(profile['focus']['desktop_hit_test'],'kwin-shell-at-spi')
        self.assertEqual(profile['shell_health']['process'],'plasmashell')
        # Plasma Qt extents must continue to be placed via clientGeometry.
        self.assertFalse(probe.WaylandPlacement(profile['compositor']).absolute('plasmashell'))

    def test_icon_selector_excludes_the_same_named_plasma_task_button(self):
        selector=load_profile('kubuntu-kde-wayland')['tray']['icon']
        sni={'name':'put.gd','role':'button','application':'plasmashell','objectName':''}
        task=dict(sni,objectName='Task_QMLTYPE_358')
        self.assertTrue(probe.matches(sni,selector))
        self.assertFalse(probe.matches(task,selector))

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


class IsolatedSecretService(unittest.TestCase):
    def scope(self,owners):
        ctx=SimpleNamespace(placement='boat',profile={'guest':'kde-wayland'},
                            shell=Mock(return_value={'returncode':0}),wait=lambda ready,*a,**k:ready())
        return SimpleNamespace(ctx=ctx,root='/tmp/private/fx-owned',json=Mock(side_effect=owners),
                               initialize=Mock(),python=Mock(),launch=Mock(return_value={'pid':42,'start':'100'}),restart=Mock())

    def test_unowned_name_gets_private_daemon_storage_and_scoped_backend(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([{'owner':None,'pid':None},{'owner':':1.42','pid':42}])
        self.assertEqual(isolated_service(scope),{'pid':42,'start':'100'})
        args=scope.launch.call_args
        self.assertIn('--foreground',args.args[0]);self.assertIn('--components=secrets',args.args[0])
        self.assertTrue(all(p.startswith(scope.root+'/secret-service/') for p in args.kwargs['environment'].values()))
        scope.restart.assert_called_once_with({'QTKEYCHAIN_BACKEND':'libsecret','KDE_SESSION_VERSION':''})
        self.assertFalse(any('SetAlias' in call.args[0] for call in scope.json.call_args_list))

    def test_existing_service_is_preserved_before_launch_or_storage_mutation(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([{'owner':':1.99','pid':99}])
        with self.assertRaisesRegex(RecipeBlocked,'already owned'):isolated_service(scope)
        scope.launch.assert_not_called();scope.python.assert_not_called();scope.restart.assert_not_called()

    def test_racing_foreign_owner_never_receives_fixture_items(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([{'owner':None,'pid':None},{'owner':':1.99','pid':99}])
        with self.assertRaisesRegex(RecipeBlocked,'another process'):isolated_service(scope)
        scope.restart.assert_not_called()

    def test_physical_session_refuses_before_bus_access(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([]);scope.ctx.placement='native'
        with self.assertRaisesRegex(RecipeBlocked,'disposable'):isolated_service(scope)
        scope.json.assert_not_called();scope.launch.assert_not_called()

    def test_missing_distro_daemon_reports_overlay_provisioning(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([{'owner':None,'pid':None}]);scope.ctx.shell.return_value={'returncode':1}
        with self.assertRaisesRegex(RecipeBlocked,'keyring-overlay'):isolated_service(scope)
        scope.launch.assert_not_called()


    def test_daemon_control_socket_is_checked_before_launch(self):
        from tools.lab.surface_driver.fixtures.keyring import isolated_service
        scope=self.scope([{'owner':None,'pid':None}]);scope.root='/tmp/'+'é'*60
        with self.assertRaisesRegex(RecipeBlocked,'socket'):isolated_service(scope)
        scope.launch.assert_not_called();scope.python.assert_not_called()


class PlasmaCrash(unittest.TestCase):
    def setUp(self):
        shell_tests.ShellCrashDetector.setUp(self)
        self.addCleanup(self.temp.cleanup)
        self.ctx.profile=load_profile('kubuntu-kde-wayland')

    def test_plasma_crash_refuses_shell_pointer_before_dispatch(self):
        self.ctx.shell_baseline();self.state=shell_tests.shell(200)
        with self.assertRaisesRegex(RecipeBlocked,'plasmashell pid 100 ended'):
            self.ctx._desktop_guard({'pid':100},[20,20])


if __name__=='__main__':unittest.main()
