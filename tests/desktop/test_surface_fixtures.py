#!/usr/bin/env python3
"""Seconds-long fixture contracts. No product binary, GUI, VM or cloud account."""
import copy
import hashlib
import json
import os
import signal
from pathlib import Path
import subprocess
import tempfile
import sys
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))

from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import FACTORIES, FixtureManager
from tools.lab.surface_driver.fixtures.base import Fixture, Scope
from tools.lab.surface_driver.fixtures import clipboard, destinations, dialogs, events, geometry, history, images, live, notifications, privileges, profile, recordings, uploads
from tools.lab.surface_driver.fixtures.profile import seed_profile, seeded_config
from tools.lab.surface_driver.fixtures.s3_server import Server
from tools.lab.surface_driver.run import run_recipes
from tools.lab.surface_driver.receipts import Evidence


class Context:
    def __init__(self, root):
        self._deadline=time.monotonic()+120
        self._fixtures={};self.host='boat';self.placement='boat';self.destination='local'
        self.profile={'guest':'cinnamon-x11','session':'x11'}
        self.calls=[];self.rows=[];self.operations={}
        self.document=seeded_config(root)
        self.target=SimpleNamespace(root=root,socket=root+'/runtime/putgd/session.sock',app_executable='putgd',started=True,close=lambda:[])
        self.target.execute=self.shell
        self.manager=FixtureManager(self)
        self.target.cli=lambda *args,**kw:self.cli(*args,**kw)
    def fixture(self,name,**options):return self.manager.get(name,**options)
    def shell(self,script,timeout=60):
        env=dict(os.environ)
        env.update({key:self.target.root+'/'+value for key,value in
                    (('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_STATE_HOME','state'))})
        p=subprocess.run(script,shell=True,capture_output=True,text=True,timeout=timeout,env=env)
        return dict(returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
    def wait(self,predicate,what,timeout=10,interval=.01):
        until=time.monotonic()+timeout
        while True:
            result=predicate()
            if result:return result
            if time.monotonic()>until:raise RecipeFailed(what)
            time.sleep(interval)
    def call(self,method,params=None):
        params=params or {};self.calls.append((method,params))
        if method=='settings.get':return {'document':copy.deepcopy(self.document),'metadata':{'path':self.target.root+'/config/putgd/config.json','observedRevision':'revision'}}
        if method=='settings.save':self.document=copy.deepcopy(params['document']);return {}
        if method=='settings.patch':
            def merge(a,b):
                for k,v in b.items():
                    if isinstance(v,dict) and isinstance(a.get(k),dict):merge(a[k],v)
                    else:a[k]=copy.deepcopy(v)
            merge(self.document,params['patch']);return {}
        if method=='history.list':return {'records':self.rows,'total':len(self.rows)}
        if method=='history.clear':self.rows=[];return {}
        if method=='cache.clear':return {}
        if method=='editor.open':
            identity='op-'+str(len(self.operations)+1)
            self.operations[identity]={'source':params['path'],'id':identity,'state':'editing'}
            return {'operationId':identity}
        if method=='editor.inspect':return self.operations[params['id']]
        if method=='editor.submit':
            op=self.operations[params['id']];remote=self.document['actions']['edit']['destination']=='upload'
            control=Path(self.manager.scope.root+'/control.json')
            ctl=json.loads(control.read_text()) if control.exists() else {}
            failed=ctl.get('failWrites') or Path(self.document['archive']['root']).is_file()
            op['state']='failed' if failed else 'uploading' if ctl.get('stallSeconds') else 'completed'
            op['items']=[{'artifact':{'id':'item-'+op['id'],'bytes':300},'uploadStatus':'uploading' if op['state']=='uploading' else 'failed' if failed else 'uploaded' if remote else 'local_only','local':{'path':op['source'],'status':'saved'}}]
            self.rows.append({'id':'row-'+op['id'],'operationId':op['id'],'name':Path(op['source']).name,'local':{'path':op['source'],'status':'saved'},'receipt':{'directUrl':'http://127.0.0.1:1234/synthetic.png' if remote else ''}})
            return {}
        if method=='operation.inspect':return self.operations[params['id']]
        if method=='operation.cancel':
            op=self.operations[params['id']]
            if op['state']=='uploading':op['state']='cancelled'
            return {}
        if method=='editor.cancel':return {}
        if method=='history.detail':return next(r for r in self.rows if r['id']==params['id'])
        if method=='update.status':return {'build':{'supported':False},'channel':None}
        if method=='ui.snapshot':return {'build':{'version':'26.1029.1-dev'}}
        return {}
    def cli(self,method,params='{}',timeout=60):
        return {'returncode':0,'stdout':json.dumps({'ok':True,'result':self.call(method,json.loads(params))}),'stderr':''}


class FixtureCase(unittest.TestCase):
    """Private roots and a Context; the round-3 and tray suites reuse it without
    re-running Fixtures' tests (they were collected three times)."""
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='sd-')
        self.root=Path(self.temp.name);self.root.chmod(0o700)
        for name in ('config','data','state','runtime','files'):(self.root/name).mkdir(mode=0o700)
        self.ctx=Context(str(self.root))
        self.restart=patch.object(Scope,'restart');self.restart.start()
    def tearDown(self):
        errors=self.ctx.manager.close();self.restart.stop();self.temp.cleanup()
        self.assertEqual(errors,[])


