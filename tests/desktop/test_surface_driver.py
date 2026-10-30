#!/usr/bin/env python3
"""Fast host contracts: no app, desktop, credentials, network or VM required."""
import dataclasses
import importlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, Mock, call, patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed, ReceiptRow
from tools.lab.surface_driver.catalog import Catalog, compare, control_point
from tools.lab.surface_driver.context import DriverContext, parse_notifications
from tools.lab.surface_driver.profiles import load_profile, merge
from tools.lab.surface_driver.receipts import write_receipts
from tools.lab.surface_driver.run import discover, guard_target, main, native_endpoint, run_recipes, validate_recipes, verify_physical_endpoints
from tools.lab import desktop_lock, lab
from tools.lab.surface_driver.target import Target, checked_socket, qcodes
from tools.lab.surface_map import surface_map
sys.path.insert(0,str(ROOT/'tests/desktop'))
from surface_live_fixture import write_catalog
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
        self.assertEqual(control_point(inv,control,{'x':-1900,'y':10,'width':100,'height':50,'scale':2,'screen_x':-1920}),(80,40))
        self.assertEqual(control_point(inv,control,{'x':0,'y':0,'width':100,'height':50},(0,0)),(10,5))

    def test_nonuniform_resize_refused(self):
        with self.assertRaisesRegex(RecipeBlocked,'size differs'):
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
    def capture_image(self,head=0):
        return Image.new('RGB',(20,10),'white')
    def cli(self,*args,timeout=60):
        return {'returncode':0,'stdout':'{"ok":true,"result":{"ready":true}}','stderr':''}
    def execute(self,script,timeout=60):
        return {'returncode':0,'stdout':'','stderr':''}
    def key(self,codes):pass


