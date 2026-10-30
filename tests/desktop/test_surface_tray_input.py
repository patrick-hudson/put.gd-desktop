"""Tray input regressions; fake witnesses do not establish desktop acceptance."""
import sys
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
sys.path.insert(0,str(Path(__file__).parent))
from test_surface_input_guards import context
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed


class DesktopGestures(unittest.TestCase):
    def ctx(self):
        ctx,directory=context({'session':'wayland'})
        self.addCleanup(directory.cleanup)
        ctx.accessible=Mock(return_value=[{'name':'put.gd','application':'gnome-shell','role':'menu',
            'pid':70,'enabled':True,'rect':[1800,0,36,32]}])
        ctx._probe=Mock(return_value={'ok':True})
        return ctx

    def test_double_click_has_one_live_resolution_and_guard_for_the_pair(self):
        ctx=self.ctx()
        ctx.double_click_accessible({'application':'gnome-shell*','name':'put.gd'})
        ctx.accessible.assert_called_once()
        ctx._probe.assert_called_once()
        self.assertEqual(ctx.target.pointer.call_count,2)
        self.assertEqual(ctx.target.release_pointer.call_count,2)
        self.assertEqual(ctx._click_count,1)

    def test_double_click_refuses_obstruction_without_input(self):
        ctx=self.ctx();ctx._probe.return_value={'ok':False,'obscuring_window':'foreign menu'}
        with self.assertRaisesRegex(RecipeFailed,'foreign menu'):
            ctx.double_click_accessible({'pid':70,'name':'put.gd'})
        ctx.target.pointer.assert_not_called()
        self.assertEqual(ctx._click_count,1)

    def test_release_failure_stops_the_pair_and_poisoning_is_preserved(self):
        ctx=self.ctx();ctx.target.release_pointer.side_effect=RecipeBlocked('release unconfirmed')
        with self.assertRaisesRegex(RecipeBlocked,'release unconfirmed'):
            ctx.double_click_accessible({'pid':70,'name':'put.gd'})
        self.assertEqual(ctx.target.pointer.call_count,1)
        self.assertTrue(ctx._input_poisoned)

    def test_wheel_uses_desktop_witness_and_releases_each_pulse(self):
        ctx=self.ctx()
        ctx.scroll_accessible({'pid':70,'name':'put.gd'},-2)
        self.assertEqual(ctx.target.pointer.call_count,2)
        self.assertEqual(ctx.target.release_pointer.call_count,2)
        self.assertTrue(all(call.args[5]=='wheel-down' for call in ctx.target.pointer.call_args_list))
        self.assertEqual(ctx._probe.call_count,2)

    def test_wheel_stops_when_an_obstruction_arrives(self):
        ctx=self.ctx();ctx._probe.side_effect=[{'ok':True},{'ok':False,'obscuring_window':'foreign window'}]
        with self.assertRaisesRegex(RecipeFailed,'foreign window'):
            ctx.scroll_accessible({'pid':70,'name':'put.gd'},2)
        self.assertEqual(ctx.target.pointer.call_count,1)
        ctx.target.release_pointer.assert_called_once()


class NativeGrab(unittest.TestCase):
    target={'x':0,'y':0,'width':1920,'height':1080,'pid':50,'window_id':'10','override_redirect':True}
    row={'name':'Capture region','role':'panel','pid':50,'rect':[0,0,1920,1080],'states':{'focused':True}}

    def check(self,rows,owned=True,hit=None):
        with patch.object(probe,'owned',return_value=owned),patch.object(probe,'native_capture_process',return_value=True),\
             patch.object(probe,'accessible',return_value=rows),patch.object(probe,'x11_pointer',return_value=(20,20,0)),\
             patch.object(probe,'x11_focus_owner',return_value='30'),patch.object(probe,'x11_ancestors',return_value=['30','1']),\
             patch.object(probe,'run',return_value='Main window'),patch.object(probe,'shell_cover',return_value=None),\
             patch.object(probe,'x11_hit',return_value=hit or {'path':['1','10'],'name':'selector','pid':50}):
            return probe.x11_unmanaged_foreground({'geometry':self.target,'keyboard_grab':True,'shell_cover':'cinnamon*'})

    def test_private_native_selector_focused_state_proves_its_grab(self):
        result=self.check([self.row])
        self.assertTrue(result['ok']);self.assertEqual(result['backend'],'native-selector-keyboard-grab')

    def test_stale_ambiguous_or_foreign_selector_never_authorizes_keyboard(self):
        for rows in ([],[self.row,self.row],[dict(self.row,pid=51)],[dict(self.row,states={'focused':False})]):
            self.assertFalse(self.check(rows)['ok'])
        self.assertFalse(self.check([self.row],owned=False)['ok'])
        self.assertFalse(self.check([self.row],hit={'path':['1','99'],'pid':60,'name':'foreign cover'})['ok'])


class ShellActorPick(unittest.TestCase):
    node={'name':'Settings','role':'label','role_id':28,'application':'gnome-shell','pid':70,'rect':[1650,140,90,24]}
    query={'desktop_node':node,'point':[1695,152],'shell_application':'gnome-shell*',
           'compositor':{'kind':'gnome-lab-witness'}}
    pick={'pid':70,'window':None,'chain':[],'shell_nodes':[{'text':'Settings','role':28,'accessible':'','rect':[1650,140,90,24]}]}

    def test_native_shell_actor_proves_label_above_the_client(self):
        with patch.object(probe,'compositor_pick',return_value=self.pick):
            self.assertTrue(probe.shell_witness_hit(self.query)['ok'])

    def test_foreign_window_pid_duplicate_name_or_wrong_rect_cannot_match(self):
        for pick in (dict(self.pick,pid=71),dict(self.pick,window={'id':99,'title':'Foreign window'}),
                     dict(self.pick,shell_nodes=[{'text':'Settings','rect':[20,20,90,24]}])):
            with patch.object(probe,'compositor_pick',return_value=pick):
                self.assertFalse(probe.shell_witness_hit(self.query)['ok'])
        with patch.object(probe,'compositor_pick',return_value=self.pick):
            query=dict(self.query,desktop_node=dict(self.node,application='imposter'))
            self.assertFalse(probe.shell_witness_hit(query)['ok'])


if __name__=='__main__':unittest.main()