class Fixtures(FixtureCase):
    def test_external_config_controller_uses_atomic_revision_guard(self):
        from tools.lab.surface_driver.fixtures.events import config_change
        self.ctx.fixture('app-profile')
        path=self.root/'config/putgd/config.json'
        path.parent.mkdir(mode=0o700,exist_ok=True)
        path.write_text(json.dumps(self.ctx.document));path.chmod(0o600)
        original=self.ctx.call
        def call(method,params=None):
            result=original(method,params)
            if method=='settings.get':result['metadata']['observedRevision']=hashlib.sha256(path.read_bytes()).hexdigest()
            return result
        self.ctx.call=call
        config_change(self.ctx.manager.scope,{'preferences':{'notifications':False}})
        self.assertFalse(json.loads(path.read_text())['preferences']['notifications'])
        self.assertFalse(Path(str(path)+'.lock').exists())
        self.assertFalse(list(path.parent.glob('.fixture-config-*')))

    def test_cancellation_releases_stall_before_cancel_and_waits_past_client_bounds(self):
        scope=self.ctx.manager.scope;scope.initialize()
        scope.values['loopback-s3']=Fixture()
        scope.write(scope.root+'/control.json',b'{"stallSeconds":120}')
        order=[]
        def cli(method,params):
            self.assertEqual(json.loads(Path(scope.root+'/control.json').read_text()),{})
            order.append(method)
            return {'returncode':0,'stdout':'{}'}
        def wait(predicate,what,timeout):
            order.append('wait')
            self.assertGreater(timeout,30+25)
            self.assertLessEqual(timeout,90)
            return {'state':'cancelled'}
        with patch.object(self.ctx,'cli',cli),patch.object(self.ctx,'wait',wait):
            uploads.track(scope,'operation.cancel','owned-op')
            self.assertEqual(scope.close(),[])
        self.assertEqual(order,['operation.cancel','wait'])

    def test_recipe_journals_are_isolated_and_paging_uses_the_new_root(self):
        baseline=self.root/'data/putgd';baseline.mkdir();(baseline/'history.sqlite').write_bytes(b'preserve baseline')
        self.ctx.fixture('app-profile')
        first=self.ctx.manager.scope.root
        self.assertEqual(Scope.restart.call_args.args[0]['XDG_DATA_HOME'],first+'/data')
        self.ctx.manager.close()
        self.ctx.fixture('app-profile')
        second=self.ctx.manager.scope.root
        self.assertNotEqual(first,second)
        scope=self.ctx.manager.scope;commands=[]
        def restart(environment=None,*,before=None):
            if before:before()
        original=scope.python
        def python(code,**kw):
            if 'subprocess.run' in code:
                commands.append(code);return '{"result":{"historyRows":51,"independentOperations":51}}'
            return original(code,**kw)
        with patch.object(scope,'restart',restart),patch.object(scope,'python',python):
            history.grow_history(scope,51)
        self.assertIn(second+'/data/putgd/history.sqlite',commands[0])
        self.assertNotIn(str(baseline/'history.sqlite'),commands[0])
        self.assertEqual((baseline/'history.sqlite').read_bytes(),b'preserve baseline')

    def test_privilege_staging_is_overlay_only_and_removed_after_os_cleanup(self):
        scope=self.ctx.manager.scope;commands=[];order=[]
        def shell(command,**kwargs):
            commands.append(command)
            return {'returncode':0,'stdout':'','stderr':''}
        with patch.object(self.ctx,'shell',shell):
            privileges.ensure(scope)
            privileges.ensure(scope)
            scope.defer('nft',lambda:order.append('nft'),priority=250)
            scope.defer('unmount',lambda:order.append('unmount'))
            scope.close()
        hooks=[c for c in commands if 'APT::Update::Pre-Invoke' in c]
        self.assertEqual(len(hooks),2)
        self.assertIn('NOPASSWD: /usr/bin/mount, /usr/bin/umount, /usr/sbin/nft',hooks[0])
        self.assertIn('Dir::Etc::sourcelist=/dev/null',hooks[0])
        self.assertIn('APT::Get::List-Cleanup=0',hooks[0])
        self.assertIn('rm -- /etc/sudoers.d/putgd-surface-',hooks[1])
        self.assertEqual(order,['nft','unmount'])

    def test_privilege_removal_retries_and_is_proven_by_sudo_listing(self):
        # #762: a held apt lists lock skips the removal hook; retry, bounded,
        # and accept removal only once sudo -l no longer grants the commands.
        scope=self.ctx.manager.scope;hooks=[];listed=[1,1,1,1,0];notes=[]
        self.ctx.note=notes.append
        def shell(command,**kwargs):
            if 'APT::Update::Pre-Invoke' in command:hooks.append(command)
            if 'sudo -n -l' in command:return {'returncode':0,'stdout':str(listed.pop(0))+'\n','stderr':''}
            return {'returncode':0,'stdout':'','stderr':''}
        with patch.object(self.ctx,'shell',shell),patch.object(privileges.time,'sleep') as sleep:
            privileges.ensure(scope)
            self.assertEqual(notes,['removing a fixture sudo rule an earlier run left in this overlay'])
            self.assertIn('for f in /etc/sudoers.d/putgd-surface-*',hooks[0])
            self.assertEqual(scope.close(),[])
        self.assertEqual(len(hooks),4)  # Staging, then three removal attempts.
        self.assertEqual([c.args for c in sleep.call_args_list],[(1,),(3,)])
        scope=self.ctx.manager.scope=Scope(self.ctx);listed[:]=[0,1]+[1]*3
        with patch.object(self.ctx,'shell',shell),patch.object(privileges.time,'sleep'):
            privileges.ensure(scope)
            self.assertEqual(scope.close(),['overlay sudo rule: RecipeBlocked'])

    def test_leftover_rules_removed_only_when_named_and_worded_as_staged(self):
        with tempfile.TemporaryDirectory() as directory:
            names={token:Path(directory)/('putgd-surface-'+token) for token in ('a'*32,'b'*32,'c'*32,'xyz')}
            names['a'*32].write_text(privileges.rule('a'*32))
            names['b'*32].write_text(privileges.rule('b'*32)+'lab ALL=(ALL) ALL\n')
            names['c'*32].write_text(privileges.rule('c'*32))
            names['xyz'].write_text(privileges.rule('xyz'))
            script=privileges.leftovers('/etc/sudoers.d/putgd-surface-'+'c'*32).replace('/etc/sudoers.d',directory)
            subprocess.run(['sh','-c','set -eu; '+script],check=True,timeout=10)
            self.assertEqual(sorted(p.name[-3:] for p in Path(directory).iterdir()),['bbb','ccc','xyz'])

    def test_setup_failure_cleanup_defers_termination_until_finalizers_finish(self):
        # #762: a SIGTERM (run.py's handler raises) or the recipe alarm during
        # a failed setup's cleanup no longer aborts the running finalizer.
        stops,finished,reviews=[],[],[]
        self.ctx.review=reviews.append
        def terminate(signum,_frame):
            stops.append(signum)
            raise RecipeFailed('runner interrupted by '+signal.Signals(signum).name)
        def failing(scope):
            def slow():
                for sig in (signal.SIGTERM,signal.SIGALRM):signal.raise_signal(sig)
                time.sleep(.01)
                finished.append('slow')
            scope.defer('slow',slow)
            scope.defer('first',lambda:finished.append('first'))
            raise RecipeBlocked('setup failed')
        previous={sig:signal.signal(sig,terminate) for sig in (signal.SIGTERM,signal.SIGALRM)}
        try:
            with patch.dict(FACTORIES,{'failing':failing}),self.assertRaisesRegex(RecipeBlocked,'setup failed'):
                self.ctx.fixture('failing')
        finally:
            for sig,handler in previous.items():signal.signal(sig,handler)
        self.assertEqual(finished,['first','slow'])
        self.assertEqual(stops,[signal.SIGTERM,signal.SIGALRM])
        self.assertEqual(self.ctx.manager.errors,[])
        self.assertTrue(any('SIGTERM fell due during fixture setup-failure cleanup' in r for r in reviews))
        self.assertTrue(any('Recipe deadline fell due during fixture setup-failure cleanup' in r for r in reviews))
        self.assertFalse(set(signal.pthread_sigmask(signal.SIG_BLOCK,()))&{signal.SIGTERM,signal.SIGALRM})

    def test_clipboard_missing_tool_blocks_clearly_without_launch(self):
        scope=self.ctx.manager.scope;scope.initialize()
        reply={'returncode':1,'stdout':'','stderr':''}
        with patch.object(self.ctx,'shell',return_value=reply),patch.object(scope,'launch') as launch:
            with self.assertRaisesRegex(RecipeBlocked,'clipboard requires xclip'):
                clipboard.owner(scope,scope.root+'/text','text/plain',name='clip')
        launch.assert_not_called()

    def test_clipboard_stages_missing_tool_in_owned_directory(self):
        scope=self.ctx.manager.scope;scope.initialize();commands=[]
        self.ctx.target.bin_dir='/owned-appimage/bin'
        restored=[]
        def shell(command,**kwargs):
            commands.append(command)
            return {'returncode':1 if command.startswith('command -v') else 0,'stdout':'','stderr':''}
        with patch.object(self.ctx,'shell',shell):
            path=clipboard.tool(scope,'xclip')
            self.assertEqual(self.ctx.target.bin_dir,path.rsplit('/',1)[0]+':/owned-appimage/bin')
            scope.defer('baseline relaunch',lambda:restored.append(self.ctx.target.bin_dir),priority=-500)
            self.assertEqual(scope.close(),[])
        self.assertTrue(path.startswith(scope.root+'/'))
        self.assertEqual(restored,['/owned-appimage/bin'])
        self.assertTrue(any('apt-get download xclip' in c and 'dpkg-deb --extract' in c for c in commands))

    def test_clipboard_failed_owner_does_not_return_silent_success(self):
        scope=self.ctx.manager.scope;scope.initialize()
        with patch.object(clipboard,'tool',return_value='/missing/xclip'),patch.object(self.ctx,'wait',side_effect=RecipeFailed('owner absent')):
            with self.assertRaisesRegex(RecipeBlocked,'owner failed to start'):
                clipboard.owner(scope,scope.root+'/text','text/plain',name='clip')

    def test_s3_live_blocks_before_starting_any_dependencies(self):
        self.ctx.destination='s3'
        with self.assertRaisesRegex(RecipeBlocked,'sensitive_transport'):
            self.ctx.fixture('live')
        self.assertFalse(list(self.root.glob('fx-*')))
        self.assertEqual(self.ctx.calls,[])

    def test_slow_upload_uses_requested_destination(self):
        with patch.object(events,'submit',return_value=Fixture()),patch.object(events,'control'),patch.dict(FACTORIES,{'loopback-s3':lambda s:Fixture(port=1234)}):
            self.assertEqual(self.ctx.fixture('events.slow-upload',destination='local').destination,'local')

    def test_live_canvas_points_are_resolved_after_editor_opens(self):
        boxes=iter([dict(x=100,y=200,width=400,height=200,scale=2,screen_x=0,screen_y=0)]*2)
        ctx=SimpleNamespace(geometry=lambda role:next(boxes))
        value=geometry.LiveFixture(ctx,configured=True)
        self.assertIn('editor_mark_start',value)
        self.assertEqual(value['editor_mark_start'],[480,540])
        self.assertEqual(value.editor_mark_end,[720,660])

    def test_history_header_point_avoids_live_navigation(self):
        box=dict(x=100,y=50,width=1320,height=880,pid=42)
        self.ctx.view=lambda *a:None;self.ctx.focus=lambda *a:None
        observations=[]
        def windows(query):
            observations.append('windows')
            return [] if len(observations)==1 else [box]
        self.ctx.windows=windows
        self.ctx.geometry=lambda *a:box
        nodes=[{'name':'Application menu','role':'push button','rect':[112,62,36,36]},
                                     {'name':'Main navigation','role':'page tab list','rect':[600,62,300,36]},
                                     {'name':'Close window','role':'push button','rect':[1370,62,36,36]}]
        def accessible(query):
            observations.append('accessible')
            return [] if observations.count('accessible')==1 else nodes
        self.ctx.accessible=accessible
        point=self.ctx.fixture('history_window').header_drag_point
        self.assertEqual(observations[:2],['windows','windows'])
        self.assertEqual(observations.count('accessible'),2)
        self.assertEqual(point[1],80)
        self.assertTrue(150<point[0]<598 or 902<point[0]<1368)

    def test_cleanup_error_does_not_retain_fake_input_or_safe_owned_directory(self):
        scope=self.ctx.manager.scope;scope.initialize()
        scope.write(scope.root+'/fake-input.json',b'synthetic')
        def broken():raise RecipeFailed('synthetic cleanup failure')
        scope.defer('broken',broken)
        self.assertEqual(scope.close(),['broken: RecipeFailed'])
        self.assertFalse(Path(scope.root).exists())

    def test_fake_input_is_erased_even_if_collection_and_directory_cleanup_fail(self):
        scope=self.ctx.manager.scope;scope.initialize()
        path=scope.root+'/fake-input.json'
        def create(code):
            scope.write(path,b'synthetic input')
            return {'collection':'/owned','reference':'owned'}
        with patch.object(scope,'json',create):self.ctx.fixture('fake-keyring')
        original=scope.python
        def python(code,**kwargs):
            if "'Delete'" in code or 'shutil.rmtree' in code:raise RecipeBlocked('forced cleanup refusal')
            return original(code,**kwargs)
        with patch.object(scope,'python',python):errors=scope.close()
        self.assertEqual(len(errors),2)
        self.assertFalse(Path(path).exists())

    def test_recording_working_path_follows_the_isolated_data_directory(self):
        scope=self.ctx.manager.scope;scope.initialize()
        data=scope.root+'/data';scope._environment['XDG_DATA_HOME']=data
        with patch.object(self.ctx,'call',return_value={'locations':{'data':{'path':data+'/putgd'}}}):
            self.assertEqual(recordings.actual_working(scope,'owned'),data+'/putgd/recording/owned/working.mkv')
        with patch.object(self.ctx,'call',return_value={'locations':{'data':{'path':str(self.root/'data/putgd')}}}):
            with self.assertRaises(RecipeBlocked):recordings.actual_working(scope,'owned')

    def test_sigterm_and_sighup_run_finalizers_and_stop_the_recipe_sequence(self):
        for sig in (signal.SIGTERM,signal.SIGHUP):
            with self.subTest(signal=sig),tempfile.TemporaryDirectory() as directory:
                notes=[];cleaned=[];ran=[]
                context=SimpleNamespace(evidence=Evidence(directory),begin=lambda r:None,frame=lambda *a:None,frames=[],reviews=[],notes=notes,
                                        review=lambda v:None,note=notes.append,_cleanup_fixtures=lambda:cleaned.append(True) or [])
                def interrupted(ctx):os.kill(os.getpid(),sig)
                def later(ctx):ran.append(True)
                env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
                previous=signal.getsignal(sig)
                rows=run_recipes(context,[Recipe(('editor.undo',),interrupted),Recipe(('editor.undo',),later)],env,directory,'signal-test',Path(directory)/'coverage.json')
                self.assertEqual(cleaned,[True]);self.assertEqual(ran,[])
                self.assertEqual(len(rows),1);self.assertEqual(rows[0].functional,'failed')
                self.assertIn(signal.Signals(sig).name,rows[0].note)
                self.assertEqual(signal.getsignal(sig),previous)

    def test_recipe_cleanup_interrupt_still_unwinds_fixture_finalizers(self):
        with tempfile.TemporaryDirectory() as directory:
            cleaned=[]
            context=SimpleNamespace(evidence=Evidence(directory),begin=lambda r:None,frame=lambda *a:None,frames=[],reviews=[],notes=[],
                                    review=lambda v:None,note=lambda v:None,_cleanup_fixtures=lambda:cleaned.append(True) or [])
            def recipe(ctx):pass
            def interrupt(ctx):raise KeyboardInterrupt
            recipe.cleanup=interrupt
            env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
            alarm=signal.getsignal(signal.SIGALRM)
            with self.assertRaises(KeyboardInterrupt):
                run_recipes(context,[Recipe(('editor.undo',),recipe)],env,directory,'interrupt-test',Path(directory)/'coverage.json')
            self.assertEqual(cleaned,[True])
            self.assertEqual(signal.getsignal(signal.SIGALRM),alarm)
            self.assertEqual(signal.getitimer(signal.ITIMER_REAL)[0],0)

    def test_recording_family_cancels_both_ids_and_removes_clips(self):
        scope=self.ctx.manager.scope;commands=[]
        def capture(s,scene,**kw):
            identity='recording-'+str(len(commands));commands.append(identity)
            path=s.root+'/working/'+identity+'.mkv';s.write(path,b'synthetic clip')
            from tools.lab.surface_driver.fixtures.uploads import track
            track(s,'recording.cancel',identity)
            return identity,path
        scene=Fixture(command=lambda v:None)
        with patch.dict(FACTORIES,{'events.recording-source':lambda s:scene}),patch.object(recordings,'capture',capture):
            self.ctx._ids={}
            value=self.ctx.fixture('recording')
            self.assertTrue(Path(value.working_path).exists())
            self.assertTrue(Path(value.frozen_path).exists())
            self.ctx.manager.close()
        self.assertFalse(Path(value.working_path).exists())
        cancelled=[p['id'] for m,p in self.ctx.calls if m=='recording.cancel']
        self.assertTrue(set(commands).issubset(cancelled))

    def test_selector_contract_observes_heads_and_matches_helper_build(self):
        scope=self.ctx.manager.scope;scope.initialize()
        build={'version':'26.1029.1-dev','gitCommit':'a'*40,'gitTree':'b'*40,'dirty':False}
        logical=[{'rect':[0,0,1280,800],'names':['Virtual-1']},
                 {'rect':[1280,0,1024,768],'names':['Virtual-2']}]
        scope.write(scope.root+'/cover.py',('import json\nprint(json.dumps('+repr({'logical':logical})+'))').encode())
        app=self.root/'putgd';app.write_text('');app.chmod(0o700)
        helper=self.root/'putgd-record'
        def helper_manifest(value):
            helper.write_text('#!/usr/bin/env python3\nimport json\nprint(json.dumps('+repr(value)+'))')
            helper.chmod(0o700)
        helper_manifest(build);self.ctx.target.app_executable=str(app)
        original=self.ctx.call
        self.ctx.call=lambda method,params=None:{'build':build} if method=='ui.snapshot' else original(method,params)
        value=recordings.desktop_contract(scope)
        self.assertEqual(value['logical'],logical)
        self.assertEqual(value['record_helper'],str(helper))
        helper_manifest(dict(build,gitCommit='c'*40))
        with self.assertRaises(RecipeBlocked):recordings.desktop_contract(scope)

    def test_keyring_generation_never_changes_default_or_reads_existing_items(self):
        scope=self.ctx.manager.scope;scope.initialize();scripts=[]
        with patch.object(scope,'json',lambda code:scripts.append(code) or {'collection':'/owned','reference':'owned'}),patch.object(scope,'python',lambda code:scripts.append(code) or ''):
            self.ctx.fixture('fake-keyring');self.ctx.manager.close()
        source='\n'.join(scripts)
        self.assertIn('CreateWithMasterPassword',source)
        self.assertIn("'CreateItem'",source)
        self.assertNotIn('SetAlias',source);self.assertNotIn('GetSecret',source)
        self.assertIn("'Delete'",source)

    def test_quota_family_failure_runs_owned_unmount_only(self):
        from tools.lab.surface_driver.fixtures import events
        self.ctx.fixture('app-profile');commands=[]
        original=self.ctx.shell
        def shell(script,timeout=60):
            if 'sudo -n /usr/bin/mount' in script or 'sudo -n /usr/bin/umount' in script:
                commands.append(script);return {'returncode':0,'stdout':'','stderr':''}
            return original(script,timeout)
        self.ctx.shell=shell
        with patch('tools.lab.surface_driver.fixtures.privileges.ensure'):
            descriptor=self.ctx.fixture('events.record-storage')
            self.ctx.manager.close()
        self.assertTrue(any('umount -- '+descriptor.mountPath in c for c in commands))
        self.assertFalse(Path(descriptor.mountPath).exists())

    def test_feed_family_serves_and_cleans_up_after_failure(self):
        import ast
        scope=self.ctx.manager.scope;original=scope.json
        def metadata(code,**kwargs):
            ast.parse(code)
            if 'SO_PEERCRED' in code:return {'pid':42,'start':'owned-start'}
            return original(code,**kwargs)
        with patch.object(scope,'json',metadata):
            value=self.ctx.fixture('events.update-feed')
        with urlopen(value.url+'stable.json',timeout=2) as response:
            self.assertEqual(json.load(response)['release']['version'],value.version)
        self.assertEqual(value.residentPid,42)
        self.assertEqual(self.ctx.manager.close(),[])
        self.assertFalse(Path(value.root).exists())
        from urllib.error import URLError
        with self.assertRaises(URLError):urlopen(value.url,timeout=1)

    def test_quota_denial_leaves_no_owned_mount_directory(self):
        original=self.ctx.shell
        for deny_preflight in (True,False):
            def shell(script,timeout=60):
                if 'sudo -n /usr/bin/mount' in script:
                    code=1 if deny_preflight or '-t tmpfs' in script else 0
                    return {'returncode':code,'stdout':'','stderr':''}
                return original(script,timeout)
            self.ctx.shell=shell
            with patch('tools.lab.surface_driver.fixtures.privileges.ensure'),self.subTest(deny_preflight=deny_preflight),self.assertRaises(RecipeBlocked):
                self.ctx.fixture('events.record-storage')
            self.assertFalse(list((self.root/'data').glob('quota-*')))

    def test_remote_receipts_cannot_be_mislabelled_as_proton_or_b2(self):
        for destination in ('proton','b2'):
            self.ctx.destination=destination
            for name in ('upload','destination','live'):
                with self.subTest(destination=destination,name=name),self.assertRaises(RecipeBlocked):
                    self.ctx.fixture(name)

    def test_b2_template_uses_https_loopback_and_is_configuration_only(self):
        service=Fixture(profile={'id':'synthetic','provider':'s3','settings':{}},port=12345,endpoint='http://127.0.0.1:12345')
        with patch.dict(FACTORIES,{'loopback-s3':lambda scope:service}):
            value=self.ctx.fixture('destinations-b2')
        self.assertTrue(value.configuration_only)
        self.assertEqual(value.profile['provider'],'backblaze-b2')
        self.assertEqual(value.profile['settings']['apiUrl'],'https://127.0.0.1:12345')
        self.assertTrue(value.profile['settings']['publicUrlBase'].startswith('https://127.0.0.1:'))

    def test_recipe_contract_names(self):
        names={'image','upload','history','history-row','local-history','recording','recording-review','review-controls','destination','mcp','live','release','notification_upload','destinations','selector-desktop','diagnostics-upload','file-dialog-portal','file-dialog-fallback','history_rows','history_pair','history_paging','history_retry_upload','history_retry_local','history_active_uploads','history_delete','history_saved_copy','history_window','launcher','recording-export-failure','recording-budget-stop','recording-preview-error','events.slow-upload','events.update-feed','events.older-update-feed','events.update-notification','events.record-storage','events.import-storage','events.recording-source','events.incomplete-multipart','events.browser-handler','events.software-updates-handler'}
        self.assertFalse(names-FACTORIES.keys())
    def test_seeded_config_references_no_provider_or_credentials(self):
        doc=seeded_config('/tmp/new')
        self.assertEqual(doc['profiles'],[])
        self.assertTrue(all(a['destination']=='local' and a['profileId'] is None for a in doc['actions'].values()))
        self.assertNotIn('secret',json.dumps(doc).lower())
    def test_prelaunch_seed_and_first_launch_option(self):
        seed_profile(self.ctx.target)
        config=self.root/'config/putgd/config.json'
        self.assertEqual(config.stat().st_mode&0o777,0o600)
        self.assertEqual(json.loads(config.read_text())['profiles'],[])
        with self.assertRaises(RecipeBlocked):seed_profile(self.ctx.target)
        config.unlink();seed_profile(self.ctx.target,first_launch=True)
        self.assertFalse(config.exists())
    def test_image_known_pixels_and_cleanup_on_failure(self):
        from PIL import Image
        value=self.ctx.fixture('image')
        with Image.open(value.path) as image:
            self.assertEqual(image.size,(640,320))
            self.assertEqual(len(set(image.getdata())),4)
        self.assertIs(value,self.ctx.fixture('image'))
        self.ctx._deadline=0 # cleanup must not inherit recipe expiry
        self.assertEqual(self.ctx.manager.close(),[])
        self.assertFalse(Path(value.path).exists())
    def test_local_rows_and_settings_are_cleaned_on_failure(self):
        baseline=copy.deepcopy(self.ctx.document)
        row=self.ctx.fixture('history-row')
        self.assertEqual(row.row['id'],row.history_id)
        self.assertIs(row,self.ctx.fixture('history'))
        self.assertIs(row,self.ctx.fixture('local-history'))
        self.assertTrue(Path(row.path).exists())
        self.assertEqual(self.ctx.manager.close(),[])
        self.assertEqual(self.ctx.rows,[])
        self.assertEqual(self.ctx.document,baseline)
    def test_two_independent_operations_and_paging(self):
        pair=self.ctx.fixture('history_pair')
        self.assertEqual(len(set(pair.ids)),2)
        self.assertEqual(len({row['name'] for row in self.ctx.rows}),2)
        self.assertEqual(pair.bytes,600)
        self.ctx.manager.close()
        def grow(scope,total):
            for _ in range(total-1):
                from tools.lab.surface_driver.fixtures.uploads import submit
                submit(scope)
            return {'historyRows':total,'independentOperations':total}
        with patch.object(history,'grow_history',grow):
            self.assertEqual(self.ctx.fixture('history_paging').total,51)
        self.assertEqual(len({r['operationId'] for r in self.ctx.rows}),51)
    def test_retry_local_fault_is_removed_before_recipe(self):
        row=self.ctx.fixture('history_retry_local')
        self.assertEqual(row.state['state'],'failed')
        self.assertTrue(Path(self.ctx.document['archive']['root']).is_dir())
    def test_initial_history_is_preserved(self):
        self.ctx.rows=[{'id':'preexisting'}]
        with self.assertRaisesRegex(RecipeBlocked,'empty owned history'):self.ctx.fixture('app-profile')
        self.assertEqual(self.ctx.rows,[{'id':'preexisting'}])
    def test_settings_states_and_failure_restore(self):
        baseline=copy.deepcopy(self.ctx.document)
        self.ctx.fixture('settings-no-output')
        self.assertFalse(self.ctx.document['archive']['keepUploads'])
        self.ctx.manager.close()
        self.assertEqual(self.ctx.document,baseline)
    def test_finalizers_all_run_in_order_even_when_one_fails(self):
        scope=self.ctx.manager.scope;seen=[]
        scope.defer('last',lambda:seen.append('last'),priority=-1)
        def broken():seen.append('broken');raise ValueError('must never be printed')
        scope.defer('broken',broken)
        scope.defer('first',lambda:seen.append('first'),priority=1)
        self.assertEqual(scope.close(),['broken: ValueError'])
        self.assertEqual(seen,['first','broken','last'])
        self.assertEqual(self.ctx.manager.close(),[])
    def test_partial_factory_failure_and_outer_close(self):
        def fail(scope):
            scope.initialize();scope.write(scope.root+'/partial',b'synthetic')
            raise RecipeFailed('fixture setup failed')
        with patch.dict(FACTORIES,{'partial':fail}):
            with self.assertRaises(RecipeFailed):self.ctx.fixture('partial')
        self.assertFalse(list(self.root.glob('fx-*')))
        self.assertEqual(self.ctx.target.close(),[])
    def test_scope_is_fresh_after_optional_factory_failure(self):
        old=self.ctx.manager.scope
        self.ctx.fixture('image')
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('chooser-failure')
        self.assertIsNot(old,self.ctx.manager.scope)
        new=self.ctx.fixture('image')
        self.assertTrue(Path(new.path).exists())
        self.assertIsNot(old,self.ctx.manager.scope)

    def test_unknown_fixture_and_live_provider_are_specific_blocks(self):
        for name,issue in [('live-provider',669),('chooser-failure',671),('tray-proton-busy',670)]:
            with self.subTest(name=name),self.assertRaises(RecipeBlocked) as caught:self.ctx.fixture(name)
            self.assertEqual(caught.exception.issue,issue)
        with self.assertRaisesRegex(RecipeBlocked,'unknown fixture'):self.ctx.fixture('misspelled')
    def test_destination_and_recording_contracts_have_safe_guards(self):
        self.ctx.placement='native'
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('fake-keyring')
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('events.controllers')
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('events.clipboard')
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('release')
    def test_dialog_environment_applied_before_restart(self):
        self.ctx.fixture('file-dialog-fallback')
        self.assertEqual(Scope.restart.call_args.args[0]['PUTGD_FILE_DIALOG'],'fallback')
        self.ctx.manager.close()
        self.ctx.profile['dialogs']={'backends':{'fallback':{'environment':{'DBUS_SESSION_BUS_ADDRESS':'foreign'}}}}
        with self.assertRaises(RecipeBlocked):self.ctx.fixture('file-dialog-fallback')

    def test_dialog_fixture_prepares_and_retires_diagnostics_start_folder(self):
        baseline=copy.deepcopy(self.ctx.document)
        self.ctx.fixture('file-dialog-fallback')
        folder=Path(self.ctx.document['archive']['root'])/'diagnostics'
        self.assertTrue(folder.is_dir(), 'Nautilus must open the chooser without a missing-folder alert')
        self.assertTrue(folder.is_relative_to(self.root))
        self.assertEqual(folder.stat().st_mode & 0o777,0o700)
        self.assertEqual(list(folder.iterdir()),[])
        self.assertEqual(self.ctx.manager.close(),[])
        self.assertFalse(folder.exists())
        self.assertEqual(self.ctx.document,baseline)
    def test_notification_is_published_lazily_after_needs(self):
        calls=[]
        with patch.object(notifications,'upload',lambda scope:calls.append('publish') or Fixture(link='http://127.0.0.1/synthetic',name='synthetic.png')):
            value=self.ctx.fixture('notification_upload')
            self.assertEqual(calls,[])
            self.assertFalse(hasattr(value,'__html__'))
            with self.assertRaises(TypeError):json.dumps(value)
            self.assertEqual(calls,[])
            self.assertEqual(dict(value)['name'],'synthetic.png')
            self.assertTrue(value.get('link'));self.assertEqual(value.name,'synthetic.png')
            self.assertEqual(calls,['publish'])

    def test_tagged_release_matches_the_links_lane_page_contract(self):
        original=self.ctx.call
        link={'version':'26.1029.1','kind':'stable','url':'https://github.com/patrick-hudson/put.gd-desktop/releases/tag/v26.1029.1'}
        self.ctx.call=lambda method,params=None:{'diagnostics':{'release':link}} if method=='ui.snapshot' else original(method,params)
        value=self.ctx.fixture('release')
        self.assertEqual(value.release_kind,'stable')
        self.assertEqual(value.expect,{'kind':'local','name':link['version'],'preview':False,'url_host':'github.com','text':link['version']})
    def test_runner_cleans_fixture_after_failed_recipe_and_marks_cleanup_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            context=SimpleNamespace(evidence=Evidence(directory),begin=lambda r:None,frame=lambda *a,**k:None,frames=[],reviews=[],notes=[],review=lambda v:None,note=lambda v:None)
            cleaned=[]
            context.note=context.notes.append
            context._cleanup_fixtures=lambda:cleaned.append(True) or ['owned mount: RecipeBlocked']
            def fail(c):raise RecipeFailed('synthetic recipe failure')
            env=dict(desktop='mint-cinnamon-x11',host='boat',placement='boat',guest='cinnamon-x11',package='deb',destination='local',build='abcdef')
            result=run_recipes(context,[Recipe(('editor.undo',),fail)],env,directory,'fixture-test',Path(directory)/'coverage.json')
            self.assertEqual(cleaned,[True]);self.assertEqual(result[0].functional,'failed')
            self.assertIn('synthetic recipe failure',result[0].note)
            self.assertIn('fixture cleanup failed',result[0].note)
    def test_owned_helper_is_stopped_on_recipe_failure(self):
        scope=self.ctx.manager.scope
        process=scope.launch([sys.executable,'-c','import time;time.sleep(30)'],name='owned-helper')
        self.assertEqual(self.ctx.manager.close(),[])
        proc=Path('/proc')/str(process['pid'])/'stat'
        self.assertTrue(not proc.exists() or proc.read_text().split(') ',1)[1].split()[0]=='Z')

    def test_sigkill_waits_for_stubborn_helper_before_removing_directory(self):
        scope=self.ctx.manager.scope;scope.initialize()
        ready=scope.root+'/ready'
        process=scope.launch([sys.executable,'-c',
            'import signal,time;from pathlib import Path;signal.signal(signal.SIGTERM,signal.SIG_IGN);Path('+repr(ready)+').touch();time.sleep(30)'],name='stubborn-helper')
        self.ctx.wait(lambda:Path(ready).exists(),'helper installed SIGTERM handler')
        self.assertEqual(scope.close(),[])
        proc=Path('/proc')/str(process['pid'])/'stat'
        self.assertTrue(not proc.exists() or proc.read_text().split(') ',1)[1].split()[0]=='Z')
        self.assertFalse(Path(scope.root).exists())

    def test_cleanup_never_removes_unowned_sibling(self):
        sibling=self.root/'existing';sibling.write_text('preserve')
        self.ctx.fixture('image');self.ctx.manager.close()
        self.assertEqual(sibling.read_text(),'preserve')

    def test_cleanup_preserves_a_mount_with_escaped_spaces(self):
        scope=Scope(self.ctx);scope.root=str(self.root/'fx with space');scope.initialize()
        scope.write(scope.root+'/preserve',b'synthetic mounted data')
        original=scope.python
        mountinfo='1 0 0:1 / '+scope.root.replace(' ',chr(92)+'040')+' rw - tmpfs tmpfs rw\n'
        def mounted(code,**kwargs):
            return original(code.replace("Path('/proc/self/mountinfo').read_text()",repr(mountinfo)),**kwargs)
        with patch.object(scope,'python',mounted):
            self.assertEqual(scope.close(),['owned directory: RecipeBlocked'])
        self.assertTrue(Path(scope.root+'/preserve').is_file())