class Running(unittest.TestCase):
    def setUp(self):
        self.ocr_patch = patch('tools.lab.surface_driver.redact.ocr_text', return_value='')
        self.ocr_patch.start(); self.addCleanup(self.ocr_patch.stop)
        self.which_patch = patch('tools.lab.surface_driver.redact.shutil.which', return_value='unit-ocr')
        self.which_patch.start(); self.addCleanup(self.which_patch.stop)
        # The real self-test needs tesseract; LiveEvidence tests cover it directly.
        self.selftest_patch = patch('tools.lab.surface_driver.redact.ocr_self_test', return_value=True)
        self.selftest_patch.start(); self.addCleanup(self.selftest_patch.stop)
        self.temp=tempfile.TemporaryDirectory();self.path=Path(self.temp.name)
        self.env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
        profile=load_profile(self.env['desktop'])
        self.ctx=DriverContext(FakeTarget(),self.env,profile,self.path,self.path)
    def tearDown(self):self.temp.cleanup()

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_cleanup_has_a_fresh_budget_after_action_deadline_and_keeps_failure(self):
        cleaned=[]
        def expired(ctx):
            ctx._deadline=time.monotonic()-1
            signal.raise_signal(signal.SIGALRM)
        def cleanup(ctx):
            self.assertGreater(signal.getitimer(signal.ITIMER_REAL)[0],0)
            self.assertTrue(ctx.call('capabilities')['ready'])
            cleaned.append(True)
        expired.cleanup=cleanup
        def following(ctx):ctx.expect(cleaned==[True],'cleanup completed before following recipe')
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),expired),Recipe(('editor.redo',),following)],self.env,self.path,'cleanup',self.path/'coverage.json')
        self.assertEqual([r.functional for r in rows],['failed','passed'])
        self.assertIn('recipe deadline exceeded',rows[0].note)

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_failed_cleanup_blocks_following_actions_and_before_frame(self):
        calls=[]
        def action(ctx):pass
        def cleanup(ctx):raise RecipeFailed('editor still open')
        action.cleanup=cleanup
        def following(ctx):calls.append('unsafe action')
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),action),Recipe(('editor.redo',),following)],self.env,self.path,'cleanup',self.path/'coverage.json')
        self.assertEqual([r.functional for r in rows],['failed','blocked'])
        self.assertEqual(calls,[])
        self.assertNotIn('before',[f['label'] for f in rows[1].frames])

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
        catalog=self.path/'catalog';catalog.mkdir()
        self.ctx.catalog=Catalog(catalog,{})
        Image.new('RGB',(20,10),'white').save(catalog/'good.png')
        Image.new('RGB',(20,10),'black').save(catalog/'bad.png')
        self.assertEqual(compare(catalog/'good.png',catalog/'good.png',.025),0)
        self.assertEqual(compare(catalog/'good.png',catalog/'bad.png',.025),1)
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':20,'height':10,'scale':1}
        def good(ctx):ctx.frame('match',window='editor',reference='good')
        def bad(ctx):ctx.frame('mismatch',window='editor',reference='bad')
        self.ctx.out=self.path/'run';self.ctx.evidence.out=self.ctx.out
        rows=run_recipes(self.ctx,[Recipe(('editor.undo',),good),Recipe(('editor.redo',),bad)],self.env,self.ctx.out,'test',self.path/'coverage.json')
        self.assertEqual(rows[0].visual,'reviewed-ok');self.assertFalse(rows[0].review)
        self.assertEqual(rows[1].visual,'frames');self.assertTrue(rows[1].review)
        self.assertIn(str(catalog/'bad.png'),rows[1].review_reason)
        self.assertIn(str(self.ctx.out/'frames'),rows[1].review_reason)

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
        self.ctx.geometry=lambda role:{'x':0,'y':0,'width':20,'height':10,'scale':1,'head':0}
        self.ctx._probe=lambda name,query:({'rect':[2,2,4,4]} if name=='control' else {'ok':True})
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
        self.ctx._probe=lambda name,query:probes.append((name,query)) or ({'rect':[6,8,10,12]} if name=='control' else {'ok':True})
        events=[];self.ctx.target.pointer=lambda *args:events.append(args)
        self.ctx.click('upload',window='editor')
        self.assertEqual(events[0][:2],(11,14))
        self.assertEqual(probes[0][0],'control')
        self.assertEqual(probes[0][1]['candidates'][0]['text'],'&Upload')
        self.ctx.profile['at_spi']['available']=False
        with self.assertRaisesRegex(RecipeBlocked,'live AT-SPI'):
            self.ctx.click('upload',window='editor')

    def test_unicode_input_refuses_before_partial_typing(self):
        sent=[];self.ctx.target.key=lambda value:sent.append(value)
        with self.assertRaises(RecipeBlocked):self.ctx.type_text('hello ☃')
        self.assertEqual(sent,[])
        self.ctx._current_window='editor'
        self.ctx._guard=lambda **kwargs:None
        self.ctx.type_text('Ab!')
        self.assertEqual(sent,[['shift','a'],['b'],['shift','1']])

    def test_main_refuses_live_destinations_until_secret_safe_evidence(self):
        from tools.lab.surface_driver import run as runner, receipts, context, redact
        write_catalog(self.path)
        self.assertEqual(runner.secret_safe_evidence_blockers('boat', self.path, 'abcdef'), [])
        failures = ((redact.shutil, 'which'), (receipts, 'Redactor'), (receipts, 'save_frame'),
                    (receipts, 'scan_run'), (redact, 'ocr_text'), (runner, 'finish_evidence'), (runner, 'run_recipes'), (context, 'Evidence'))
        for destination in ('s3','b2','proton'):
            for module, name in failures:
                with self.subTest(destination=destination, missing=name):
                    out=self.path/('live-'+destination+'-'+name)
                    args=['--desktop',self.env['desktop'],'--host','boat','--placement','boat','--boat-id','bx_test',
                          '--guest','cinnamon-x11','--run',str(self.path),'--package','deb','--destination',destination,
                          '--recipes','core_demo','--catalog',str(self.path),'--build','abcdef','--out',str(out)]
                    replacement = (lambda *a:None) if name == 'which' else None
                    with patch.object(module,name,replacement), patch.object(runner,'Target') as target:
                        with self.assertRaises(SystemExit) as raised:
                            runner.main(args)
                        target.assert_not_called()
                    self.assertIn('blocked by secret-safe evidence',str(raised.exception))
                    self.assertFalse(out.exists())

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_main_allows_live_destinations_with_the_complete_evidence_pipeline(self):
        from tools.lab.surface_driver import run as runner
        for destination in ('s3','b2','proton'):
            with self.subTest(destination=destination):
                target=FakeTarget();target.info={'id':'run-id','profile':'cinnamon-x11'};target.run=self.path
                target.start=lambda:None;target.close=lambda:None
                target.identity=lambda:{'session_type':'x11','desktop':'X-Cinnamon'}
                def cli(method,*args,**kw):
                    result=({'build':{'gitCommit':'abcdef','dirty':False}} if method=='ui.snapshot'
                            else {'metadata':{'observedRevision':'revision'}} if method=='settings.get' else {})
                    return {'returncode':0,'stdout':json.dumps({'ok':True,'result':result}),'stderr':''}
                target.cli=cli;target.execute=lambda script,timeout=60:{'returncode':0,'stdout':'123','stderr':''}
                touched=[]
                recipe=Recipe(('editor.undo',),lambda ctx:touched.append(ctx.destination),destinations=(destination,))
                out=self.path/('admitted-'+destination)
                args=['--desktop',self.env['desktop'],'--host','boat','--placement','boat','--boat-id','bx_test',
                      '--guest','cinnamon-x11','--run',str(self.path),'--package','deb','--destination',destination,
                      '--recipes','core_demo','--catalog',str(write_catalog(self.path/'catalog')),'--build','abcdef','--out',str(out),
                      '--coverage',str(out/'coverage.json')]
                with patch.object(runner,'Target',return_value=target),patch.object(runner,'guard_target'),patch.object(runner,'discover',return_value=[recipe]):
                    self.assertEqual(runner.main(args),0)
                self.assertEqual(touched,[destination])
                self.assertTrue(json.loads((out/'receipt.json').read_text())['secret_scan']['passed'])

    @unittest.skipUnless(Image,'requires python3-pil')
    def test_live_gate_refuses_a_present_but_nonworking_ocr_executable(self):
        from tools.lab.surface_driver import run as runner, redact
        self.ocr_patch.stop();self.selftest_patch.stop()
        with patch.object(redact.subprocess,'run',return_value=subprocess.CompletedProcess([],1,b'',b'')):
            self.assertIn('ocr-self-test-failed',runner.secret_safe_evidence_blockers('boat',write_catalog(self.path),'abcdef'))
        # Exits 0 but reads nothing: still refused (blind OCR).
        with patch.object(redact.subprocess,'run',return_value=subprocess.CompletedProcess([],0,b'',b'')):
            self.assertIn('ocr-self-test-failed',runner.secret_safe_evidence_blockers('boat',write_catalog(self.path),'abcdef'))

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
        with patch('tools.lab.surface_driver.run.Target',return_value=target),patch('tools.lab.surface_driver.run.discover',return_value=[recipe]),patch('tools.lab.surface_driver.run.guard_target'),patch('tools.lab.surface_driver.run.resolve_boat_cli',return_value='/trusted/boat'):
            self.assertEqual(main(args),0)
        mutation=next(json.loads(a[1]) for a in calls if a[0]=='settings.patch')
        self.assertEqual(mutation['patch']['actions']['edit']['destination'],'local')
        self.assertTrue(mutation['patch']['archive']['root'].startswith(target.root))
        self.assertTrue(mutation['patch']['archive']['keepUploads'])
        self.assertEqual(json.loads((self.path/'run/receipt.json').read_text())['environment']['observed_build']['gitCommit'],'abcdef')
        target.close=lambda:['cleanup incomplete']
        args[args.index('--out')+1]=str(self.path/'run-error')
        with patch('tools.lab.surface_driver.run.Target',return_value=target),patch('tools.lab.surface_driver.run.discover',return_value=[recipe]),patch('tools.lab.surface_driver.run.guard_target'),patch('tools.lab.surface_driver.run.resolve_boat_cli',return_value='/trusted/boat'):
            self.assertEqual(main(args),2)
        self.assertEqual(json.loads((self.path/'run-error/receipt.json').read_text())['environment']['cleanup_errors'],['cleanup incomplete'])

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

    def test_target_removes_only_its_private_session_root_after_exit(self):
        target=Target.__new__(Target)
        target._rfb=None;target.monitor_pid=None;target.started=False;target._created_root=True
        target.root=str(self.path/'owned space');Path(target.root).mkdir(mode=0o700)
        sibling=self.path/'preserve';sibling.write_text('unrelated')
        def execute(script,**kwargs):
            reply=subprocess.run(script,shell=True,text=True,capture_output=True,timeout=10)
            return {'returncode':reply.returncode,'stdout':reply.stdout,'stderr':reply.stderr}
        target.execute=execute
        self.assertEqual(target.close(),[])
        self.assertFalse(Path(target.root).exists());self.assertEqual(sibling.read_text(),'unrelated')
        Path(target.root).mkdir(mode=0o700);target._created_root=True
        (Path(target.root)/'fx-pending').mkdir(mode=0o700)
        self.assertEqual(target.close(),['private session root cleanup refused'])
        self.assertTrue(Path(target.root).exists())
        (Path(target.root)/'fx-pending').rmdir()
        mountinfo='1 0 0:1 / '+target.root.replace(' ',chr(92)+'040')+' rw - tmpfs tmpfs rw\n'
        target.execute=lambda script,**kwargs:execute(script.replace("Path('/proc/self/mountinfo').read_text()",repr(mountinfo)),**kwargs)
        self.assertEqual(target.close(),['private session root cleanup refused'])
        self.assertTrue(Path(target.root).exists())

    def test_resident_operation_identity_is_retained(self):
        self.ctx.target.cli=lambda *args,**kwargs:{'returncode':0,'stderr':'','stdout':
            json.dumps({'ok':True,'result':{'operationId':'test-operation-123'}})}
        self.ctx.call('editor.open',{'source':'file','path':'/tmp/synthetic.png'})
        self.assertEqual(self.ctx.notes,['operationId=test-operation-123'])
        self.ctx.evidence.redactor.remember('e', opaque=True)
        write_receipts(self.path,self.env,[],'test',self.path/'coverage.json',evidence=self.ctx.evidence)
        receipt=json.loads((self.path/'receipt.json').read_text())
        self.assertEqual(receipt['environment']['operation_ids'],[{'operationId':'test-operation-123'}])

    def test_cached_heartbeat_does_not_allow_input_past_expiry(self):
        from types import SimpleNamespace
        target=Target.__new__(Target)
        target._lock=('f44','owned-session');target._last_heartbeat=100;target._lock_valid_until=105
        calls=[]
        target.physical=SimpleNamespace(ssh=lambda *args:calls.append(args) or json.dumps({'ok':True,'state':'expired','expiresInSeconds':0}))
        target.run=self.path
        with patch('tools.lab.surface_driver.target.time.clock_gettime',return_value=110):
            with self.assertRaisesRegex(RecipeBlocked,'no longer held'):target.heartbeat()
        self.assertEqual(len(calls),1)

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


