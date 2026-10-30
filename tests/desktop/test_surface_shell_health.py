#!/usr/bin/env python3
"""Cinnamon ATK-bridge crash (#814): raw AT-SPI reads and the shell crash detector.

Fast host tests: no app, desktop, credentials, network or VM required.
"""
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver import probe
from tools.lab.surface_driver.api import Recipe, RecipeBlocked
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import run_recipes
try:
    from PIL import Image
except ImportError:
    Image=None


class FakeTarget:
    root='/tmp/sd-test';socket=root+'/runtime/putgd/session.sock';physical=None
    def screenshot(self,path,head=0):
        Image.new('RGB',(20,10),'white').save(path)
    def cli(self,*args,timeout=60):
        return {'returncode':0,'stdout':'{"ok":true,"result":{"ready":true}}','stderr':''}
    def execute(self,script,timeout=60):
        return {'returncode':0,'stdout':'','stderr':''}
    def key(self,codes):pass

ROOT_PATH=probe.ROOT_PATH
STATES=SimpleNamespace(STATE_VISIBLE=30,STATE_SHOWING=25,STATE_ENABLED=8,STATE_FOCUSED=12,DESKTOP_COORDS=0)
SHOWN=[(1<<8)|(1<<25)|(1<<30),0]


class FakeBus:
    """An a11y bus: registry desktop, put.gd (pid 7, owned) and the shell (pid 40)."""
    def __init__(self):
        self.calls=[]
        self.pids={':1.5':7,':1.9':40,'org.a11y.atspi.Registry':1}
        self.nodes={
            ('org.a11y.atspi.Registry',ROOT_PATH):dict(role='desktop frame',name='main',children=[(':1.5',ROOT_PATH),(':1.9',ROOT_PATH)]),
            (':1.5',ROOT_PATH):dict(role='application',name='putgd-ui',children=[(':1.5','/w')]),
            (':1.5','/w'):dict(role='frame',name='put.gd',children=[(':1.5','/b')],extents=(0,0,800,600)),
            (':1.5','/b'):dict(role='push button',name='Upload',extents=(10,20,80,30),interfaces=['org.a11y.atspi.Action']),
            (':1.9',ROOT_PATH):dict(role='application',name='cinnamon',children=[(':1.9','/stage')]),
            (':1.9','/stage'):dict(role='window',name='',children=[(':1.9','/banner')],extents=(0,0,1280,800),pick=(':1.9','/banner')),
            (':1.9','/banner'):dict(role='notification',name='Upload complete',extents=(780,10,480,80),pick=(':1.9','/banner')),
        }

    def get_object(self,bus,path,introspect=False):
        return FakeProxy(self,str(bus),str(path))


class FakeProxy:
    def __init__(self,bus,name,path):self.bus,self.name,self.path=bus,name,path
    def get_dbus_method(self,method,interface):
        def call(*args,**kwargs):
            self.bus.calls.append((self.name,self.path,interface,method))
            if interface=='org.a11y.atspi.Cache':
                raise AssertionError('Cache.'+method+' must never be called (#814)')
            if interface=='org.freedesktop.DBus':
                return self.bus.pids[args[0]]
            node=self.bus.nodes[(self.name,self.path)]
            if method=='Get':
                return {'Name':node.get('name',''),'Description':'','ChildCount':len(node.get('children',[])),
                        'NActions':1}.get(args[1])
            return {'GetRole':lambda:0,'GetRoleName':lambda:node['role'],'GetState':lambda:SHOWN,
                    'GetAttributes':lambda:{},'GetChildren':lambda:node.get('children',[]),
                    'GetInterfaces':lambda:node.get('interfaces',[]),'GetKeyBinding':lambda:'',
                    'GetExtents':lambda:node.get('extents',(0,0,0,0)),
                    'GetAccessibleAtPoint':lambda:node.get('pick',('','/org/a11y/atspi/null'))}[method]()
        return call


