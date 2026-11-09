#!/usr/bin/env python3
"""Seconds-long fixture contracts. No product binary, GUI, VM or cloud account."""
import copy
import json
import os
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

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.fixtures import FixtureManager
from tools.lab.surface_driver.fixtures.base import Scope
from tools.lab.surface_driver.fixtures import privileges
from tools.lab.surface_driver.fixtures.profile import seeded_config
from tools.lab.surface_driver.fixtures.s3_server import Server


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

    def test_s3_live_blocks_before_starting_any_dependencies(self):
        self.ctx.destination='s3'
        with self.assertRaisesRegex(RecipeBlocked,'sensitive_transport'):
            self.ctx.fixture('live')
        self.assertFalse(list(self.root.glob('fx-*')))
        self.assertEqual(self.ctx.calls,[])

    def test_keyring_generation_never_changes_default_or_reads_existing_items(self):
        scope=self.ctx.manager.scope;scope.initialize();scripts=[]
        with patch.object(scope,'json',lambda code:scripts.append(code) or {'collection':'/owned','reference':'owned'}),patch.object(scope,'python',lambda code:scripts.append(code) or ''):
            self.ctx.fixture('fake-keyring');self.ctx.manager.close()
        source='\n'.join(scripts)
        self.assertIn('CreateWithMasterPassword',source)
        self.assertIn("'CreateItem'",source)
        self.assertNotIn('SetAlias',source);self.assertNotIn('GetSecret',source)
        self.assertIn("'Delete'",source)

    def test_remote_receipts_cannot_be_mislabelled_as_proton_or_b2(self):
        for destination in ('proton','b2'):
            self.ctx.destination=destination
            for name in ('upload','destination','live'):
                with self.subTest(destination=destination,name=name),self.assertRaises(RecipeBlocked):
                    self.ctx.fixture(name)

    def test_seeded_config_references_no_provider_or_credentials(self):
        doc=seeded_config('/tmp/new')
        self.assertEqual(doc['profiles'],[])
        self.assertTrue(all(a['destination']=='local' and a['profileId'] is None for a in doc['actions'].values()))
        self.assertNotIn('secret',json.dumps(doc).lower())
    def test_finalizers_all_run_in_order_even_when_one_fails(self):
        scope=self.ctx.manager.scope;seen=[]
        scope.defer('last',lambda:seen.append('last'),priority=-1)
        def broken():seen.append('broken');raise ValueError('must never be printed')
        scope.defer('broken',broken)
        scope.defer('first',lambda:seen.append('first'),priority=1)
        self.assertEqual(scope.close(),['broken: ValueError'])
        self.assertEqual(seen,['first','broken','last'])
        self.assertEqual(self.ctx.manager.close(),[])

    def test_unknown_fixture_and_live_provider_are_specific_blocks(self):
        for name,issue in [('live-provider',669),('chooser-failure',671),('tray-proton-busy',670)]:
            with self.subTest(name=name),self.assertRaises(RecipeBlocked) as caught:self.ctx.fixture(name)
            self.assertEqual(caught.exception.issue,issue)
        with self.assertRaisesRegex(RecipeBlocked,'unknown fixture'):self.ctx.fixture('misspelled')

    def test_cleanup_never_removes_unowned_sibling(self):
        sibling=self.root/'existing';sibling.write_text('preserve')
        self.ctx.fixture('image');self.ctx.manager.close()
        self.assertEqual(sibling.read_text(),'preserve')


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


if __name__=='__main__':unittest.main()