class InputFoundations(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)
        self.ctx=DriverContext(FakeTarget(),dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',package='deb',destination='local'),
                               load_profile('mint-cinnamon-x11'),self.path,self.path)
        self.g={'x':0,'y':0,'width':100,'height':80,'scale':1,'head':0,'pid':7,'window_id':'10'}
        self.ctx.geometry=lambda role:dict(self.g)
        self.ctx._dimensions=lambda head=0:(200,200)
        self.ctx._current_window='editor'

    def test_obstruction_stops_every_input_and_names_window(self):
        self.ctx._probe=lambda name,query:{'ok':False,'obscuring_window':'First launch Settings','active':'20'}
        sent=[]
        self.ctx.target.pointer=lambda *args,**kwargs:sent.append(args)
        self.ctx.target.key=lambda *args:sent.append(args)
        for action in (lambda:self.ctx.click_at(10,10), lambda:self.ctx.drag((10,10),(20,20)),
                       lambda:self.ctx.keys('T'),lambda:self.ctx.type_text('x'),
                       lambda:self.ctx.scroll(10,10,1),lambda:self.ctx.hover(10,10)):
            with self.assertRaisesRegex(RecipeFailed,'editor.*First launch Settings'):
                action()
        self.assertEqual(sent,[])

    def test_focus_is_rechecked_between_chords(self):
        probes=[];sent=[]
        def probe(name,query):
            probes.append(query)
            return {'ok':len(probes)==1,'obscuring_window':'Other client'}
        self.ctx._probe=probe;self.ctx.target.key=sent.append
        with self.assertRaisesRegex(RecipeFailed,'Other client'):
            self.ctx.keys('T','R')
        self.assertEqual(sent,[['t']])
        self.assertEqual(len(probes),2)

    def test_held_keys_release_on_exception(self):
        self.ctx._probe=lambda *a:{'ok':True}
        events=[];self.ctx.target.key_state=lambda *args:events.append(args)
        with self.assertRaisesRegex(RecipeFailed,'during drag'):
            with self.ctx.held_keys('Ctrl','Shift'):
                raise RecipeFailed('during drag')
        self.assertEqual(events,[(['ctrl','shift'],True),(['shift','ctrl'],False)])
        self.assertEqual(self.ctx._held_codes,[])

    def test_overlapping_held_key_chord_refuses_before_release(self):
        self.ctx._probe=lambda *a:{'ok':True}
        sent=[];self.ctx.target.key_state=lambda *args:sent.append(args)
        with self.ctx.held_keys('AltGr'):
            with self.assertRaisesRegex(RecipeBlocked,'held by held_keys'):
                self.ctx.keys('AltGr+E')
        self.assertEqual(sent,[(['alt_r'],True),(['alt_r'],False)])

    def test_double_click_is_one_resolved_gesture(self):
        self.ctx._probe=lambda name,query:({'rect':[10,10,20,20]} if name=='control' else {'ok':True})
        sent=[];self.ctx.target.pointer=lambda *args:sent.append(args)
        self.ctx.double_click('text:Save','editor')
        self.assertEqual([args[4] for args in sent],[True,False,True,False])
        self.assertEqual(self.ctx._click_count,1)

    def test_canvas_focus_selects_owning_window(self):
        queries=[]
        self.ctx._probe=lambda name,query:queries.append(query) or {'ok':True}
        self.ctx.focus('editor.canvas')
        self.assertEqual(self.ctx._current_window,'editor')
        self.assertEqual(queries[0]['geometry']['window_id'],'10')

    def test_drag_failure_releases_held_button(self):
        guards=[];events=[]
        def guard(**kwargs):
            guards.append(kwargs)
            if len(guards)>1:raise RecipeFailed('focus changed during drag')
        self.ctx._guard=guard;self.ctx.target.pointer=lambda *args,**kw:events.append(args)
        with self.assertRaisesRegex(RecipeFailed,'focus changed'):
            self.ctx.drag((10,10),(30,30))
        self.assertEqual(events[0][4],True)
        self.assertEqual(events[-1][4],False)

    def test_desktop_label_click_uses_pointer_and_hit_test(self):
        self.ctx.accessible=lambda query:[{'name':'Notice','role':'label','pid':8,'enabled':False,'rect':[10,10,80,20]}]
        probes=[];events=[]
        self.ctx._probe=lambda name,query:probes.append(query) or {'ok':True}
        self.ctx.target.pointer=lambda *args:events.append(args)
        self.ctx.click_accessible({'application':'cinnamon','name':'Notice'})
        self.assertEqual(probes[0]['point'],(50,20))
        self.assertEqual(events[0][:2],(50,20))
        self.assertEqual([e[4] for e in events],[True,False])

    def test_desktop_hover_does_not_press_a_button(self):
        self.ctx.accessible=lambda query:[{'name':'Notice','role':'label','pid':8,'rect':[10,10,80,20]}]
        self.ctx._probe=lambda *args:{'ok':True}
        sent=[];self.ctx.target.pointer=lambda *args,**kwargs:sent.append((args,kwargs))
        self.ctx.hover_accessible({'application':'cinnamon','name':'Notice'})
        self.assertEqual(sent,[((50,20,200,200),{'head':0})])

    def test_read_only_tray_inventory_is_private_and_hashes_pixmaps(self):
        from tools.lab.surface_driver import probe
        from types import SimpleNamespace
        props={'Id':'putgd','Title':'put.gd','Status':'Active','IconName':'putgd',
               'IconPixmap':[(1,1,[255,0,0,0])],'ToolTip':('',[],'put.gd','Idle'),'Menu':'/Menu'}
        class Interface:
            def Get(self,*a):return [':1.7/Item',':1.8/Item']
            def GetConnectionUnixProcessID(self,service):return 7 if service==':1.7' else 8
            def GetAll(self,*a):return props
        fake=SimpleNamespace(SessionBus=lambda:SimpleNamespace(get_object=lambda *a:None),Interface=lambda *a:Interface())
        with patch.dict(sys.modules,dbus=fake),patch.object(probe,'owned',side_effect=lambda pid:pid==7):
            rows=probe.tray({})
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['pid'],7)
        self.assertEqual(rows[0]['tooltip'],{'title':'put.gd','text':'Idle'})
        self.assertEqual(rows[0]['icon_pixmaps'][0]['bytes'],4)
        self.assertEqual(len(rows[0]['icon_pixmaps'][0]['sha256']),64)

    def test_uncertain_key_down_attempts_release_before_propagating(self):
        self.ctx._probe=lambda *a:{'ok':True}
        sent=[]
        def state(codes,down):
            sent.append((codes,down))
            if down:raise RecipeFailed('reply lost after dispatch')
        self.ctx.target.key_state=state
        with self.assertRaisesRegex(RecipeFailed,'reply lost'):
            with self.ctx.held_keys('Ctrl'):
                self.fail('acquisition did not complete')
        self.assertEqual(sent,[(['ctrl'],True),(['ctrl'],False)])
        self.assertEqual(self.ctx._held_codes,[])

    def test_unconfirmed_key_release_blocks_subsequent_input(self):
        def state(codes,down):
            if not down:raise RecipeBlocked('transport lost')
        self.ctx.target.key_state=state
        self.ctx._probe=lambda *a:{'ok':True}
        with self.assertRaisesRegex(RecipeBlocked,'transport lost'):
            with self.ctx.held_keys('Ctrl'):
                pass
        with self.assertRaisesRegex(RecipeBlocked,'release was not confirmed'):
            self.ctx.keys('T')

    def test_physical_held_keys_and_vnc_altgr_block_before_dispatch(self):
        target=Target.__new__(Target);target.physical=object();target.run=self.path
        target.heartbeat=lambda **kwargs:None
        target.events=lambda *a:self.fail('physical held input must not dispatch')
        target.vnc=lambda:self.fail('unsupported VNC chord must not dispatch')
        with self.assertRaisesRegex(RecipeBlocked,'scoped cleanup'):
            target.key_state(['ctrl'],True)
        with self.assertRaisesRegex(RecipeBlocked,'AltGr requires'):
            target.key(['alt_r','e'])

    def test_x11_error_handler_is_restored_when_display_open_fails(self):
        from tools.lab.surface_driver import probe
        library=MagicMock();library.XSetErrorHandler.side_effect=[321,None]
        library.XOpenDisplay.return_value=None
        with patch.object(probe.ctypes.util,'find_library',return_value='libX11.so'),patch.object(probe.ctypes,'CDLL',return_value=library):
            with self.assertRaisesRegex(RuntimeError,'cannot open X11 display'):
                probe.x11_hit((20,20))
        self.assertEqual(library.XSetErrorHandler.call_count,2)
        self.assertEqual(library.XSetErrorHandler.call_args.args,(321,))

    def test_broad_role_profiles_have_putgd_application_scope(self):
        for desktop in ('mint-cinnamon-x11','ubuntu-gnome-wayland','kubuntu-kde-wayland'):
            profile=load_profile(desktop)
            for role in ('fileDialog','putgdFileDialog','region','recsel','confirm','confirmAction'):
                self.assertEqual(profile['windows'][role]['accessible']['application'],'putgd*')

    def test_live_menus_without_catalog_context_and_row(self):
        events=[];self.ctx.target.pointer=lambda *args:events.append(args)
        requests=[]
        def probe(name,query):
            requests.append((name,query))
            if name=='control':
                return {'rect':[15,20,40,12],'pid':7,'popup':{'rect':[10,10,80,60],'pid':7}}
            return {'ok':True}
        self.ctx._probe=probe
        for role in ('historyContextMenu','historyRowMenu'):
            self.ctx.click('text:Copy path',role)
            self.assertIsNone(self.ctx._popup)
        self.assertEqual(events[0][:2],(35,26))
        self.assertTrue(all(q['popup'] for name,q in requests if name=='control'))
        self.assertTrue(all(q['geometry']['pid']==7 for name,q in requests if name=='foreground'))

    def test_matching_catalog_cannot_replace_missing_live_control_witness(self):
        self.ctx.profile['at_spi']['available']=False
        self.ctx.target.pointer=lambda *a,**kw:self.fail('static coordinates must not dispatch input')
        with self.assertRaisesRegex(RecipeBlocked,'live AT-SPI control geometry'):
            self.ctx.click('text:Save','editor')

    def test_live_controls_fail_closed_even_when_catalog_exists(self):
        inv={'width':100,'height':80,'controls':[{'name':'save','text':'Save','rect':[1,2,4,4]}]}
        (self.path/'editor.controls.json').write_text(json.dumps(inv));self.ctx.catalog=Catalog(self.path,{})
        def missing(*args):raise RecipeBlocked('hidden or disabled')
        self.ctx._probe=missing
        with self.assertRaisesRegex(RecipeBlocked,'hidden or disabled'):
            self.ctx.click('save','editor')

    def test_secondary_head_offset_applied_once(self):
        self.ctx.profile['screens']=[{'x':-1920,'y':0,'width':1920,'height':1080,'head':1,'scale':2}]
        self.ctx.profile['session']='wayland'
        self.g.update(x=-1900,width=100,coordinate_space='logical')
        def probe(name,query):
            return {'rect':[-1880,20,20,10]} if name=='control' else {'ok':True}
        self.ctx._probe=probe
        events=[];self.ctx.target.pointer=lambda *args:events.append(args)
        self.ctx.click('text:Save','editor')
        self.assertEqual(events[0][:2],(100,50))
        self.assertEqual(events[0][-1],1)

    def test_wayland_missing_compositor_witness_blocks(self):
        from tools.lab.surface_driver import probe
        with patch.dict('os.environ',{'XDG_SESSION_TYPE':'wayland'}):
            with self.assertRaisesRegex(RuntimeError,'compositor focus/stacking witness unavailable'):
                probe.foreground({'geometry':self.g})

    def test_wayland_shell_pick_requires_the_expected_compositor_node(self):
        from tools.lab.surface_driver import probe
        from types import SimpleNamespace
        node=SimpleNamespace(name='Synthetic notice',get_process_id=lambda:50,getRoleName=lambda:'label')
        component=SimpleNamespace(getAccessibleAtPoint=lambda *a:node)
        window=SimpleNamespace(queryComponent=lambda:component)
        class App(list):
            def get_process_id(self):return 50
        fake=SimpleNamespace(Registry=SimpleNamespace(getDesktop=lambda _:[App([window])]),DESKTOP_COORDS=0)
        query={'desktop_node':{'application':'gnome-shell','name':'Synthetic notice','role':'label','pid':50},
               'point':[40,30],'shell_application':'gnome-shell*'}
        with patch.dict(sys.modules,pyatspi=fake):
            self.assertTrue(probe.shell_hit(query)['ok'])
            query['desktop_node']['application']='other app'
            with self.assertRaisesRegex(RuntimeError,'profile compositor'):
                probe.shell_hit(query)

    def test_x11_active_and_stacking_reject_same_process_obscurer(self):
        from tools.lab.surface_driver import probe
        settings=dict(self.g,window_id='20',name='Settings')
        def run(*args):
            return '_NET_ACTIVE_WINDOW(WINDOW): window id # 0xa' if args[-1]=='_NET_ACTIVE_WINDOW' else '_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0xa, 0x14'
        with patch.object(probe,'run',side_effect=run),patch.object(probe,'x11_windows',return_value=[dict(self.g,name='Editor'),settings]):
            result=probe.x11_foreground({'geometry':self.g})
        self.assertFalse(result['ok']);self.assertEqual(result['obscuring_window'],'Settings')

    def test_x11_unmanaged_popup_uses_server_hit_test(self):
        from tools.lab.surface_driver import probe
        def run(*args):
            return '_NET_ACTIVE_WINDOW(WINDOW): window id # 0xa' if args[-1]=='_NET_ACTIVE_WINDOW' else '_NET_CLIENT_LIST_STACKING(WINDOW): window id # 0xa'
        with patch.object(probe,'run',side_effect=run),patch.object(probe,'x11_windows',return_value=[dict(self.g,name='Editor')]),patch.object(probe,'x11_hit',return_value={'pid':8,'path':['99'],'name':'Other popup'}):
            result=probe.x11_foreground({'geometry':self.g,'point':[5,5]})
        self.assertFalse(result['ok']);self.assertEqual(result['obscuring_window'],'Other popup')

    def test_x11_inventory_retains_unmanaged_obscurer_without_pid(self):
        from tools.lab.surface_driver import probe
        from types import SimpleNamespace
        def run(*args):
            if args[0]=='xprop' and args[1]=='-root':return 'window id # 0x14'
            if args[1]=='getwindowpid':raise subprocess.CalledProcessError(1,args)
            if args[0]=='xwininfo':return '  Absolute upper-left X:  0\n  Absolute upper-left Y:  0\n  Width: 100\n  Height: 80\n  Override Redirect State: no\n'
            if args[1]=='getwindowname':return 'Other client without PID'
            return ''
        with patch.object(probe.subprocess,'run',return_value=SimpleNamespace(stdout='20')),patch.object(probe,'run',side_effect=run):
            rows=probe.x11_windows({'owned':False})
        self.assertEqual(rows[0]['name'],'Other client without PID')
        self.assertIsNone(rows[0]['pid'])

    def test_private_seed_precedes_launch_and_first_launch_is_explicit(self):
        target=Target.__new__(Target);target.root='/tmp/sd-seed';target.app_executable='putgd';target.started=False
        target.preflight=lambda:None;target.preserve_clipboard=lambda:None
        calls=[]
        target.execute=lambda script,**kw:calls.append(script) or {'returncode':0}
        target.cli=lambda *a:{'returncode':0,'stdout':'{"ok":true,"result":{}}'}
        target.start()
        seed=next(i for i,s in enumerate(calls) if 'SD_SEED' in s)
        launch=next(i for i,s in enumerate(calls) if 'nohup' in s)
        self.assertLess(seed,launch)
        self.assertNotIn('development-s3',calls[seed]);self.assertIn("'profiles': []",calls[seed])
        self.assertIn("'updates': {'check': False}",calls[seed])
        calls.clear();target.start(first_launch=True)
        self.assertFalse(any('SD_SEED' in s for s in calls))

    def test_accessibility_shape_text_filters_and_protected_text(self):
        from tools.lab.surface_driver import probe
        from types import SimpleNamespace
        class Node:
            def __init__(self,name,role,text='',children=()):
                self.name,self.role,self.text,self.children=name,role,text,children
                self.description='description'
            def getRoleName(self):return self.role
            def get_process_id(self):return 7
            def getAttributes(self):return ['id:field']
            def getState(self):return SimpleNamespace(contains=lambda value:value in ('visible','showing','enabled'))
            def queryComponent(self):return SimpleNamespace(getExtents=lambda coords:(1,2,30,40))
            def queryText(self):
                if self.role=='password text':raise AssertionError('protected text must not be read')
                return SimpleNamespace(characterCount=len(self.text),getText=lambda a,b:self.text[a:b])
            def queryAction(self):raise NotImplementedError
            def __iter__(self):return iter(self.children)
        field=Node('File name','text','/tmp/synthetic.png')
        secret=Node('Password','password text','must stay private')
        app=Node('putgd-ui','application',children=[Node('Open image','file chooser',children=[field,secret]),
                 Node('Edit image','filler',children=[Node('Image being edited','filler')])])
        fake=SimpleNamespace(Registry=SimpleNamespace(getDesktop=lambda _:Node('desktop','desktop frame',children=[app])),DESKTOP_COORDS=0,
                             **{'STATE_'+k.upper():k for k in ('visible','showing','enabled')})
        with patch.dict(sys.modules,pyatspi=fake),patch.object(probe,'owned',return_value=True):
            rows=probe.accessible({'text':'/tmp/*.png','role':'text','owned':True})
            self.assertEqual(len(rows),1)
            row=rows[0]
            for key in ('name','role','text','states','rect','bounds','pid','application','window','accelerator','ancestors'):
                self.assertIn(key,row)
            self.assertEqual(row['window'],'Open image');self.assertEqual(row['rect'],[1,2,30,40])
            self.assertEqual(probe.accessible({'name':'Password'})[0]['text'],'')
            self.assertEqual(probe.accessible({'objectName':'missing'}),[])
            self.assertEqual(probe.accessible({'name':'Image being edited'})[0]['window'],'Edit image')
            with self.assertRaisesRegex(ValueError,'unknown accessibility'):
                probe.accessible({'typo':True})

    def test_notification_action_is_real_pointer_and_waits_for_correlated_signal(self):
        self.ctx.profile['notifications']['actions_available']=True
        notices=[{'id':72,'summary':'Synthetic notice','actions':['default','Show'],'action_events':[]}]
        self.ctx.notifications=lambda:notices
        node={'name':'Synthetic notice','pid':50,'role':'label','rect':[10,10,80,20],'application':'cinnamon'}
        queries=[];self.ctx.accessible=lambda query:queries.append(query) or [node]
        picks=[];self.ctx._probe=lambda name,query:picks.append(query) or {'ok':True}
        events=[]
        def pointer(*args,**kwargs):
            events.append(args)
            if len(args)>4 and args[4] is False:notices[0]['action_events'].append({'action':'default'})
        self.ctx.target.pointer=pointer
        self.ctx.notification_action(72)
        # One owned title lookup, hover first, then press/release on the title.
        self.assertEqual(len(queries),1);self.assertIn('application',queries[0])
        self.assertEqual(events[0],(50,20,200,200));self.assertEqual([e[4] for e in events[1:]],[True,False])
        self.assertTrue(all(q['desktop_node'] is node and q['point']==(50,20) for q in picks))
        notices[0]['closed']={'reason':1}
        with self.assertRaisesRegex(RecipeBlocked,'closed'):
            self.ctx.notification_action(72)

    def test_notification_button_uses_common_ancestor_not_title_only_panel(self):
        self.ctx.profile['notifications']['actions_available']=True
        notice={'id':72,'summary':'Synthetic notice','actions':['retry','Retry'],'action_events':[]}
        title={'name':'Synthetic notice','pid':50,'role':'label','rect':[20,10,80,20],
               'ancestors':[{'role':'notification','rect':[0,0,200,150]},
                            {'role':'panel','rect':[20,10,80,20]}]}
        button={'name':'Retry','pid':50,'role':'push button','rect':[20,100,80,20]}
        outside={'name':'Retry','pid':50,'role':'push button','rect':[20,170,80,20]}
        self.ctx.notifications=lambda:[notice]
        self.ctx.accessible=lambda query:[title] if query['name']=='Synthetic notice' else [button,outside]
        self.ctx._probe=lambda name,query:{'ok':True}
        events=[]
        def pointer(*args,**kwargs):
            events.append(args)
            if len(args)>4 and args[4] is False:notice['action_events'].append({'action':'retry'})
        self.ctx.target.pointer=pointer
        self.ctx.notification_action(72,'retry')
        self.assertEqual(events[0][:2],(60,20))
        self.assertEqual([e[:2] for e in events[1:]],[(60,110),(60,110)])

    def test_declared_notification_input_gap_blocks_before_observation(self):
        self.ctx.profile['notifications']=dict(self.ctx.profile['notifications'],actions_available=False,
                                               reason='Declared desktop gap (#670)')
        self.ctx.notifications=lambda: self.fail('gap must withhold all input before observation')
        with self.assertRaisesRegex(RecipeBlocked,'Declared desktop gap') as error:
            self.ctx.notification_action(72)
        self.assertEqual(error.exception.issue,670)

    def test_notification_events_are_correlated_to_server_and_replacement(self):
        raw='''method call time=100.1 sender=:1.2 -> destination=org.freedesktop.Notifications serial=45 path=/org/freedesktop/Notifications; interface=org.freedesktop.Notifications; member=Notify
   string "put.gd"
   uint32 0
   string "putgd"
   string "Synthetic notice"
   string "Synthetic body"
   array [
      string "default"
      string "Show"
      string "retry"
      string "Retry"
   ]
   array [
      dict entry (
         string "resident"
         variant             boolean true
      )
   ]
   int32 5000
method return time=100.2 sender=:1.3 -> destination=:1.2 serial=9 reply_serial=45
   uint32 72
signal time=100.3 sender=:1.99 -> destination=(null destination) serial=20 interface=org.freedesktop.Notifications; member=ActionInvoked
   uint32 72
   string "wrong-server"
signal time=100.4 sender=:1.3 -> destination=(null destination) serial=21 interface=org.freedesktop.Notifications; member=ActionInvoked
   uint32 72
   string "retry"
signal time=100.5 sender=:1.3 -> destination=(null destination) serial=22 interface=org.freedesktop.Notifications; member=NotificationClosed
   uint32 72
   uint32 2
'''
        row=parse_notifications(raw,100)[0]
        self.assertEqual(row['sender'],':1.2');self.assertEqual(row['server'],':1.3')
        self.assertTrue(row['resident']);self.assertEqual(row['timeout'],5000)
        self.assertEqual(row['action_events'],[{'action':'retry','at':100.4}])
        self.assertEqual(row['closed'],{'reason':2,'at':100.5})


