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

    def test_held_drag_steps_hit_test_the_pointer_not_keyboard_focus(self):
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_pointer',return_value=(610,715,1<<8)),\
             patch.object(probe,'x11_hit',return_value={'path':['1','90'],'pid':7,'name':'bar'}) as hit:
            self.assertTrue(probe.x11_foreground({'geometry':self.bar})['ok'])
        self.assertEqual(hit.call_args[0][0],(610,715))
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_pointer',return_value=(10,10,1<<8)),\
             patch.object(probe,'x11_hit',return_value={'path':['1','33'],'pid':9,'name':'Panel'}):
            self.assertFalse(probe.x11_foreground({'geometry':self.bar})['ok'])

    def test_keyboard_needs_x_input_focus_inside_the_bar(self):
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_focus_owner',return_value='12'),\
             patch.object(probe,'x11_pointer',return_value=(0,0,0)),\
             patch.object(probe,'x11_ancestors',return_value=['12','1']),patch.object(probe,'run',return_value='Edit image'):
            result=probe.x11_foreground({'geometry':self.bar})
        self.assertFalse(result['ok']);self.assertIn("'Edit image'",result['obscuring_window'])
        with patch.object(probe,'owned',return_value=True),patch.object(probe,'x11_focus_owner',return_value='91'),\
             patch.object(probe,'x11_pointer',return_value=(0,0,0)),\
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

    def test_reader_reports_a_dead_monitor_and_reads_memory_over_a_private_socket(self):
        import os,socket,threading
        with tempfile.TemporaryDirectory() as directory:
            path=directory+'/notify.sock'
            self.assertFalse(probe.notification_read({'socket':path,'pid':2**22+7})['alive'])
            server=socket.socket(socket.AF_UNIX);server.bind(path);os.chmod(path,0o600);server.listen(1)
            def serve():
                connection,_=server.accept()
                with connection:connection.sendall(b'kept block')
            worker=threading.Thread(target=serve);worker.start()
            reply=probe.notification_read({'socket':path,'pid':os.getpid()})
            worker.join(5);server.close()
            self.assertEqual(reply,{'alive':True,'text':'kept block'})
            os.chmod(path,0o666)
            with self.assertRaises(RuntimeError):
                probe.notification_read({'socket':path,'pid':os.getpid()})

    def test_monitor_keeps_bodies_in_memory_never_a_file(self):
        import inspect
        source=inspect.getsource(probe.notification_monitor)
        import re
        self.assertIsNone(re.search(r'(?<![P.])open\(|os\.open\(|os\.write\(',source))
        self.assertIn('deque(',source)
        self.assertIn('popup_watch',source)
        self.assertIn('NotificationFilter',source)


class FakeNode(list):
    def __init__(self,name,role,rect,pid=50,children=()):
        super().__init__(children);self.name,self.role,self.rect,self.pid=name,role,rect,pid
        self.description='';self.path='/n/%x'%id(self);self.extent_reads=0
    def getRoleName(self):return self.role
    def get_process_id(self):return self.pid
    def getAttributes(self):return []
    def getState(self):
        from types import SimpleNamespace
        return SimpleNamespace(contains=lambda state:state in ('visible','showing','enabled'))
    def queryComponent(self):
        from types import SimpleNamespace
        def extents(_):
            self.extent_reads+=1;return self.rect
        return SimpleNamespace(getExtents=extents)
    def queryText(self):raise NotImplementedError
    def queryAction(self):raise NotImplementedError


