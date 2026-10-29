#!/usr/bin/env python3
"""Fast host contracts: no app, desktop, credentials, network or VM required."""
import dataclasses
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed, ReceiptRow
from tools.lab.surface_driver.catalog import Catalog, compare, control_point
from tools.lab.surface_driver.context import DriverContext, parse_notifications
from tools.lab.surface_driver.profiles import load_profile, merge
from tools.lab.surface_driver.receipts import write_receipts
from tools.lab.surface_driver.run import discover, main, run_recipes, validate_recipes
from tools.lab.surface_driver.target import Target, checked_socket, qcodes
from tools.lab.surface_map import surface_map
try:
    from PIL import Image
except ImportError:
    Image=None


class Profiles(unittest.TestCase):
    def test_all_model_desktops(self):
        for row in json.loads((ROOT/'tools/lab/surface_map/model.json').read_text())['desktops']:
            p=load_profile(row['id'])
            for key in ('input_backend','catalog_tolerance','notification_server','default_browser_kind','at_spi','known_gaps'):
                self.assertIn(key,p)
        self.assertFalse(load_profile('debian-kde-wayland')['at_spi']['available'])

    def test_recursive_merge_is_sorted_and_does_not_mutate(self):
        base={'a':{'b':1,'c':[2]}}
        self.assertEqual(merge(base,{'a':{'d':3}}),{'a':{'b':1,'c':[2],'d':3}})
        self.assertEqual(base,{'a':{'b':1,'c':[2]}})
        with tempfile.TemporaryDirectory() as td:
            p=Path(td);(p/'test.json').write_text(json.dumps({'desktop':'test','catalog_tolerance':.01,'a':{'b':1}}))
            (p/'test.d').mkdir()
            (p/'test.d/z.json').write_text('{"a":{"b":3}}')
            (p/'test.d/a.json').write_text('{"a":{"b":2,"c":4}}')
            self.assertEqual(load_profile('test',p)['a'],{'b':3,'c':4})


class Mapping(unittest.TestCase):
    def test_scale_hidpi_and_negative_monitor_origin(self):
        inv={'width':100,'height':50};control={'rect':[10,5,20,10]}
        self.assertEqual(control_point(inv,control,{'x':30,'y':40,'width':100,'height':50,'scale':1}),(50,50))
        self.assertEqual(control_point(inv,control,{'x':-1900,'y':10,'width':200,'height':100,'scale':2,'screen_x':-1920}),(120,60))
        self.assertEqual(control_point(inv,control,{'x':0,'y':0,'width':100,'height':50},(0,0)),(10,5))

    def test_nonuniform_resize_refused(self):
        with self.assertRaisesRegex(RecipeBlocked,'aspect'):
            control_point({'width':100,'height':50},{'rect':[0,0,10,10]}, {'x':0,'y':0,'width':200,'height':50})

    def test_selectors_and_ambiguity(self):
        with tempfile.TemporaryDirectory() as td:
            p=Path(td); inv={'width':100,'height':50,'controls':[{'name':'editorUpload','text':'&Upload','class':'QPushButton','rect':[1,2,3,4]}]}
            (p/'01-editor.controls.json').write_text(json.dumps(inv))
            cat=Catalog(p,{})
            for selector in ('editorUpload','text:Upload','text-glob:Up*','glob:editor*','class:QPushButton'):
                self.assertEqual(cat.resolve(selector,'editor')[2]['name'],'editorUpload')
            inv['controls'].append({'name':'editorOther','text':'Upload','class':'QPushButton','rect':[10,2,3,4]})
            (p/'02-editor.controls.json').write_text(json.dumps(inv))
            with self.assertRaisesRegex(RecipeBlocked,'distinct matches'):
                Catalog(p,{}).resolve('text:Upload','editor')
            with self.assertRaises(RecipeBlocked): cat.reference('../escaped')

    def test_key_translation_and_socket_byte_limit(self):
        self.assertEqual(qcodes('Ctrl+Z'),['ctrl','z'])
        self.assertEqual(qcodes('Ctrl+Shift+Z'),['ctrl','shift','z'])
        self.assertEqual(qcodes('Escape'),['esc'])
        self.assertEqual(qcodes('Ctrl++'),['ctrl','shift','equal'])
        self.assertEqual(qcodes('Backtab'),['shift','tab'])
        self.assertEqual(qcodes('!'),['shift','1'])
        with self.assertRaises(RecipeBlocked): qcodes('☃')
        self.assertEqual(checked_socket('/'+'x'*106),'/'+'x'*106)
        with self.assertRaises(RecipeBlocked): checked_socket('/'+'é'*54)


