#!/usr/bin/env python3
"""Desktop observation contracts (menus, notifications, occlusion, Wayland geometry).

Fast host tests: no app, desktop, credentials, network or VM required.
"""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile


class FakeTarget:
    root='/tmp/sd-test';socket=root+'/runtime/putgd/session.sock';physical=None;monitor_pid=None
    def __init__(self):self.sent=[]
    def pointer(self,*args,**kwargs):self.sent.append(('pointer',args))
    def key(self,codes):self.sent.append(('key',codes))
    def execute(self,script,timeout=60):return {'returncode':0,'stdout':'','stderr':''}


def context(desktop='mint-cinnamon-x11'):
    directory=tempfile.TemporaryDirectory()
    ctx=DriverContext(FakeTarget(),dict(desktop=desktop,host='boat',placement='boat',package='deb',destination='local'),
                      load_profile(desktop),directory.name,directory.name)
    ctx._dimensions=lambda head=0:(1280,800)
    return ctx,directory


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

    def test_newer_at_spi_button_role_is_push_button(self):
        self.assertEqual(probe.normalize_role('button'),'push button')
        self.assertEqual(probe.normalize_role('push_button'),'push button')
        live=dict(row('Upload',role=probe.normalize_role('button')),states={},text='',description='',application='putgd',
                  owned=True)
        self.assertTrue(probe.matches(live,{'role':'button'}))
        self.assertTrue(probe.matches(live,{'role':'push button'}))
        self.assertEqual(probe.selector_matches(live,'class:QPushButton',[]),1)


XWININFO="""xwininfo: Window id: 0x3a00007 "About put.gd"
  Absolute upper-left X:  410
  Absolute upper-left Y:  354
  Relative upper-left X:  0
  Relative upper-left Y:  32
  Width: 460
  Height: 292
  Map State: IsViewable
  Override Redirect State: %s
"""


class X11Geometry(unittest.TestCase):
    def test_client_origin_comes_from_absolute_server_coordinates(self):
        # xdotool reported Y=394 for this reparented native dialog (title bar
        # counted twice); the server's absolute origin is y=354.
        def run(*args):
            if args[0]=='xwininfo':return XWININFO%'no'
            if args[:2]==('xprop','-root'):return 'window id # 0x3a00007'
            if args[1]=='getwindowpid':return '7'
            if args[1]=='getwindowname':return 'About put.gd'
            if args[-1]=='_NET_FRAME_EXTENTS':return '_NET_FRAME_EXTENTS(CARDINAL) = 0, 0, 32, 0'
            return ''
        from types import SimpleNamespace
        with patch.object(probe.subprocess,'run',return_value=SimpleNamespace(stdout='60817415')),\
             patch.object(probe,'run',side_effect=run),patch.object(probe,'owned',return_value=True):
            window=probe.x11_windows({'title':'About'})[0]
        self.assertEqual((window['x'],window['y'],window['width'],window['height']),(410,354,460,292))
        self.assertEqual(window['frame'],{'x':410,'y':322,'width':460,'height':324})
        self.assertFalse(window['override_redirect'])


class UnmanagedForeground(unittest.TestCase):
    bar={'x':500,'y':700,'width':300,'height':40,'pid':7,'window_id':'90','override_redirect':True}

    def test_pointer_input_requires_the_bar_in_the_server_hit_path(self):
        with patch.object(probe,'owned',return_value=True),\
             patch.object(probe,'x11_hit',return_value={'path':['1','90'],'pid':7,'name':'bar'}):
            self.assertTrue(probe.x11_foreground({'geometry':self.bar,'point':[600,720]})['ok'])
        with patch.object(probe,'owned',return_value=True),\
             patch.object(probe,'x11_hit',return_value={'path':['1','44'],'pid':9,'name':'Update Manager'}):
            result=probe.x11_foreground({'geometry':self.bar,'point':[600,720]})
        self.assertFalse(result['ok']);self.assertEqual(result['obscuring_window'],'Update Manager')

    def test_keyboard_needs_x_input_focus_inside_the_bar(self):
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_focus_owner',return_value='12'),\
             patch.object(probe,'x11_ancestors',return_value=['12','1']),patch.object(probe,'run',return_value='Edit image'):
            result=probe.x11_foreground({'geometry':self.bar})
        self.assertFalse(result['ok']);self.assertIn("'Edit image'",result['obscuring_window'])
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_focus_owner',return_value='91'),\
             patch.object(probe,'x11_ancestors',return_value=['91','90','1']):
            self.assertTrue(probe.x11_foreground({'geometry':self.bar})['ok'])

    def test_unowned_unmanaged_window_is_never_a_target(self):
        with patch.object(probe,'owned',return_value=False):
            self.assertFalse(probe.x11_foreground({'geometry':self.bar,'point':[600,720]})['ok'])

    def test_focus_selects_an_unmanaged_bar_by_centre_hit_test(self):
        ctx,directory=context();self.addCleanup(directory.cleanup)
        ctx.geometry=lambda role:dict(self.bar,head=0,scale=1)
        queries=[];ctx._probe=lambda name,query:queries.append(query) or {'ok':True}
        ctx.focus('bar')
        self.assertEqual(queries[0]['point'],(650,720))