class LazyWalk(unittest.TestCase):
    def test_only_candidates_and_matched_ancestry_read_extents(self):
        from types import SimpleNamespace
        label=FakeNode('Upload complete','label',[830,30,200,20])
        other=[FakeNode('Clock '+str(i),'label',[0,0,40,10]) for i in range(30)]
        banner=FakeNode('','notification',[780,10,480,80],children=[label])
        stage=FakeNode('','window',[0,0,1280,800],children=[banner,*other])
        app=FakeNode('cinnamon','application',None,children=[stage])
        desktop=FakeNode('main','desktop frame',[0,0,1280,800],children=[app])
        fake=SimpleNamespace(DESKTOP_COORDS=0,STATE_VISIBLE='visible',STATE_SHOWING='showing',STATE_ENABLED='enabled')
        with patch.dict(sys.modules,pyatspi=fake),patch.object(probe,'desktop_root',return_value=desktop):
            rows=probe.accessible({'application':'cinnamon*','name':'Upload complete','showing':True})
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['rect'],[830,30,200,20])
        self.assertEqual([a['rect'] for a in rows[0]['ancestors']][-2:],[[0,0,1280,800],[780,10,480,80]])
        self.assertTrue(all('_node' not in a for a in rows[0]['ancestors']))
        self.assertEqual(sum(n.extent_reads for n in other),0)


class PickNode(FakeNode):
    def __init__(self,name,role,rect,child=None):
        super().__init__(name,role,rect);self.child=child
    def queryComponent(self):
        from types import SimpleNamespace
        return SimpleNamespace(getExtents=lambda _:self.rect,getAccessibleAtPoint=lambda x,y,_:self.child)


class ShellChrome(unittest.TestCase):
    def desktop(self,child):
        from contextlib import ExitStack
        from types import SimpleNamespace
        stage=PickNode('','window',[0,0,1280,800],child)
        app=FakeNode('cinnamon','application',None,children=[stage])
        stack=ExitStack()
        stack.enter_context(patch.dict(sys.modules,pyatspi=SimpleNamespace(DESKTOP_COORDS=0,STATE_SHOWING='showing')))
        stack.enter_context(patch.object(probe,'desktop_root',return_value=[app]))
        return stack

    def test_banner_over_a_client_point_is_reported(self):
        banner=PickNode('','panel',[781,10,479,138],PickNode('','notification',[781,10,479,138],PickNode('Upload complete','label',[827,31,392,16])))
        with self.desktop(PickNode('','panel',[0,0,1280,800],banner)):
            cover=probe.shell_cover((1000,40),'cinnamon*')
        self.assertIn('notification',cover);self.assertIn("'Upload complete'",cover)

    def test_full_stage_container_is_not_chrome(self):
        with self.desktop(PickNode('','panel',[0,0,1280,800],PickNode('','panel',[0,0,1280,800]))):
            self.assertIsNone(probe.shell_cover((300,400),'cinnamon*'))

    def test_x11_guard_refuses_input_under_shell_chrome(self):
        target={'x':0,'y':0,'width':900,'height':600,'pid':7,'window_id':'10'}
        with patch.object(probe,'shell_cover',return_value='cinnamon notification at [781, 10, 479, 138]'):
            result=probe.x11_foreground({'geometry':target,'point':[850,140],'shell_cover':'cinnamon*'})
        self.assertFalse(result['ok']);self.assertIn('notification',result['obscuring_window'])


class LockedProbe(unittest.TestCase):
    def test_raising_probe_forces_the_lock_heartbeat(self):
        from tools.lab.surface_driver.target import Target, probe_mutates
        self.assertTrue(probe_mutates('foreground',{'raise':True}))
        self.assertFalse(probe_mutates('foreground',{'raise':False}))
        self.assertFalse(probe_mutates('accessible',{'raise':True}))
        target=Target.__new__(Target)
        calls=[];target.heartbeat=lambda force=False:calls.append(force)
        target._probe_failed=True
        for name,query in (('foreground',{'raise':True}),('geometry',{})):
            with self.assertRaises(Exception):
                target.probe(name,query,'source')
        self.assertEqual(calls,[True,False])