class RawAccessibility(unittest.TestCase):
    def setUp(self):
        self.bus=FakeBus()
        probe._A11Y.clear()
        self.patches=[patch.dict(sys.modules,pyatspi=STATES),patch.object(probe,'a11y_bus',return_value=self.bus),
                      patch.object(probe,'role_name',side_effect=lambda role,fallback:fallback()),
                      patch.object(probe,'owned',side_effect=lambda pid:pid==7),
                      patch.object(probe,'popup_roots',return_value=[])]
        for p in self.patches:p.start()
    def tearDown(self):
        for p in self.patches:p.stop()
        probe._A11Y.clear()

    def test_owned_walk_never_contacts_the_shell(self):
        rows=probe.accessible({'owned':True,'name':'Upload','visible':True})
        self.assertEqual([(r['name'],r['window'],r['rect']) for r in rows],[('Upload','put.gd',[10,20,80,30])])
        self.assertFalse([c for c in self.bus.calls if c[0]==':1.9'])
        self.assertFalse([c for c in self.bus.calls if c[2]=='org.a11y.atspi.Cache'])

    def test_shell_walk_reads_live_nodes_without_the_bridge_cache(self):
        rows=probe.accessible({'application':'cinnamon*','name':'Upload complete'})
        self.assertEqual(rows[0]['role'],'notification')
        self.assertTrue([c for c in self.bus.calls if c[0]==':1.9'])
        self.assertFalse([c for c in self.bus.calls if c[3]=='GetItems'])

    def test_shell_pick_is_a_component_call(self):
        query={'desktop_node':{'application':'cinnamon','name':'Upload complete','role':'notification','pid':40},
               'point':[900,40],'shell_application':'cinnamon*'}
        self.assertTrue(probe.shell_hit(query)['ok'])
        self.assertIn('notification',probe.shell_cover((900,40),'cinnamon*'))
        self.assertFalse([c for c in self.bus.calls if c[2]=='org.a11y.atspi.Cache'])

    def test_probe_source_does_not_use_libatspi_objects(self):
        source=Path(probe.__file__).read_text()
        for forbidden in ('Registry.getDesktop','registerEventListener','Registry.start'):
            self.assertNotIn(forbidden,source)


def shell(pid,start=500,windows=(),cores=(),dumping=False):
    return {'process':'cinnamon','instances':[{'pid':pid,'start':start,'dumping':dumping}] if pid else [],
            'crash_windows':list(windows),'cores':list(cores)}


class ShellCrashDetector(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
        self.env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
        self.ctx=DriverContext(FakeTarget(),self.env,load_profile(self.env['desktop']),self.path,self.path)
        self.state=shell(100);self.queries=[]
        original=self.ctx._probe
        def fake(name,query):
            if name=='shell_state':
                self.queries.append(query);return self.state
            return original(name,query)
        self.ctx._probe=fake
    def tearDown(self):self.temp.cleanup()

    def test_profile_declares_the_cinnamon_shell(self):
        self.assertEqual(load_profile('mint-cinnamon-x11')['shell_health']['process'],'cinnamon')

    def test_baseline_then_crash_blocks_input(self):
        self.assertEqual(self.ctx.shell_baseline()['pid'],100)
        self.assertIsNone(self.ctx.shell_failure())
        self.state=shell(200,windows=[{'window_id':'33554440','name':'Cinnamon just crashed'}],cores=[{'pid':100,'signal':11}])
        failure=self.ctx.shell_failure(full=False)
        for part in ('cinnamon pid 100 ended (now: 200)','Cinnamon just crashed','dumped core (signal 11)'):
            self.assertIn(part,failure)
        with self.assertRaises(RecipeBlocked) as raised:
            self.ctx._guard(window='settings',geometry={'x':0,'y':0,'width':10,'height':10,'pid':7,'window_id':'1'})
        self.assertEqual(raised.exception.issue,814)
        self.assertFalse(self.queries[-1]['full'])

    def test_restart_with_the_same_pid_number_is_still_a_crash(self):
        self.ctx.shell_baseline()
        self.state=shell(100,start=900)
        self.assertIn('ended',self.ctx.shell_failure())

    def test_a_shell_still_writing_its_core_has_crashed(self):
        self.ctx.shell_baseline()
        self.state=shell(100,dumping=True)
        self.assertIn('cinnamon pid 100 is dumping core',self.ctx.shell_failure(full=False))

    def test_run_refuses_a_shell_already_showing_a_crash(self):
        self.state=shell(100,windows=[{'window_id':'5','name':'Cinnamon just crashed'}])
        with self.assertRaisesRegex(RecipeBlocked,'already shows a crash notice'):
            self.ctx.shell_baseline()

    def test_boundary_check_has_its_own_budget(self):
        import time
        from tools.lab.surface_driver.run import shell_check
        self.ctx.shell_baseline()
        self.ctx._deadline=time.monotonic()-1  # the previous recipe overran
        self.assertIsNone(shell_check(self.ctx))
        # A lost probe channel is not a crash; later probes block with their own reason.
        self.ctx._probe=lambda name,query:(_ for _ in ()).throw(RecipeBlocked('persistent desktop probe failed'))
        self.assertIsNone(shell_check(self.ctx))

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_crash_during_a_recipe_stops_the_run(self):
        self.ctx.shell_baseline()
        ran=[]
        def crashes(ctx):
            ran.append('first');self.state=shell(200)
        def following(ctx):ran.append('second')
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),crashes),Recipe(('editor.redo',),following)],
                         self.env,self.path,'shell',self.path/'coverage.json')
        self.assertEqual(ran,['first'])
        self.assertEqual([(r.functional,r.issue) for r in rows],[('blocked',814),('blocked',814)])
        self.assertIn('recipe result on the crashed desktop: passed',rows[0].note)
        self.assertEqual(rows[1].frames,[])
        self.assertIn('desktop shell crashed during the run',rows[1].note)


if __name__=='__main__':
    unittest.main()
