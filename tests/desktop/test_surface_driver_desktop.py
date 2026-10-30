#!/usr/bin/env python3
"""Desktop observation contracts (menus, notifications, occlusion, Wayland geometry).

Fast host tests: no app, desktop, credentials, network or VM required.
"""
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver import probe


def row(name='',role='push button',objectName='',rect=(10,10,20,10),popup=None,window='put.gd'):
    return {'name':name,'role':role,'objectName':objectName,'rect':list(rect),'visible':True,'showing':True,
            'enabled':True,'popup':popup,'window':window,'pid':7}


class ControlMatching(unittest.TestCase):
    def resolve(self,rows,selector,candidates=(),**query):
        with patch.object(probe,'accessible',return_value=rows) as scan:
            result=probe.control_geometry({'selector':selector,'candidates':list(candidates),**query})
        return result,scan.call_args[0][0]

    def test_text_selector_matches_catalog_accessible_name_without_label_prefilter(self):
        # History's List button: catalog text "List", live accessible name differs.
        rows=[row('Show history as a list'),row('Show history as a grid')]
        candidates=[{'name':'','text':'List','accessibleName':'Show history as a list'}]
        result,filters=self.resolve(rows,'text:List',candidates)
        self.assertEqual(result['name'],'Show history as a list')
        self.assertNotIn('name',filters)

    def test_aliases_and_tooltip_are_labels(self):
        rows=[row('Search history'),row('Filters')]
        self.assertEqual(self.resolve(rows,'text:Search',[{'text':'Search','accessibleNames':['Search history']}])[0]['name'],'Search history')
        self.assertEqual(self.resolve(rows,'historyFilters',[{'name':'historyFilters','toolTip':'&Filters'}])[0]['name'],'Filters')

    def test_live_object_name_wins_over_same_label(self):
        rows=[row('Upload',objectName='sidebarUpload'),row('Upload',objectName='editorUpload',rect=(50,10,20,10))]
        result,_=self.resolve(rows,'editorUpload',[{'name':'editorUpload','text':'Upload'}])
        self.assertEqual(result['objectName'],'editorUpload')
        result,_=self.resolve(rows,'glob:editorUp*')
        self.assertEqual(result['objectName'],'editorUpload')

    def test_label_ambiguity_and_absence_fail_closed(self):
        rows=[row('Copy link'),row('Copy link',rect=(50,10,20,10))]
        with self.assertRaisesRegex(RuntimeError,r'\(2 matches\)'):
            self.resolve(rows,'text:Copy link')
        with self.assertRaisesRegex(RuntimeError,r'\(0 matches\)'):
            self.resolve(rows,'text:Delete')

    def test_open_menu_item_wins_over_same_label_button(self):
        menu={'name':'','role':'popup menu','rect':[0,0,100,100],'pid':7}
        rows=[row('Copy details'),row('Copy details',role='menu item',popup=menu)]
        self.assertEqual(self.resolve(rows,'text:Copy details')[0]['role'],'menu item')


if __name__=='__main__':
    unittest.main()