class Loopback(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='s3-')
        self.root=Path(self.temp.name);(self.root/'control.json').write_text('{}')
        self.server=Server(self.root)
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.url='http://127.0.0.1:'+str(self.server.server_address[1])
    def tearDown(self):
        self.server.shutdown();self.server.server_close();self.thread.join();self.temp.cleanup()
    def request(self,path,method='GET',body=None):
        return urlopen(Request(self.url+path,data=body,method=method,headers={'Authorization':'synthetic never retain'}),timeout=2)
    def test_upload_public_get_delete_and_no_sensitive_request_log(self):
        self.request('/bucket/synthetic.png','PUT',b'synthetic').close()
        self.assertEqual(self.request('/bucket/synthetic.png').read(),b'synthetic')
        self.assertEqual(self.request('/bucket/synthetic.png','DELETE').status,204)
        with self.assertRaises(HTTPError) as caught:self.request('/bucket/synthetic.png')
        self.assertEqual(caught.exception.code,404)
        raw=json.dumps(self.server.events)
        self.assertNotIn('Authorization',raw);self.assertNotIn('synthetic never retain',raw)
        self.assertNotIn('synthetic.png',raw)
    def test_fail_and_retry_same_key(self):
        (self.root/'control.json').write_text('{"failWrites":true}')
        with self.assertRaises(HTTPError) as caught:self.request('/bucket/file','PUT',b'x')
        self.assertEqual(caught.exception.code,403)
        (self.root/'control.json').write_text('{}')
        self.assertEqual(self.request('/bucket/file','PUT',b'x').status,200)
    def test_completed_multipart_retains_composite_etag(self):
        import xml.etree.ElementTree as ET
        root=ET.fromstring(self.request('/bucket/file?uploads','POST',b'').read())
        identity=root.findtext('{*}UploadId');query='?uploadId='+identity
        self.request('/bucket/file'+query+'&partNumber=1','PUT',b'one').close()
        self.request('/bucket/file'+query+'&partNumber=2','PUT',b'two').close()
        complete=ET.fromstring(self.request('/bucket/file'+query,'POST',b'<CompleteMultipartUpload/>').read())
        etag=complete.findtext('{*}ETag')
        with self.request('/bucket/file') as response:
            self.assertEqual(response.read(),b'onetwo')
            self.assertEqual(response.headers['ETag'],etag)
        self.assertTrue(etag.endswith('-2"'))

    def test_multipart_abort_denial_then_cleanup(self):
        import xml.etree.ElementTree as ET
        root=ET.fromstring(self.request('/bucket/file?uploads','POST',b'').read())
        identity=root.findtext('{*}UploadId');query='?uploadId='+identity
        self.request('/bucket/file'+query+'&partNumber=1','PUT',b'synthetic').close()
        (self.root/'control.json').write_text('{"denyAbort":true}')
        with self.assertRaises(HTTPError):self.request('/bucket/file'+query,'DELETE')
        self.assertIn(identity,self.server.uploads)
        (self.root/'control.json').write_text('{}')
        self.request('/bucket/file'+query,'DELETE').close()
        self.assertFalse(self.server.uploads)