class TargetSafety(unittest.TestCase):
    def setUp(self):
        endpoint_check=patch('tools.lab.surface_driver.run.verify_physical_endpoints')
        self.endpoint_check=endpoint_check.start()
        self.addCleanup(endpoint_check.stop)
        guest_check=patch('tools.lab.surface_driver.run.verify_guest_endpoint')
        self.guest_check=guest_check.start()
        self.addCleanup(guest_check.stop)

    def arguments(self, host='f44', placement='native', session='owned-session'):
        return SimpleNamespace(host=host, placement=placement, lock_session=session, boat_id='bx_test', boat_cli='/trusted/boat')

    def target(self, address='192.168.68.70', user='patrick', port=22):
        return SimpleNamespace(physical=SimpleNamespace(config={'host':address,'user':user,'ssh_port':port}),
                               run=Path('/tmp/sd-guard'), require_lock=Mock())

    def test_every_host_placement_profile_and_lock_combination(self):
        # The declaration never exempts a builder, including boat/boat and boat/guest.
        from tools.lab.surface_driver.api import HOSTS, PLACEMENTS
        for host in HOSTS:
            for placement in PLACEMENTS:
                for profile in (*desktop_lock.HOSTS, 'unknown', 'guest', 'boat'):
                    for session in (None, 'owned-session'):
                        with self.subTest(host=host, placement=placement, profile=profile, lock=session):
                            address=desktop_lock.HOSTS[profile][0] if profile in desktop_lock.HOSTS else ('127.0.0.1' if profile=='boat' else 'unknown.example')
                            target = self.target(address)
                            target.physical.ssh=Mock(return_value='boot-id\n')
                            if profile=='guest':
                                target.physical=None
                            args=self.arguments(host,placement,session)
                            builder=profile in desktop_lock.HOSTS
                            allowed = (builder and host==profile and placement=='native' and session) or (profile=='guest' and placement!='native') or (profile=='boat' and host=='boat' and placement=='boat')
                            command=b'qemu-system-x86_64\0-qmp\0unix:/tmp/sd-guard/qmp.sock,server=on,wait=off\0'
                            reply=SimpleNamespace(stdout=json.dumps({'sandbox':{'id':'bx_test','hydrated':True}}))
                            proof=SimpleNamespace(stdout=json.dumps({'exitCode':0,'stdout':'boot-id\n'}))
                            with patch.object(lab,'verify_owned_guest',return_value={'pid':123}),patch.object(Path,'read_bytes',return_value=command),patch.object(Path,'read_text',return_value='boot-id\n'),patch('tools.lab.surface_driver.run.subprocess.run',side_effect=lambda command,**kwargs:reply if command[1]=='info' else proof):
                                if allowed:
                                    guard_target(args,target)
                                else:
                                    with self.assertRaises(RecipeBlocked):guard_target(args,target)
                            if builder and allowed:
                                target.require_lock.assert_called_once_with(profile,'owned-session')
                            else:
                                target.require_lock.assert_not_called()

    def test_all_builders_use_actual_endpoint_not_profile_name(self):
        for builder,(address,_) in desktop_lock.HOSTS.items():
            target=self.target(address)
            target.physical.config['name']='boat-sandbox'
            guard_target(self.arguments(builder),target)
            target.require_lock.assert_called_once_with(builder,'owned-session')
            for user,port in (('other',22),('patrick',2222)):
                with self.assertRaises(RecipeBlocked):
                    guard_target(self.arguments(builder),self.target(address,user,port))

    def test_unknown_or_non_qemu_run_is_not_a_guest_exemption(self):
        target=self.target();target.physical=None
        for problem in (lab.LabError('physical state'),OSError('missing QEMU'),RuntimeError('symlink')):
            with patch.object(lab,'verify_owned_guest',side_effect=problem):
                with self.assertRaisesRegex(RecipeBlocked,'unverified input target'):
                    guard_target(self.arguments('boat','guest'),target)
        with patch.object(lab,'verify_owned_guest',return_value={'pid':123}),patch.object(Path,'read_bytes',return_value=b'qemu-system-x86_64\0-qmp\0unix:/another/qmp.sock,server=on,wait=off\0'):
            with self.assertRaisesRegex(RecipeBlocked,'unverified input target'):
                guard_target(self.arguments('boat','boat'),target)

    def test_boat_profile_needs_authenticated_sandbox_and_local_machine(self):
        target=self.target('127.0.0.1', 'user')
        target.physical.ssh=Mock(return_value='boot-id\n')
        args=self.arguments('boat','boat')
        for identity,hydrated,remote_boot,allowed in (
                ('bx_other',True,'boot-id',False),('bx_test',False,'boot-id',False),
                ('bx_test',True,'different',False),('bx_test',True,'boot-id',True)):
            target.physical.ssh.return_value=remote_boot
            reply=SimpleNamespace(stdout=json.dumps({'sandbox':{'id':identity,'hydrated':hydrated}}))
            proof=SimpleNamespace(stdout=json.dumps({'exitCode':0,'stdout':'boot-id\n'}))
            with patch('tools.lab.surface_driver.run.subprocess.run',side_effect=[reply,proof]),patch.object(Path,'read_text',return_value='boot-id\n'):
                if allowed:guard_target(args,target)
                else:
                    with self.assertRaises(RecipeBlocked):guard_target(args,target)
        # A supplied BOAT_ID and matching local SSH boot identity do not prove
        # that the runner is the authenticated sandbox.
        reply=SimpleNamespace(stdout=json.dumps({'sandbox':{'id':'bx_test','hydrated':True}}))
        for witness in ({'exitCode':0,'stdout':'other-boot'}, {'exitCode':1,'stdout':'boot-id'}):
            proof=SimpleNamespace(stdout=json.dumps(witness))
            with patch('tools.lab.surface_driver.run.subprocess.run',side_effect=[reply,proof]),patch.object(Path,'read_text',return_value='boot-id\n'):
                with self.assertRaises(RecipeBlocked):guard_target(args,target)
        with patch('tools.lab.surface_driver.run.subprocess.run',side_effect=FileNotFoundError):
            with self.assertRaises(RecipeBlocked):guard_target(args,target)
        for malformed in ('not-json', '[]', '{}', '{"sandbox":null}'):
            with patch('tools.lab.surface_driver.run.subprocess.run',return_value=SimpleNamespace(stdout=malformed)):
                with self.assertRaises(RecipeBlocked):guard_target(args,target)
        args.boat_id='--api-url=https://untrusted.invalid'
        with patch('tools.lab.surface_driver.run.subprocess.run') as command:
            with self.assertRaises(RecipeBlocked):guard_target(args,target)
            command.assert_not_called()
        args.boat_id='bx_test';target.physical.config['user']='-oProxyCommand=untrusted'
        with patch('tools.lab.surface_driver.run.subprocess.run') as command:
            with self.assertRaises(RecipeBlocked):guard_target(args,target)
            command.assert_not_called()

    def test_bypass_and_missing_native_lock_refuse_before_start_input_or_capture(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td)
            for number,(host,placement,session,address) in enumerate((
                    ('boat','boat',None,'192.168.68.70'),('boat','guest',None,'192.168.68.70'),
                    ('f44','native',None,'192.168.68.70'),('f44','native','owned-session','unknown.example'),
                    ('f44','native','revoked-session','192.168.68.70'))):
                target=self.target(address)
                target.require_lock.side_effect=RecipeBlocked('lock is not held by this session')
                target.info={'id':'fake-target','profile':'cinnamon-x11'}
                target.root='/tmp/sd-fake';target.socket=target.root+'/runtime/putgd/session.sock'
                target.identity=Mock(return_value={'session_type':'x11'})
                target.key=Mock();target.screenshot=Mock();target.close=Mock(return_value=[])
                def unsafe_start():
                    target.key(['t'])
                    raise RecipeFailed('unlocked resident start was reached')
                target.start=Mock(side_effect=unsafe_start)
                args=['--desktop','mint-cinnamon-x11','--host',host,'--placement',placement,
                      '--host-profile',str(root/'pinned.json'),'--boat-id','bx_test','--package','deb',
                      '--destination','local','--recipes','core_demo','--catalog',str(root),'--build','abcdef',
                      '--out',str(root/str(number)),'--coverage',str(root/'coverage.json')]
                if session:args+=['--lock-session',session]
                with patch('tools.lab.surface_driver.run.Target',return_value=target),patch('tools.lab.surface_driver.run.resolve_boat_cli',return_value='/trusted/boat'):
                    self.assertEqual(main(args),2)
                for operation in (target.start,target.identity,target.key,target.screenshot):operation.assert_not_called()
                self.assertIn('blocked',json.loads((root/str(number)/'receipt.json').read_text())['environment'])

    def test_each_real_input_rechecks_lock_even_with_cached_lease(self):
        for method,arguments in (('key',(['t'],)),('pointer',(1,1,100,100)),('events',([],)),('scroll_vnc',(1,1,1))):
            target=Target.__new__(Target)
            target._lock=('f44','owned-session');target._last_heartbeat=100;target._lock_valid_until=1000
            target.run=Path('/tmp/sd-guard');target.info={};target._rfb=Mock()
            target.physical=SimpleNamespace(ssh=Mock(side_effect=[
                json.dumps({'ok':True,'state':'held','expiresInSeconds':30}),
                json.dumps({'ok':False,'state':'held','expiresInSeconds':29})]))
            with patch('tools.lab.surface_driver.target.time.clock_gettime',return_value=101),patch.object(lab,'input_events') as events,patch.object(lab,'key') as key,patch.object(lab,'pointer') as pointer:
                getattr(target,method)(*arguments)
                with self.assertRaises(RecipeBlocked):getattr(target,method)(*arguments)
                self.assertEqual(target.physical.ssh.call_count,2)
                # A wheel gesture consists of press plus deadline-safe release;
                # the rejected second gesture must dispatch neither packet.
                expected = 2 if method == 'scroll_vnc' else 1
                self.assertEqual(sum(m.call_count for m in (events,key,pointer,target._rfb.key,target._rfb.pointer_button,target._rfb.scroll)),expected)
                if method == 'scroll_vnc':
                    self.assertEqual(target._rfb.pointer_button.call_args_list,
                                     [call(1,1,'wheel-up',True),call(1,1,'wheel-up',False)])

    def test_unexpired_held_lock_required_at_start(self):
        for reply in ({'ok':True,'state':'expired','expiresInSeconds':0},
                      {'ok':True,'state':'held','expiresInSeconds':0},
                      {'ok':True,'state':'held'}, {'ok':False,'state':'held','expiresInSeconds':30},
                      {'ok':True,'state':'held','expiresInSeconds':float('nan')},
                      {'ok':True,'state':'held','expiresInSeconds':float('inf')},
                      {'ok':True,'state':'held','expiresInSeconds':'30'}):
            target=Target.__new__(Target)
            target.run=Path('/tmp/sd-guard')
            target.physical=SimpleNamespace(ssh=Mock(return_value=json.dumps(reply)))
            with self.assertRaises(RecipeBlocked):target.require_lock('f44','owned-session')

    def test_expiry_during_heartbeat_round_trip_blocks_input(self):
        private=tempfile.TemporaryDirectory();self.addCleanup(private.cleanup)
        target=Target.__new__(Target);target.run=Path(private.name);target._rfb=Mock()
        target._lock=('f44','owned-session')
        target.physical=SimpleNamespace(ssh=Mock(return_value=json.dumps({'ok':True,'state':'held','expiresInSeconds':2})))
        with patch('tools.lab.surface_driver.target.time.clock_gettime',side_effect=[100,102]):
            with self.assertRaises(RecipeBlocked):target.key(['t'])
        target._rfb.key.assert_not_called()

    def test_lock_is_rechecked_after_endpoint_proof_immediately_before_input(self):
        private=tempfile.TemporaryDirectory();self.addCleanup(private.cleanup)
        target=Target.__new__(Target);target.run=Path(private.name)
        target.physical=SimpleNamespace(config={'host':'192.168.68.70','user':'patrick'})
        target.require_lock=Mock();target._rfb=Mock()
        guard_target(self.arguments(),target)
        order=[]
        self.endpoint_check.side_effect=lambda *args:order.append('endpoints')
        target.heartbeat=lambda **kwargs:order.append('lock')
        target._rfb.key.side_effect=lambda *args:order.append('input')
        target.key(['t'])
        self.assertEqual(order,['endpoints','lock','input'])

    def test_endpoint_rebinding_blocks_the_next_input(self):
        private=tempfile.TemporaryDirectory();self.addCleanup(private.cleanup)
        target=Target.__new__(Target);target.run=Path(private.name);target.info={}
        target.physical=SimpleNamespace(config={'host':'192.168.68.70','user':'patrick'})
        target.heartbeat=Mock();target._rfb=Mock()
        guard_target(self.arguments(),target)
        self.endpoint_check.assert_called_once_with(target)
        self.endpoint_check.side_effect=[None,RecipeBlocked('endpoint changed')]
        target.key(['t'])
        with self.assertRaises(RecipeBlocked):target.key(['t'])
        target._rfb.key.assert_called_once_with('t')

    def test_native_host_input_still_works_under_a_held_lock(self):
        with tempfile.TemporaryDirectory() as td:
            target=Target.__new__(Target);target.run=Path(td)
            (target.run/'qmp.sock').touch()
            held=json.dumps({'ok':True,'state':'held','expiresInSeconds':30})
            target.physical=SimpleNamespace(ssh=Mock(side_effect=[held,held,
                json.dumps({'ok':True,'state':'expired','expiresInSeconds':0})]))
            target.require_lock('f44','owned-session')
            with patch.object(lab,'key') as key:
                target.key(['t'])
                key.assert_called_once_with(target.run,['t'])
                with self.assertRaises(RecipeBlocked):target.key(['t'])
                self.assertEqual(key.call_count,1)
            self.assertEqual(target.physical.ssh.call_count,3)