class WithheldInput(unittest.TestCase):
    def test_desktop_pointer_paths_refuse_after_unconfirmed_release(self):
        ctx,directory=context();self.addCleanup(directory.cleanup)
        ctx._input_poisoned=True
        ctx.accessible=lambda query:[{'name':'Notice','role':'label','pid':8,'enabled':True,'rect':[10,10,80,20]}]
        ctx._probe=lambda name,query:{'ok':True}
        for action in (lambda:ctx.click_accessible({'application':'cinnamon','name':'Notice'}),
                       lambda:ctx.hover_accessible({'application':'cinnamon','name':'Notice'})):
            with self.assertRaisesRegex(RecipeBlocked,'input is withheld'):
                action()
        self.assertEqual(ctx.target.sent,[])


def notify(sender,serial,app='put.gd',summary='Done'):
    return (f'method call time=1.0 sender={sender} -> destination=:1.10 serial={serial} path=/org/freedesktop/Notifications; '
            f'interface=org.freedesktop.Notifications; member=Notify\n   string "{app}"\n   uint32 0\n   string "putgd"\n'
            f'   string "{summary}"\n   string "Body"\n')


def reply(destination,serial,identity):
    return f'method return time=1.1 sender=:1.10 -> destination={destination} serial=90 reply_serial={serial}\n   uint32 {identity}\n'


def signal(member,identity,sender=':1.10'):
    return (f'signal time=1.2 sender={sender} -> destination=(null destination) serial=91 path=/org/freedesktop/Notifications; '
            f'interface=org.freedesktop.Notifications; member={member}\n   uint32 {identity}\n   string "default"\n')


class NotificationPrivacy(unittest.TestCase):
    def setUp(self):
        self.filter=probe.NotificationFilter(':1.10',lambda sender:sender==':1.42')

    def test_only_private_family_traffic_is_kept(self):
        kept=[b for b in (notify(':1.42',7),reply(':1.42',7,5),signal('ActionInvoked',5),signal('NotificationClosed',5))
              if self.filter.keep(b)]
        self.assertEqual(len(kept),4)

    def test_other_applications_never_reach_the_log(self):
        # Another client's Notify (even if it calls itself put.gd), its reply,
        # and the server's broadcast signals for its ID are all dropped.
        for block in (notify(':1.77',3),notify(':1.77',4,app='Software Updater'),reply(':1.77',3,9),
                      signal('ActionInvoked',9),signal('NotificationClosed',9),
                      reply(':1.42',99,6),signal('ActionInvoked',5,sender=':1.55')):
            self.assertFalse(self.filter.keep(block),block)

    def test_closed_ids_stop_matching(self):
        self.filter.keep(notify(':1.42',7));self.filter.keep(reply(':1.42',7,5))
        self.assertTrue(self.filter.keep(signal('NotificationClosed',5)))
        self.assertFalse(self.filter.keep(signal('ActionInvoked',5)))

    def test_parsed_log_still_correlates(self):
        from tools.lab.surface_driver.context import parse_notifications
        text=''.join(b for b in (notify(':1.42',7),reply(':1.42',7,5),signal('ActionInvoked',5)) if self.filter.keep(b))
        rows=parse_notifications(text)
        self.assertEqual([(r['id'],r['summary'],[e['action'] for e in r['action_events']]) for r in rows],[(5,'Done',['default'])])

    def test_reader_reports_a_dead_monitor(self):
        with tempfile.NamedTemporaryFile() as log:
            self.assertFalse(probe.notification_log({'log':log.name,'pid':2**22+7})['alive'])
            import os
            self.assertTrue(probe.notification_log({'log':log.name,'pid':os.getpid()})['alive'])


if __name__=='__main__':
    unittest.main()