WITNESS={'kind':'gnome-lab-witness','shell_application':'gnome-shell*'}
FRAMES={'windows':[{'id':11,'title':'Edit image — put.gd','pid':50,'wm_class':'putgd','type':0,'frame':[224,70,900,657],
                    'focus':True,'minimized':False,'stack':3,'transient_for':None},
                   {'id':12,'title':'','pid':50,'wm_class':'putgd','type':9,'frame':[700,400,229,190],'focus':False,
                    'minimized':False,'stack':4,'transient_for':11}],'monitors':[{'rect':[0,0,1280,800],'scale':1}]}


class WaylandCoordinates(unittest.TestCase):
    def walk(self,rows):
        from types import SimpleNamespace
        canvas=FakeNode('Image being edited','filler',[0,108,659,456])
        top=FakeNode('Edit image — put.gd','filler',[0,0,900,620],children=[canvas])
        app=FakeNode('putgd-ui','application',None,children=[top])
        shell_label=FakeNode('Upload complete','label',[830,30,200,20],pid=40)
        shell=FakeNode('gnome-shell','application',None,children=[FakeNode('','window',[0,0,1280,800],pid=40,children=[shell_label])])
        desktop=FakeNode('main','desktop frame',[0,0,1280,800],children=[app,shell])
        fake=SimpleNamespace(DESKTOP_COORDS=0,STATE_VISIBLE='visible',STATE_SHOWING='showing',STATE_ENABLED='enabled')
        with patch.dict(sys.modules,pyatspi=fake),patch.dict('os.environ',{'XDG_SESSION_TYPE':'wayland'}),\
             patch.object(probe,'desktop_root',return_value=desktop),\
             patch.object(probe,'compositor_windows',return_value=FRAMES),patch.object(probe,'popup_roots',return_value=[]):
            return probe.accessible(rows)

    def test_client_rows_move_to_the_compositor_content_origin(self):
        # frame 900x657 vs content 900x620: 0 px sides, 37 px title bar.
        row=self.walk({'name':'Image being edited','compositor':WITNESS})[0]
        self.assertEqual(row['rect'],[224,70+37+108,659,456])
        self.assertEqual((row['coordinate_space'],row['window_id']),('desktop',11))
        self.assertEqual(row['ancestors'][-1]['rect'],[224,107,900,620])

    def test_shell_rows_are_already_desktop_coordinates(self):
        row=self.walk({'name':'Upload complete','compositor':WITNESS})[0]
        self.assertEqual(row['rect'],[830,30,200,20])

    def test_without_a_witness_wayland_rects_are_refused_not_guessed(self):
        row=self.walk({'name':'Image being edited'})[0]
        self.assertIsNone(row['rect']);self.assertEqual(row['coordinate_space'],'window-relative')
        self.assertIn('no compositor',row['coordinate_error'])

    def test_implausible_decorations_fail_loudly(self):
        placement=probe.WaylandPlacement(WITNESS)
        placement._windows=[dict(FRAMES['windows'][0],frame=[0,0,903,640])]
        with self.assertRaisesRegex(RuntimeError,'implausible decoration split'):
            placement.toplevel(50,'Edit image — put.gd',[0,0,900,620])
        placement._windows=[]
        with self.assertRaisesRegex(RuntimeError,'missing or ambiguous'):
            placement.toplevel(50,'Edit image — put.gd',[0,0,900,620])

    def test_popups_take_their_own_compositor_frame(self):
        placement=probe.WaylandPlacement(WITNESS);placement._windows=FRAMES['windows']
        where=placement.popup(50,[476,330,229,190])
        self.assertEqual(probe.shift([482,336,217,32],where),[706,406,217,32])

    def test_witness_foreground_names_the_panel_or_other_window(self):
        target={'window_id':11,'pid':50}
        with patch.dict('os.environ',{'XDG_SESSION_TYPE':'wayland'}):
            with patch.object(probe,'compositor_pick',return_value={'window':FRAMES['windows'][0],'chain':[]}):
                self.assertTrue(probe.foreground({'geometry':target,'point':[300,300],'compositor':WITNESS})['ok'])
            with patch.object(probe,'compositor_pick',return_value={'window':None,'chain':[{'name':'panel'},{'name':'panelBox'}]}):
                result=probe.foreground({'geometry':target,'point':[300,5],'compositor':WITNESS})
            self.assertFalse(result['ok']);self.assertEqual(result['obscuring_window'],'shell panel')
            other=dict(FRAMES['windows'][0],id=99,title='Core 2a obstruction',pid=77)
            with patch.object(probe,'compositor_pick',return_value={'window':other,'chain':[]}):
                self.assertEqual(probe.foreground({'geometry':target,'point':[300,300],'compositor':WITNESS})['obscuring_window'],'Core 2a obstruction')
            with patch.object(probe,'compositor_windows',return_value={'windows':[dict(other,focus=True),dict(FRAMES['windows'][0],focus=False)]}):
                self.assertFalse(probe.foreground({'geometry':target,'compositor':WITNESS})['ok'])

    def test_wayland_raise_activates_then_requires_compositor_focus(self):
        target={'window_id':11,'pid':50}
        states=iter([{'windows':[dict(FRAMES['windows'][0],focus=False)]}]+[{'windows':[FRAMES['windows'][0]]}]*3)
        with patch.dict('os.environ',{'XDG_SESSION_TYPE':'wayland'}),\
             patch.object(probe,'compositor_activate',return_value=True) as activate,\
             patch.object(probe,'compositor_windows',side_effect=lambda _:next(states)),patch.object(probe.time,'sleep'):
            self.assertTrue(probe.foreground({'geometry':target,'compositor':WITNESS,'raise':True})['ok'])
        activate.assert_called_once_with(WITNESS,11)


