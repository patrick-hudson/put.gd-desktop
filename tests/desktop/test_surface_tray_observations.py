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
            def GetAll(self,*args):return {'Id':'putgd','ToolTip':('',[],'put.gd','Idle')}
        fake=SimpleNamespace(DBusException=DBusException,SessionBus=lambda:SimpleNamespace(get_object=lambda *a:None),Interface=lambda *a:Interface())
        with patch.dict(sys.modules,dbus=fake),patch.object(probe,'owned',side_effect=lambda pid:pid==40):
            rows=probe.tray({})
        self.assertEqual([row['service'] for row in rows],[':1.40'])
        self.assertTrue(all('@' not in name for name in calls))
        self.assertEqual(rows[0]['path'],'/StatusNotifierItem')


if __name__=='__main__':unittest.main()
