"""Plasma guards and guest-only provisioning. Mocks are not desktop evidence."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch
from types import SimpleNamespace

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.compositor import stage_plasma_accessibility as stage
from tools.lab.surface_driver.api import RecipeBlocked


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

    def test_popup_root_is_bound_by_identity_when_its_title_is_empty(self):
        self.node.update(window='',popup={'a11y_bus':':1.7','a11y_path':'/menu'})
        self.top.name='plasmashell';self.top.bus=':1.7';self.top.path='/menu'
        with patch.object(probe,'desktop_root',return_value=[]),patch.object(probe,'popup_roots',return_value=[((70,'/menu'),(self.top,2,'','plasmashell',None,[]))]):
            self.assertTrue(probe.foreground(self.query)['ok'])
        self.top.path='/other-menu'
        with patch.object(probe,'popup_roots',return_value=[((70,'/other-menu'),(self.top,2,'','plasmashell',None,[]))]):
            self.assertFalse(probe.foreground(self.query)['ok'])


class Provisioning(unittest.TestCase):
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


if __name__=='__main__':unittest.main()