class FakeTarget:
    root='/tmp/sd-test';socket=root+'/runtime/putgd/session.sock';physical=None
    def screenshot(self,path,head=0):
        Image.new('RGB',(20,10),'white').save(path)
    def cli(self,*args,timeout=60):
        return {'returncode':0,'stdout':'{"ok":true,"result":{"ready":true}}','stderr':''}
    def execute(self,script,timeout=60):
        return {'returncode':0,'stdout':'','stderr':''}
    def key(self,codes):pass


class Running(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
        self.env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
        self.ctx=DriverContext(FakeTarget(),self.env,load_profile(self.env['desktop']),self.path,self.path)
    def tearDown(self):self.temp.cleanup()

    def test_registry_selection(self):
        self.assertTrue(discover('core_demo'))
        self.assertTrue(all('editor.undo' in r.interactions for r in discover('editor.undo')))
        with self.assertRaises(RecipeBlocked):discover('missing-lane')
        validate_recipes(discover('core_demo'))
        with self.assertRaises(ValueError):validate_recipes([Recipe(('not.real',),lambda c:None)])

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_runner_pass_fail_block_and_exact_schema(self):
        def good(ctx):ctx.expect(ctx.call('capabilities')['ready'],'resident ready')
        def bad(ctx):ctx.expect(False,'actual failure')
        def blocked(ctx):ctx.blocked('no portal',123)
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),good),Recipe(('editor.redo',),bad),Recipe(('editor.tool-text',),blocked)],self.env,self.path,'test',self.path/'coverage.json')
        self.assertEqual([r.functional for r in rows],['passed','failed','blocked'])
        self.assertEqual(rows[-1].issue,123)
        receipt=json.loads((self.path/'receipt.json').read_text())
        cov=json.loads((self.path/'coverage.json').read_text())
        self.assertEqual(receipt['schemaVersion'],2)
        self.assertEqual(cov['entries'],[r.to_json() for r in rows])
        self.assertEqual(receipt['interactions'],cov['entries'])
        self.assertEqual(receipt['environment'],self.env)
        imported=surface_map.import_receipt(surface_map.load_model(),self.path/'receipt.json',self.env['desktop'],'deb')
        self.assertEqual([r['functional'] for r in imported],['passed','failed','blocked'])
        self.assertLessEqual(set(cov['entries'][0]),{f.name for f in dataclasses.fields(ReceiptRow)})
        for row in rows:
            self.assertEqual([f['label'] for f in row.frames],['before','after'])

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_visual_within_and_outside_tolerance(self):
        Image.new('RGB',(20,10),'white').save(self.path/'good.png')
        Image.new('RGB',(20,10),'black').save(self.path/'bad.png')
        self.assertEqual(compare(self.path/'good.png',self.path/'good.png',.025),0)
        self.assertEqual(compare(self.path/'good.png',self.path/'bad.png',.025),1)
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':20,'height':10,'scale':1}
        def good(ctx):ctx.frame('match',window='editor',reference='good')
        def bad(ctx):ctx.frame('mismatch',window='editor',reference='bad')
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),good),Recipe(('editor.redo',),bad)],self.env,self.path,'test',self.path/'coverage.json')
        self.assertEqual(rows[0].visual,'reviewed-ok');self.assertFalse(rows[0].review)
        self.assertEqual(rows[1].visual,'frames');self.assertTrue(rows[1].review)
        self.assertIn(str(self.path/'bad.png'),rows[1].review_reason)
        self.assertIn(str(self.path/'frames'),rows[1].review_reason)

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_desktop_drawn_never_compares(self):
        self.ctx.begin(Recipe(('tray.tooltip',),lambda c:None,desktop_drawn=True))
        self.ctx.frame('tray',reference='does-not-exist')
        self.assertIsNone(self.ctx.frames[0].distance)
        self.assertFalse(self.ctx.reviews)

    def test_wait_timeout_and_unavailable_accessibility(self):
        with self.assertRaisesRegex(RecipeFailed,'specific state'):
            self.ctx.wait(lambda:False,'specific state',timeout=0)
        self.ctx.profile['at_spi']['available']=False
        with self.assertRaises(RecipeBlocked):self.ctx.accessible({})

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_missing_reference_requests_review(self):
        self.ctx.frame('missing',reference='unknown')
        self.assertTrue(self.ctx.reviews)

    def test_receipt_atomic_overwrite(self):
        rows=[ReceiptRow('editor.undo',self.env['desktop'],'deb','blocked')]
        write_receipts(self.path,self.env,rows,'test',self.path/'coverage.json')
        write_receipts(self.path,self.env,[],'test',self.path/'coverage.json')
        self.assertEqual(json.loads((self.path/'receipt.json').read_text())['interactions'],[])
        self.assertEqual([p.name for p in self.path.glob('.receipt*')],[])
        self.assertEqual(len(json.loads((self.path/'coverage.json').read_text())['entries']),1)

    def test_physical_module_import_has_no_side_effects(self):
        importlib.import_module('tools.lab.physical')

    def test_coverage_preserves_other_desktops(self):
        row=ReceiptRow('editor.undo','mint-cinnamon-x11','deb','passed',host='boat',placement='boat')
        other=dataclasses.replace(row,desktop='ubuntu-gnome-x11')
        write_receipts(self.path/'one',self.env,[row],'campaign',self.path/'coverage.json')
        write_receipts(self.path/'two',self.env,[other],'campaign',self.path/'coverage.json')
        row.functional='failed'
        write_receipts(self.path/'one',self.env,[row],'campaign',self.path/'coverage.json')
        entries=json.loads((self.path/'coverage.json').read_text())['entries']
        self.assertEqual(len(entries),2)
        self.assertEqual({r['desktop']:r['functional'] for r in entries},
                         {'mint-cinnamon-x11':'failed','ubuntu-gnome-x11':'passed'})

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_click_uses_catalog_and_releases_button(self):
        inv={'width':20,'height':10,'controls':[{'name':'upload','rect':[2,2,4,4]}]}
        (self.path/'test-editor.controls.json').write_text(json.dumps(inv))
        self.ctx.catalog=Catalog(self.path,{})
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':20,'height':10,'scale':1}
        events=[]
        self.ctx.target.pointer=lambda *args:events.append(args)
        self.ctx.click('upload',window='editor')
        self.assertEqual(events[0][:5],(4,4,20,10,True))
        self.assertEqual(events[-1][:5],(4,4,20,10,False))

    def test_resized_layout_uses_live_control_instead_of_guessing(self):
        inv={'width':20,'height':10,'controls':[{'name':'upload','text':'&Upload','rect':[2,2,4,4]}]}
        (self.path/'test-editor.controls.json').write_text(json.dumps(inv))
        self.ctx.catalog=Catalog(self.path,{})
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':40,'height':20,'scale':1}
        self.ctx._dimensions=lambda head=0:(100,100)
        probes=[]
        self.ctx._probe=lambda name,query:probes.append((name,query)) or [6,8,10,12]
        events=[];self.ctx.target.pointer=lambda *args:events.append(args)
        self.ctx.click('upload',window='editor')
        self.assertEqual(events[0][:2],(11,14))
        self.assertEqual(probes[0][0],'control')
        self.assertEqual(probes[0][1]['label'],'Upload')
        self.ctx.profile['at_spi']['available']=False
        with self.assertRaisesRegex(RecipeBlocked,'live AT-SPI'):
            self.ctx.click('upload',window='editor')

    def test_unicode_input_refuses_before_partial_typing(self):
        sent=[];self.ctx.target.key=lambda value:sent.append(value)
        with self.assertRaises(RecipeBlocked):self.ctx.type_text('hello ☃')
        self.assertEqual(sent,[])
        self.ctx.type_text('Ab!')
        self.assertEqual(sent,[['shift','a'],['b'],['shift','1']])

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_main_checks_identity_and_configures_local_output(self):
        target=FakeTarget();target.info={'id':'run-id','profile':'cinnamon-x11'};target.run=self.path
        target.start=lambda:None;target.close=lambda:None
        target.identity=lambda:{'session_type':'x11','desktop':'X-Cinnamon'}
        calls=[]
        def cli(*args,timeout=60):
            calls.append(args)
            result={'ui.snapshot':{'build':{'gitCommit':'abcdef','dirty':False}},
                    'settings.get':{'metadata':{'observedRevision':'revision'}}}.get(args[0],{})
            return {'returncode':0,'stdout':json.dumps({'ok':True,'result':result}),'stderr':''}
        target.cli=cli;target.execute=lambda script,timeout=60:{'returncode':0,'stdout':'123','stderr':''}
        recipe=Recipe(('editor.undo',),lambda c:None)
        args=['--desktop',self.env['desktop'],'--host','boat','--placement','boat','--boat-id','bx_test',
              '--guest','cinnamon-x11','--run',str(self.path),'--package','deb','--destination','local',
              '--recipes','core_demo','--catalog',str(self.path),'--build','abcdef','--out',str(self.path/'run'),
              '--coverage',str(self.path/'main-coverage.json')]
        with patch('tools.lab.surface_driver.run.Target',return_value=target),patch('tools.lab.surface_driver.run.discover',return_value=[recipe]):
            self.assertEqual(main(args),0)
        mutation=next(json.loads(a[1]) for a in calls if a[0]=='settings.patch')
        self.assertEqual(mutation['patch']['actions']['edit']['destination'],'local')
        self.assertTrue(mutation['patch']['archive']['root'].startswith(target.root))
        self.assertTrue(mutation['patch']['archive']['keepUploads'])
        self.assertEqual(json.loads((self.path/'run/receipt.json').read_text())['environment']['observed_build']['gitCommit'],'abcdef')

    def test_cleanup_attempts_quit_when_monitor_transport_fails(self):
        target=Target.__new__(Target)
        target._rfb=None;target.monitor_pid=123;target.started=True
        def unavailable(*args,**kwargs):raise RecipeBlocked('lost monitor transport')
        target.execute=unavailable
        calls=[]
        target.cli=lambda *args,**kwargs:calls.append(args) or {'returncode':0}
        errors=target.close()
        self.assertEqual(calls,[('ui.quit',)])
        self.assertFalse(target.started)
        self.assertEqual(errors,['notification monitor cleanup unavailable'])

    def test_resident_operation_identity_is_retained(self):
        self.ctx.target.cli=lambda *args,**kwargs:{'returncode':0,'stderr':'','stdout':
            json.dumps({'ok':True,'result':{'operationId':'test-operation-123'}})}
        self.ctx.call('editor.open',{'source':'file','path':'/tmp/synthetic.png'})
        self.assertEqual(self.ctx.notes,['operationId=test-operation-123'])

    def test_notification_correlation(self):
        raw='''method call time=100.1 sender=:1.2 -> destination=org.freedesktop.Notifications serial=45 path=/org/freedesktop/Notifications; interface=org.freedesktop.Notifications; member=Notify
   string "put.gd"
   uint32 0
   string "putgd"
   string "Saved locally"
   string "Synthetic file"
   array [
      string "default"
      string "Show"
   ]
method return time=100.2 sender=:1.3 -> destination=:1.2 serial=9 reply_serial=45
   uint32 72
'''
        row=parse_notifications(raw,100)[0]
        self.assertEqual((row['summary'],row['body'],row['id']),('Saved locally','Synthetic file',72))
        self.assertEqual(parse_notifications(raw,101),[])


if __name__=='__main__':unittest.main()
