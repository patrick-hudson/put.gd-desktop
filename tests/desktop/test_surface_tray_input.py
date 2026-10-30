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
        with patch.object(probe,'compositor_pick',return_value=self.pick),patch.object(probe,'native_shell_role',return_value='label'):
            self.assertTrue(probe.shell_witness_hit(self.query)['ok'])

    def test_foreign_window_pid_duplicate_name_or_wrong_rect_cannot_match(self):
        for pick in (dict(self.pick,pid=71),dict(self.pick,window={'id':99,'title':'Foreign window'}),
                     dict(self.pick,shell_nodes=[{'text':'Settings','rect':[20,20,90,24]}])):
            with patch.object(probe,'compositor_pick',return_value=pick),patch.object(probe,'native_shell_role',return_value='label'):
                self.assertFalse(probe.shell_witness_hit(self.query)['ok'])
        with patch.object(probe,'compositor_pick',return_value=self.pick),patch.object(probe,'native_shell_role',return_value='label'):
            query=dict(self.query,desktop_node=dict(self.node,application='imposter'))
            self.assertFalse(probe.shell_witness_hit(query)['ok'])

    def test_real_menu_wire_roles_use_semantic_names_across_atk_and_atspi(self):
        node=dict(self.node,name='put.gd',role='menu',role_id=33,rect=[1780,0,36,32])
        pick=dict(self.pick,shell_nodes=[{'accessible':'put.gd','role':32,'rect':[1780,0,36,32]}])
        query=dict(self.query,desktop_node=node,point=[1798,16])
        library=Mock();library.atk_role_get_name.return_value=b'menu'
        with patch.object(probe,'compositor_pick',return_value=pick),\
             patch.object(probe.ctypes.util,'find_library',return_value='libatk-1.0.so.0'),\
             patch.object(probe.ctypes,'CDLL',return_value=library):
            self.assertTrue(probe.shell_witness_hit(query)['ok'])
        library.atk_role_get_name.assert_called_once_with(32)

    def test_same_text_and_bounds_with_another_native_role_is_refused(self):
        with patch.object(probe,'compositor_pick',return_value=self.pick),patch.object(probe,'native_shell_role',return_value='slider'):
            self.assertFalse(probe.shell_witness_hit(self.query)['ok'])


class NotificationTarget(unittest.TestCase):
    def test_named_action_is_hovered_and_rechecked_before_its_press(self):
        with patch('tools.lab.surface_driver.context.require_host_ocr'):
            ctx, directory = context({'session': 'x11', 'notifications': {
                'accessible': {'application': 'cinnamon*'}}})
        self.addCleanup(directory.cleanup)
        notice = {'id': 5, 'at': 1, 'sender': ':1.2', 'server': ':1.3', 'summary': 'Saved',
                  'actions': ['open', 'Open'], 'closed': None, 'action_events': []}
        ctx.notifications = lambda: [notice]
        hovered, reads = [], []
        def accessible(query):
            reads.append(query['name'])
            return [{'pid': 70, 'application': 'cinnamon', 'name': query['name'],
                     'role': 'label' if query['name'] == 'Saved' else 'push button',
                     'rect': [100, 20 if query['name'] == 'Saved' else 100, 100, 20],
                     'ancestors': [{'role': 'notification', 'rect': [80, 10, 300, 200]}]}]
        ctx.accessible = accessible
        def pointer(query, button, node=None):
            if button is None:
                hovered.append((node['name'], len(reads)))
            else:
                self.assertEqual(hovered[-1][0], 'Open', 'St action must process its own pointer enter before a press')
                self.assertGreater(len(reads), hovered[-1][1], 'resolve fresh bounds after entering the action')
                notice['action_events'] = [{'action': 'open'}]
        ctx._accessible_pointer = pointer
        ctx.wait = lambda callback, *a, **k: self.assertTrue(callback())
        with patch('tools.lab.surface_driver.context.time.sleep'):
            ctx.notification_action(5, 'open')

    def test_hover_layout_change_requires_a_fresh_stable_action_rectangle(self):
        with patch('tools.lab.surface_driver.context.require_host_ocr'):
            ctx, directory = context({'session': 'x11', 'notifications': {
                'accessible': {'application': 'cinnamon*'}}})
        self.addCleanup(directory.cleanup)
        notice = {'id': 5, 'at': 1, 'sender': ':1.2', 'server': ':1.3', 'summary': 'Saved',
                  'actions': ['open', 'Open'], 'closed': None, 'action_events': []}
        ctx.notifications = lambda: [notice]
        hovered, button_reads, clicked = [], [], []
        def accessible(query):
            if query['name'] == 'Saved':
                return [{'pid': 70, 'application': 'cinnamon', 'name': 'Saved', 'role': 'label',
                         'rect': [100, 40 if hovered else 20, 100, 20], 'ancestors': [
                             {'role': 'notification', 'rect': [80, 10, 300, 200]}]}]
            button_reads.append(True)
            return [{'pid': 70, 'application': 'cinnamon', 'name': 'Open', 'role': 'push button',
                     'rect': [100, 100 if len(button_reads) == 1 else 130, 100, 30]}]
        ctx.accessible = accessible
        def pointer(query, button, node=None):
            if button is None:
                hovered.append(True)
            else:
                clicked.append(node['rect'])
                notice['action_events'] = [{'action': 'open'}]
        ctx._accessible_pointer = pointer
        ctx.wait = lambda callback, *a, **k: self.assertTrue(callback())
        with patch('tools.lab.surface_driver.context.time.sleep'):
            ctx.notification_action(5, 'open')
        self.assertEqual(clicked, [[100, 130, 100, 30]])

    def test_notification_closed_during_hover_never_receives_a_click(self):
        with patch('tools.lab.surface_driver.context.require_host_ocr'):
            ctx, directory = context({'session': 'x11', 'notifications': {
                'accessible': {'application': 'cinnamon*'}}})
        self.addCleanup(directory.cleanup)
        notice = {'id': 5, 'at': 1, 'sender': ':1.2', 'server': ':1.3', 'summary': 'Saved',
                  'actions': ['default', 'Open'], 'closed': None, 'action_events': []}
        ctx.notifications = lambda: [notice]
        ctx.accessible = lambda query: [{'pid': 70, 'application': 'cinnamon', 'name': 'Saved',
                                        'role': 'label', 'rect': [100, 20, 100, 20]}]
        def pointer(query, button, node=None):
            self.assertIsNone(button, 'closed notification must never receive a native click')
            notice['closed'] = {'reason': 2}
        ctx._accessible_pointer = pointer
        with patch('tools.lab.surface_driver.context.time.sleep'):
            with self.assertRaisesRegex(RecipeBlocked, 'generation changed'):
                ctx.notification_action(5)


if __name__=='__main__':unittest.main()