class StartupFocus(unittest.TestCase):
    """#750: right after mapping, _NET_ACTIVE_WINDOW can still be 0."""
    editor={'x':50,'y':50,'width':900,'height':620,'pid':7,'window_id':'10'}

    def settle(self,actives,owners):
        reads=iter(actives);calls=[]
        def run(*args):
            if args[:2]==('xdotool','getwindowpid'):
                if owners.get(args[2]) is None:raise probe.subprocess.CalledProcessError(1,args)
                return str(owners[args[2]])
            return ''
        with patch.object(probe,'x11_active',side_effect=lambda:next(reads)),patch.object(probe,'run',side_effect=run),\
             patch.object(probe.subprocess,'run',side_effect=lambda args,**kw:calls.append(args)),patch.object(probe.time,'sleep'):
            return probe.x11_settle(self.editor),calls

    def test_window_zero_activates_the_target_once_then_waits(self):
        ok,calls=self.settle(['0','0','0','10'],{})
        self.assertTrue(ok);self.assertEqual(calls,[['xdotool','windowactivate','--sync','10']])

    def test_same_process_window_is_replaced_by_the_target(self):
        ok,calls=self.settle(['20','20','10'],{'20':7})
        self.assertTrue(ok);self.assertEqual(len(calls),1)

    def test_a_foreign_active_window_is_never_displaced(self):
        ok,calls=self.settle(['30'],{'30':99})
        self.assertFalse(ok);self.assertEqual(calls,[])

    def test_bounded_when_nothing_settles(self):
        with patch.object(probe.time,'monotonic',side_effect=[0,0,1,2,3]):
            ok,calls=self.settle(['0']*10,{})
        self.assertFalse(ok);self.assertEqual(len(calls),1)

    def test_guard_requests_settle_on_x11_and_forces_heartbeat(self):
        from tools.lab.surface_driver.target import probe_mutates
        ctx,directory=context();self.addCleanup(directory.cleanup)
        ctx.geometry=lambda role:dict(self.editor,head=0,scale=1);ctx._current_window='editor'
        queries=[];ctx._probe=lambda name,query:queries.append(query) or {'ok':True}
        ctx._guard()
        self.assertTrue(queries[0]['settle']);self.assertTrue(probe_mutates('foreground',queries[0]))


if __name__=='__main__':
    unittest.main()