class EndpointSafety(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.run=Path(self.temp.name)
        self.path=self.run/'qmp.sock'
        self.assertLessEqual(len(str(self.path).encode('utf-8')),107)
        self.listener=socket.socket(socket.AF_UNIX)
        self.listener.bind(str(self.path));self.listener.listen()
        self.addCleanup(self.listener.close)
        self.target=SimpleNamespace(run=self.run,physical=SimpleNamespace(
            config={'host':'192.168.68.70','user':'patrick','known_hosts':'/pins','vnc_port':5970},
            ssh=lambda run,command:'1000' if command=='id -u' else ('{"verified":true}' if 'SD_ENDPOINT' in command else 'boot-id')))
        self.argv=['ssh','-NT','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile=/pins','-o','ExitOnForwardFailure=yes',
                   '-L','127.0.0.1:5970:127.0.0.1:5901',
                   '-L',str(self.path)+':/run/user/1000/putgd-lab-input.sock','patrick@192.168.68.70']
        self.executable='ssh';self.owner=os.getuid();self.rows=True
        original_stat=Path.stat;original_read=Path.read_bytes;original_link=Path.readlink;original_text=Path.read_text
        def proc_stat(path,*args,**kwargs):
            return SimpleNamespace(st_uid=self.owner) if path==Path('/proc/123') else original_stat(path,*args,**kwargs)
        def proc_read(path):
            return '\0'.join(self.argv).encode() if path==Path('/proc/123/cmdline') else original_read(path)
        def proc_link(path):
            return Path('/usr/bin/'+self.executable) if path==Path('/proc/123/exe') else original_link(path)
        def proc_text(path,*args,**kwargs):
            return 'boot-id' if path==Path('/proc/sys/kernel/random/boot_id') else original_text(path,*args,**kwargs)
        def listeners(command,**kwargs):
            self.assertEqual(command[0],'ss')
            address='127.0.0.1:5970' if '-ltnp' in command else str(self.path)
            return SimpleNamespace(stdout='LISTEN 0 128 '+address+' * users:(("ssh",pid=123,fd=4))' if self.rows else '')
        for patcher in (patch.object(Path,'stat',proc_stat),patch.object(Path,'read_bytes',proc_read),
                        patch.object(Path,'readlink',proc_link),patch.object(Path,'read_text',proc_text),
                        patch('tools.lab.surface_driver.run.subprocess.run',side_effect=listeners)):
            patcher.start();self.addCleanup(patcher.stop)

    def test_both_pinned_forwarded_endpoints_are_accepted(self):
        verify_physical_endpoints(self.target)

    def test_mismatched_vnc_qmp_host_pins_and_remote_command_are_rejected(self):
        original=list(self.argv)
        for old,new in (('patrick@192.168.68.70','patrick@192.168.68.69'),
                        ('127.0.0.1:5970:127.0.0.1:5901','127.0.0.1:5970:127.0.0.1:5900'),
                        (str(self.path)+':/run/user/1000/putgd-lab-input.sock',str(self.path)+':/another/qmp.sock'),
                        ('UserKnownHostsFile=/pins','UserKnownHostsFile=/other-pins')):
            self.argv=[new if value==old else value for value in original]
            with self.subTest(replaced=old):
                with self.assertRaisesRegex(RecipeBlocked,'endpoint binding'):verify_physical_endpoints(self.target)
        self.argv=original+['remote-command','patrick@192.168.68.70']
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_shadowed_pins_ports_and_forwarding_options_are_rejected(self):
        original=list(self.argv)
        for extra in (['-o','StrictHostKeyChecking=no'],['-o','UserKnownHostsFile /other'],['-o','ExitOnForwardFailure=no'],
                      ['-p','2222','-p','22'],['-o','Port=2222'],['-o','LocalForward=5970 127.0.0.1:5900'],
                      ['-L','127.0.0.1:5970:127.0.0.1:5900'],
                      ['-L',str(self.path)+':/another/qmp.sock'],
                      ['-R','127.0.0.1:5901:192.168.68.69:5900'],['-F','other-config']):
            self.argv=[original[0],*extra,*original[1:]]
            with self.subTest(options=extra):
                with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_forwarded_service_must_be_native_on_the_authenticated_host(self):
        for reply in ('{"verified":false}', 'null', 'not-json'):
            self.target.physical.ssh=lambda run,command:'1000' if command=='id -u' else reply
            with self.subTest(reply=reply):
                with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_native_service_proof_rejects_ssh_and_wrong_input_socket(self):
        with self.assertRaises(ValueError):native_endpoint('vnc','127.0.0.1:5970')
        self.executable='w0vncserver'
        self.assertTrue(native_endpoint('vnc','127.0.0.1:5970'))
        self.executable='python3.12';self.argv=['python3','/lab/host_input.py','serve','--socket',str(self.path)]
        self.assertTrue(native_endpoint('input',str(self.path)))
        self.argv[-1]=str(self.path)+'-other'
        with self.assertRaises(ValueError):native_endpoint('input',str(self.path))

    def test_unknown_or_unowned_listener_is_rejected(self):
        self.rows=False
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)
        self.rows=True;self.owner=os.getuid()+1
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)
        self.owner=os.getuid();self.executable='socat'
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_regular_file_or_indirect_input_socket_is_rejected(self):
        self.path.unlink();self.path.write_text('not a socket')
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)
        self.path.unlink();self.path.symlink_to(self.run/'another.sock')
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_direct_native_vnc_requires_the_same_authenticated_machine(self):
        self.path.unlink();self.executable='w0vncserver'
        verify_physical_endpoints(self.target)
        self.target.physical.ssh=lambda *args:'another-boot'
        with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_direct_host_input_requires_its_exact_socket(self):
        # The native VNC and Python input listeners have separate PIDs in a
        # real session; select their executable/argv per ss query here.
        socket_argument=str(self.path)
        def listeners(command,**kwargs):
            if '-ltnp' in command:
                self.executable='w0vncserver'
                address='127.0.0.1:5970'
            else:
                self.executable='python3.12';address=str(self.path)
                self.argv=['python3','/lab/host_input.py','serve','--socket',socket_argument]
            return SimpleNamespace(stdout='LISTEN 0 128 '+address+' * users:(("server",pid=123,fd=4))')
        with patch('tools.lab.surface_driver.run.subprocess.run',side_effect=listeners):
            verify_physical_endpoints(self.target)
            socket_argument=str(self.path)+'-other'
            with self.assertRaises(RecipeBlocked):verify_physical_endpoints(self.target)

    def test_unverifiable_endpoints_block_main_before_resident_or_capture(self):
        self.target.info={'id':'fake','profile':'cinnamon-x11'}
        self.target.root='/tmp/sd-fake';self.target.socket='/tmp/sd-fake/runtime/putgd/session.sock'
        self.target.require_lock=Mock();self.target.start=Mock();self.target.identity=Mock()
        self.target.screenshot=Mock();self.target.close=Mock(return_value=[])
        self.argv[-1]='patrick@192.168.68.69'
        args=['--desktop','mint-cinnamon-x11','--host','f44','--placement','native','--lock-session','owned',
              '--host-profile','pinned.json','--package','deb','--destination','local','--recipes','core_demo',
              '--catalog',str(self.run),'--build','abcdef','--out',str(self.run/'result'),
              '--coverage',str(self.run/'coverage.json')]
        with patch('tools.lab.surface_driver.run.Target',return_value=self.target),patch('tools.lab.surface_driver.run.resolve_boat_cli',return_value='/trusted/boat'):
            self.assertEqual(main(args),2)
        self.target.require_lock.assert_called_once_with('f44','owned')
        for operation in (self.target.start,self.target.identity,self.target.screenshot):operation.assert_not_called()