class Restart(unittest.TestCase):
    def setUp(self):
        staging = patch('tools.lab.surface_driver.appimage.prepare')
        self.staging = staging.start()
        self.addCleanup(staging.stop)

    def context(self):
        commands=[]
        ctx=SimpleNamespace(_deadline=time.monotonic()+30)
        ctx.target=SimpleNamespace(root='/tmp/owned-fixture',socket='/tmp/owned-fixture/runtime/putgd/session.sock',app_executable='putgd',started=True)
        ctx.call=lambda *a,**k:{}
        ctx.shell=lambda command,**k:commands.append(command) or {'returncode':0,'stdout':'','stderr':''}
        ctx.cli=lambda *a,**k:{'returncode':0,'stdout':'{"ok":true}'}
        ctx.wait=lambda predicate,*a,**k:predicate()
        return ctx,commands

    def test_before_failure_relaunches_and_preserves_the_original_error(self):
        ctx,commands=self.context();scope=Scope(ctx)
        original=RecipeFailed('forced before failure')
        def before():raise original
        with self.assertRaises(RecipeFailed) as caught:scope.restart(before=before)
        self.assertIs(caught.exception,original)
        self.assertTrue(ctx.target.started)
        self.assertEqual(sum(c.startswith('nohup') for c in commands),1)
        self.staging.assert_called_once_with(ctx.target)

    def test_relaunch_failure_recovers_resident_and_surfaces_original_error(self):
        ctx,commands=self.context();scope=Scope(ctx);launches=[]
        original=RecipeBlocked('forced relaunch failure')
        def launch(command,**kwargs):
            commands.append(command)
            if command.startswith('nohup'):
                launches.append(command)
                if len(launches)==1:raise original
            return {'returncode':0,'stdout':'','stderr':''}
        ctx.shell=launch
        with self.assertRaises(RecipeBlocked) as caught:scope.restart()
        self.assertIs(caught.exception,original)
        self.assertTrue(ctx.target.started);self.assertEqual(len(launches),2)

    def test_before_error_survives_a_second_relaunch_failure(self):
        ctx,_=self.context();scope=Scope(ctx)
        original=RecipeFailed('original mutation failure')
        def before():raise original
        with patch.object(scope,'_relaunch',side_effect=RecipeBlocked('launch error')):
            with self.assertRaises(RecipeFailed) as caught:scope.restart(before=before)
        self.assertIs(caught.exception,original)

    def test_scope_restores_launch_environment_once(self):
        commands=[]
        ctx=SimpleNamespace(_deadline=time.monotonic()+30)
        ctx.target=SimpleNamespace(root='/tmp/owned-fixture',socket='/tmp/owned-fixture/runtime/putgd/session.sock',app_executable='putgd',started=True)
        ctx.call=lambda *a,**k:{}
        ctx.shell=lambda command,**k:commands.append(command) or {'returncode':0,'stdout':'','stderr':''}
        ctx.cli=lambda *a,**k:{'returncode':0,'stdout':'{"ok":true}'}
        ctx.wait=lambda predicate,*a,**k:predicate()
        scope=Scope(ctx)
        scope.restart({'PUTGD_FILE_DIALOG':'fallback'})
        scope.restart({'PUTGD_UPDATE_FEED_URL':'http://127.0.0.1:1/'})
        self.assertEqual(sum(label=='resident environment' for _,label,_ in scope.finalizers),1)
        self.assertEqual(scope.close(),[])
        launches=[c for c in commands if c.startswith('nohup')]
        self.assertEqual(len(launches),3)
        self.assertIn('PUTGD_FILE_DIALOG',launches[1])
        self.assertIn('PUTGD_UPDATE_FEED_URL',launches[1])
        self.assertNotIn('PUTGD_FILE_DIALOG',launches[-1])
        self.assertNotIn('PUTGD_UPDATE_FEED_URL',launches[-1])


if __name__=='__main__':unittest.main()
