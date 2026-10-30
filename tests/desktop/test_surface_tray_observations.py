"""Real tray/notification wire regressions; no desktop or provider needed."""
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver.context import parse_notifications
from tools.lab.surface_driver import probe


class Notifications(unittest.TestCase):
    def test_captured_cinnamon_dict_entry_without_space_preserves_hint_types(self):
        path=Path(__file__).with_name('fixtures')/'notifications/cinnamon-resident.dbus.txt'
        row,=parse_notifications(path.read_text())
        self.assertEqual(row['summary'],'Local save failed')
        self.assertIs(row['hints']['resident'],True)
        self.assertIsInstance(row['hints']['urgency'],int)
        self.assertEqual(row['hints']['urgency'],2)
        self.assertTrue(row['resident']);self.assertEqual(row['timeout'],0)
        self.assertEqual(row['id'],5);self.assertEqual(row['server'],':1.67')


class NotifierIdentity(unittest.TestCase):
    def test_exact_live_gnome_watcher_capture(self):
        capture=json.loads((Path(__file__).with_name('fixtures')/'tray/gnome-watcher.json').read_text())
        item=capture['registeredItems'][0]
        self.assertEqual(item,':1.37@/org/ayatana/NotificationItem/software_update_available')
        self.assertEqual(probe.notifier_item(item),(':1.37','/org/ayatana/NotificationItem/software_update_available'))

    def test_gnome_bus_at_object_path_and_kde_formats(self):
        self.assertEqual(probe.notifier_item(':1.40@/StatusNotifierItem'),(':1.40','/StatusNotifierItem'))
        self.assertEqual(probe.notifier_item(':1.40/StatusNotifierItem'),(':1.40','/StatusNotifierItem'))
        self.assertEqual(probe.notifier_item('org.kde.StatusNotifierItem-42-1'),('org.kde.StatusNotifierItem-42-1','/StatusNotifierItem'))
        for value in (':1.40@@/StatusNotifierItem',':1.40@','/StatusNotifierItem',':1.40/../foreign'):
            with self.assertRaises(ValueError):probe.notifier_item(value)

    def test_stale_and_foreign_items_do_not_hide_private_gnome_item(self):
        class DBusException(Exception):pass
        calls=[]
        class Interface:
            def Get(self,*args):return [':1.39@/StatusNotifierItem',':1.40@/StatusNotifierItem',':1.41/StatusNotifierItem']
            def GetConnectionUnixProcessID(self,name):
                calls.append(name)
                if name==':1.39':raise DBusException('stale owner')
                return 40 if name==':1.40' else 41
            def GetNameOwner(self,name):return name
            def GetAll(self,*args):return {'Id':'putgd','ToolTip':('',[],'put.gd','Idle')}
        fake=SimpleNamespace(DBusException=DBusException,SessionBus=lambda:SimpleNamespace(get_object=lambda *a:None),Interface=lambda *a:Interface())
        with patch.dict(sys.modules,dbus=fake),patch.object(probe,'owned',side_effect=lambda pid:pid==40),patch.object(probe,'notifier_process',return_value='owned-start'):
            rows=probe.tray({})
        self.assertEqual([row['service'] for row in rows],[':1.40'])
        self.assertTrue(all('@' not in name for name in calls))
        self.assertEqual(rows[0]['path'],'/StatusNotifierItem')

    def test_reused_pid_during_property_fetch_is_rejected(self):
        class Interface:
            def Get(self,*args):return [':1.40@/StatusNotifierItem']
            def GetNameOwner(self,name):return name
            def GetConnectionUnixProcessID(self,name):return 40
            def GetAll(self,*args):return {}
        fake=SimpleNamespace(DBusException=RuntimeError,SessionBus=lambda:SimpleNamespace(get_object=lambda *a:None),Interface=lambda *a:Interface())
        with patch.dict(sys.modules,dbus=fake),patch.object(probe,'owned',return_value=True),patch.object(probe,'notifier_process',side_effect=['old-start','new-start']):
            self.assertEqual(probe.tray({}),[])


if __name__=='__main__':unittest.main()