class CoreDemoCleanup(unittest.TestCase):
    def test_finalizing_helper_waits_for_resident_editor_admission_to_clear(self):
        from tools.lab.surface_driver.recipes.core_demo import cancelled
        ctx=Mock()
        ctx.cli.side_effect=[{'stdout':json.dumps(value)} for value in (
            {'ok':True,'result':{}},
            {'ok':True,'result':{'ready':True}},
            {'ok':False,'error':{'code':'editor_finalizing'}},
            {'ok':False,'error':{'code':'editor_closed'}},
            {'ok':False,'error':{'code':'editor_unavailable'}})]
        ctx.call.side_effect=[{'editor':{'open':True}}, {'editor':{'open':True}}, {'editor':{'open':False}}]
        observed=[]
        def wait(predicate,*args):
            for _ in range(4):
                observed.append(predicate())
                if observed[-1]:return
            self.fail('editor never closed')
        ctx.wait.side_effect=wait
        cancelled(ctx,'synthetic-editor')
        self.assertEqual(observed,[True,False,False,False,True])

    def test_recipes_wait_for_async_cancel_before_next_before_frame(self):
        from tools.lab.surface_driver.recipes import core_demo
        class Context:
            active=False;closing=0;tool=None;shapes=[];before=[]
            def call(self, method, params=None):
                if method=='editor.open':
                    if self.active:raise RecipeFailed('editor_active')
                    self.active=True;return {'operationId':'synthetic-editor'}
                if method=='editor.cancel':self.closing=3
                if method=='editor.view':return {'tool':self.tool}
                if method=='editor.inspect':return {'shapes':self.shapes}
                if method=='editor.apply':self.shapes=[{}]
                if method=='capabilities':return {'editor':{'open':self.active}}
                return {}
            def cli(self,*args):
                if args[0]=='editor.cancel':
                    self.call('editor.cancel')
                    return {'stdout':json.dumps({'ok':True,'result':{}})}
                if self.closing:
                    self.closing-=1
                    if not self.closing:self.active=False
                value={'ok':True,'result':{'ready':True}} if self.active else {'ok':False,'error':{'code':'editor_unavailable'}}
                return {'stdout':json.dumps(value)}
            def wait(self,predicate,what,timeout=10):
                for _ in range(5):
                    result=predicate()
                    if result:return result
                raise RecipeFailed(what)
            def fixture(self,*args):return {'path':'/synthetic.png'}
            def view(self,*args):pass
            def frame(self,label,**kwargs):
                if label=='before':self.before.append(self.active)
            def keys(self,key):
                if key=='T':self.tool='text'
                if key=='Ctrl+Z':self.shapes=[]
                if key=='Ctrl+Shift+Z':self.shapes=[{}]
            def use_catalog(self,*args):pass
            def click(self,*args,**kwargs):self.tool='redact'
        ctx=Context()
        for function in (core_demo.text_tool,core_demo.redact_tool,core_demo.undo_redo):
            ctx.frame('before');function(ctx);function.cleanup(ctx)
            self.assertFalse(ctx.active)
        self.assertEqual(ctx.before,[False,False,False])

    def test_cancel_does_not_treat_arbitrary_inspection_failure_as_closed(self):
        from tools.lab.surface_driver.recipes.core_demo import cancelled
        ctx=Mock()
        ctx.cli.side_effect=[{'stdout':json.dumps(v)} for v in (
            {'ok':True,'result':{}},{'ok':False,'error':{'code':'permission_denied'}})]
        ctx.wait.side_effect=lambda predicate,*args:predicate()
        with self.assertRaisesRegex(RecipeFailed,'permission_denied'):cancelled(ctx,'synthetic-editor')

    def test_unknown_active_editor_blocks_cleanup_without_guessing_an_id(self):
        from tools.lab.surface_driver.recipes.core_demo import cleanup
        ctx=Mock();ctx.call.return_value={'editor':{'open':True}}
        with self.assertRaisesRegex(RecipeFailed,'cannot identify'):cleanup(ctx)
        ctx.cli.assert_not_called()

    def test_opening_failure_still_retains_its_editor_for_runner_cleanup(self):
        from tools.lab.surface_driver.recipes.core_demo import opened,cleanup
        ctx=Mock();ctx.fixture.return_value={'path':'/synthetic.png'}
        ctx.call.return_value={'operationId':'owned-editor'}
        ctx.wait.side_effect=RecipeFailed('opening timed out')
        with self.assertRaises(RecipeFailed):opened(ctx)
        ctx.wait.side_effect=lambda predicate,*args:predicate()
        ctx.call.return_value={'editor':{'open':False}}
        ctx.cli.return_value={'stdout':json.dumps({'ok':False,'error':{'code':'editor_unavailable'}})}
        cleanup(ctx)
        self.assertEqual(ctx.cli.call_args_list[0].args[0],'editor.cancel')
        self.assertEqual(json.loads(ctx.cli.call_args_list[0].args[1])['id'],'owned-editor')

    def test_cancel_retry_accepts_known_starting_and_finalizing_states(self):
        from tools.lab.surface_driver.recipes.core_demo import cancelled
        ctx=Mock()
        ctx.cli.side_effect=[{'stdout':json.dumps({'ok':False,'error':{'code':code}})}
                             for code in ('editor_starting','editor_finalizing','editor_unavailable')]
        ctx.call.return_value={'editor':{'open':False}}
        def wait(predicate,*args):
            for _ in range(3):
                if predicate():return
            self.fail('cancel never completed')
        ctx.wait.side_effect=wait
        cancelled(ctx,'owned-editor')
        self.assertEqual(ctx.cli.call_count,3)


if __name__=='__main__':unittest.main()
