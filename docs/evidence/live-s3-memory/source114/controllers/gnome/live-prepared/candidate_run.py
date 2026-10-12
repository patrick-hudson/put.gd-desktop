#!/usr/bin/env python3
"""Adapted sibling live workload: fresh identities and exact cleanup.

Host-only Python. Credentials are read only by the existing guarded setup phase.
All outputs are private. Never reuse a bundle or remote namespace.
"""
import argparse, copy, hashlib, importlib.util, io, json, os, shlex, subprocess, sys, time, urllib.request, uuid
from pathlib import Path

ORIGINAL_BASE=Path('/home/patrick/work/put.gd/work/live-s3-memory')
BASE=Path('/home/patrick/work/put.gd/work/s3-fix/gfull1')
CAMPAIGN=BASE
ADAPTER=Path(__file__).resolve().parent
SRC=Path('/home/patrick/.codex/worktrees/live-s3-memory/put.gd')
RUN=Path('/home/patrick/work/put.gd/work/lab-runs/p2g-a1')
os.umask(0o077)
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
driver=load('issue7_driver',ADAPTER/'lab_run.py');lab=load('candidate_lab',SRC/'tools/lab/lab.py')
def save(path,value):
    path.write_text(json.dumps(value,indent=2)+'\n');assert json.loads(path.read_text())==value
def stage(name):
    lab.ssh(RUN,"python3 - <<'PY'\nfrom pathlib import Path\nimport json\n(Path.home()/'putgd-lab/stage.json').write_text(json.dumps({'stage':"+repr(name)+"}))\nPY\n")
def live_gate(path):
    record=json.loads(Path(path).read_text());spec=importlib.util.spec_from_file_location('native_gate',BASE/'prepared/driver.py');native=importlib.util.module_from_spec(spec);spec.loader.exec_module(native)
    pin=native.validate_pin();native.expansion_gate(pin['expansionRelease']['path'])
    assert record['candidatePinSha256']==native.sha(BASE/'candidate.json') and record['expansionRelease']==pin['expansionRelease']
    assert all(record.get(k) is True for k in ('liveS3Authorized','vmOwnershipTransferred','parentLiveS3Drained','hostQuiet','visibleNativeSessionVerified'))
    f=record['nativeFunctional'];assert native.sha(f['path'])==f['sha256'];functional=native.read(f['path']);assert functional['functionalPassed'] is True and functional['candidateBuild']==pin['build'] and functional['candidatePinSha256']==record['candidatePinSha256']
    assert native.sha(RUN/'state.json')==pin['runStateSha256'] and json.loads((RUN/'package.json').read_text())['sha256']==pin['packageSha256']
    return native,pin,record

def main():
    ap=argparse.ArgumentParser();ap.add_argument('phase',choices=['prepare','observe','pixels','finalize','finish','cleanup-review','cleanup-verify','credentials','fixture','batch','cleanup-prepare','cleanup-confirm']);ap.add_argument('--go',type=Path,required=True);ap.add_argument('--bundle',type=Path,required=True);ap.add_argument('--deb',type=Path);ap.add_argument('--count',type=int,default=5);ap.add_argument('--kind',default='region');a=ap.parse_args();root=a.bundle.resolve()
    assert root.is_relative_to(BASE) and root!=BASE and not any(root.is_relative_to(BASE/name) for name in ('prepared','live-prepared')),'bundle_must_be_new_private_output'
    native,pin,go=live_gate(a.go)
    pre=subprocess.check_output(['python3',str(SRC/'tools/diagnostics/check_socket_paths.py'),'--socket',str(RUN/'qmp.sock'),pin['waylandSocket'],'/run/user/1000/putgd/session.sock','/run/user/1000/bus','/tmp/.X11-unix/X0','/run/dbus/system_bus_socket'],text=True)
    native.verify_wayland(lab,pin['waylandSocket'])
    driver.SRC=SRC;driver.RUN=RUN;driver.ROOT=root;driver.lab=lab;driver.prepared_live_authorized=True
    if a.phase=='prepare':
        assert root.is_dir() and not (root/'baseline102.json').exists()
        # Native acceptance must have exited normally before a fresh live profile.
        family=json.loads(lab.ssh(RUN,"python3 - <<'PY'\nimport os,json\nfrom pathlib import Path\na=[]\nfor p in Path('/proc').iterdir():\n if not p.name.isdecimal():continue\n try:\n  if p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip().startswith('putgd'):a.append(int(p.name))\n except (FileNotFoundError,ProcessLookupError):pass\nprint(json.dumps(a))\nPY\n"));assert not family,'product_still_running_before_fresh_profile'
        name='putgd-lab-before-'+root.name+'-'+uuid.uuid4().hex[:8]
        prefix='putgd-issue7-'+uuid.uuid4().hex+'-';doc=copy.deepcopy(json.loads((ORIGINAL_BASE/'baseline102.json').read_text())['settings']['document'])
        if lab.state(RUN)['profile']=='kde-wayland':
            for action,binding in lab.load_profile('kde-wayland')['fixture_hotkeys'].items():doc['actions'][action]['shortcut']=binding['shortcut']
        for field,template in {'fileTemplate':'{name}-{uuid}.{ext}','captureTemplate':'Screen-Capture-{uuid}.{ext}','imageTemplate':'Image-{uuid}.{ext}','textTemplate':'Text-{uuid}.{ext}'}.items():doc['naming'][field]=prefix+template
        code="from pathlib import Path\nimport json,os\nos.umask(0o077)\nh=Path.home();old=h/'putgd-lab';archive=h/"+repr(name)+"\nassert not archive.exists()\nif old.exists():old.rename(archive)\np=h/'putgd-lab/config/putgd/putgd/config.json';p.parent.mkdir(parents=True,mode=0o700);p.write_text(json.dumps("+repr(doc)+",indent=2)+'\\n');p.chmod(0o600)\n"
        lab.ssh(RUN,"python3 - <<'PY'\n"+code+"\nPY\n")
        lab.guest_session(RUN,'export WAYLAND_DISPLAY='+shlex.quote(Path(pin['waylandSocket']).name)+'\nulimit -c 0\nnohup putgd-launcher --background > "$HOME/putgd-lab/app.log" 2>&1 < /dev/null &\n');d=driver.wait_ready();identity=lab.verify_running_app(RUN);config=lab.cli(RUN,'settings.get');assert config['document']==doc and config['metadata']['validation']['ok'];assert not d['build']['dirty']
        assert d['build']==pin['build'] and identity['sha256']==pin['installedBinarySha256']['/usr/bin/putgd']
        code="from pathlib import Path\nimport json\np=Path('/proc/"+str(identity['pid'])+"/limits');line=next(v for v in p.read_text().splitlines() if v.startswith('Max core file size'));v=line[len('Max core file size'):].split();print(json.dumps({'softBytes':int(v[0]),'hardBytes':int(v[1])}))\n"
        core=json.loads(lab.ssh(RUN,"python3 - <<'PY'\n"+code+"\nPY\n"));assert core=={'softBytes':0,'hardBytes':0};save(root/'core-limit.json',core)
        m={'prefix':prefix,'profileId':'issue7-s3','resident':identity,'build':d['build'],'settings':config,'operationIds':[],'historyIds':[],'cleanup':[],'priorProfileArchive':name,'package':json.loads((RUN/'package.json').read_text()),'socketPreflight':json.loads(pre),'quietStage':'candidate-final-quiet'};save(root/'baseline102.json',m);driver.shot(root/'fresh.ppm')
    elif a.phase=='observe':
        m=json.loads((root/'baseline102.json').read_text());assert lab.verify_running_app(RUN)==m['resident'];lab.put(RUN,ORIGINAL_BASE/'observe_guest.py','/home/lab/putgd-lab/observe_guest.py');r=m['resident'];stage('cold')
        cmd=['python3','/home/lab/putgd-lab/observe_guest.py','--pid',str(r['pid']),'--start-ticks',str(r['startTicks']),'--sha256',r['sha256'],'--output','/home/lab/putgd-lab/observed','--seconds','3600'];save(root/'observer-command.json',{'argv':cmd,'sha256':hashlib.sha256((ORIGINAL_BASE/'observe_guest.py').read_bytes()).hexdigest()})
        lab.guest_session(RUN,'nohup '+shlex.join(cmd)+' > "$HOME/putgd-lab/observer.log" 2>&1 < /dev/null &\n');time.sleep(3);assert json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/observed/parameters.json"\n'))['build']==m['build']
    elif a.phase=='pixels':
        from PIL import Image
        cases=[];m=json.loads((root/'baseline102.json').read_text())
        for p in sorted(root.glob('batch-*/receipt.json')):
            b=json.loads(p.read_text())
            if not b['passed'] or b.get('kind')!='region':continue
            for index,ident in enumerate(b['operations']):
                assert ident in m['operationIds'];op=json.loads((p.parent/('operation-'+str(index)+'.json')).read_text());item=op['items'][0];identity=item['receipt']['objectReference'];assert identity['bytes']<=8*1024*1024
                with urllib.request.urlopen(item['receipt']['directUrl'],timeout=20) as response:body=response.read(identity['bytes']+1)
                assert len(body)==identity['bytes'] and hashlib.sha256(body).hexdigest()==identity['sha256'];image=Image.open(io.BytesIO(body)).convert('RGB');assert image.size==(80,60) and image.getextrema()==((220,220),(30,30),(40,40));cases.append({'operationId':ident,'width':80,'height':60,'everyPixelRgb':[220,30,40],'passed':True});body=b''
        assert len(cases)==15;save(root/'public-region-pixel-oracle.json',{'passed':True,'cases':cases})
    elif a.phase=='finalize':
        m=json.loads((root/'baseline102.json').read_text());out=root/'final-operations';out.mkdir(mode=0o700)
        for ident in m['operationIds']:
            op=lab.cli(RUN,'operation.inspect',{'id':ident});assert op['state']=='completed' and op['copyStatus']=='copied';save(out/(ident+'.json'),op)
        d=lab.cli(RUN,'ui.snapshot')['diagnostics'];assert d['activeOperations']==0 and d['ui']['pid']==0 and d['storage']['s3']['queuedRequests']==0 and d['storage']['s3']['moduleLiveDrivers']==0;save(root/'idle-resources.json',d);stage(m['quietStage'])
    elif a.phase=='finish':
        m=json.loads((root/'baseline102.json').read_text());progress=json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/observed/progress.json"\n'));assert progress['stage']==m['quietStage']
        # Inspect recorded samples, not wall-clock guesses, before stopping.
        check="import json\nfrom pathlib import Path\np=Path.home()/'putgd-lab/observed';rows=[json.loads(l) for l in (p/'samples.jsonl').read_text().splitlines()];q=[r for r in rows if r['stage']=="+repr(m['quietStage'])+"];assert q and q[-1]['elapsedSeconds']-q[0]['elapsedSeconds']>=310,'quiet_window_incomplete';(p/'stop').touch()\n"
        lab.ssh(RUN,"python3 - <<'PY'\n"+check+"\nPY\n")
        for _ in range(30):
            try:done=json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/observed/completed.json"\n'));break
            except lab.LabError:time.sleep(1)
        assert done['residentIdentityUnchanged']
        for f in ['parameters.json','samples.jsonl','completed.json']:lab.get(RUN,'/home/lab/putgd-lab/observed/'+f,root/f)
    elif a.phase=='cleanup-review':
        m=json.loads((root/'baseline102.json').read_text());p=json.loads((root/'cleanup-prepared.json').read_text());targets=p['prepared']['targets']
        # The frozen driver writes this root copy before its final passed flag.
        # Require an identical, actually completed phase receipt; never infer
        # success from the existence of a prepared acknowledgement alone.
        completed=[json.loads(f.read_text()) for f in root.glob('cleanup-prepare-*/receipt.json')]
        assert any(c['passed'] and c['ids']==p['ids'] and c['prepared']==p['prepared'] and c['argv']==p['argv'] for c in completed),'completed_prepare_receipt_missing'
        assert set(p['ids'])==set(m['historyIds']) and {t['historyId'] for t in targets}==set(m['historyIds'])
        receipts={}
        for f in root.glob('batch-*/operation-*.json'):
            op=json.loads(f.read_text())
            if op['id'] in m['operationIds']:
                for item in op['items']:receipts[item['artifact']['id']]=item['receipt']
        assert len(targets)==len(receipts)==31
        for t in targets:
            r=t['receipt'];assert r==receipts[t['historyId']] and r['destination']['profileId']==m['profileId'] and r['objectReference']['key'].startswith(m['prefix']) and '/' not in r['objectReference']['key'];assert r['publicationId'] and r['concurrencyToken'] and t['effect']=='Guarded origin removal; selected versions only; browser/CDN caches may retain bytes'
        p['passed']=True;p['reviewedExactRunOwnedEffects']=True;p['reviewedTargetCount']=len(targets);p['reviewScope']='Exact IDs/receipts/ETags/publication IDs/flat namespace from this isolated synthetic run; no bucket policy, versioning or existing objects';save(root/'cleanup-prepared.json',p)
    elif a.phase=='cleanup-verify':
        m=json.loads((root/'baseline102.json').read_text());started=json.loads((root/'cleanup-started.json').read_text());ident=started['delete']['deleteOperationId']
        for _ in range(600):
            deletion=lab.cli(RUN,'delete.inspect',{'id':ident});save(root/'cleanup-progress.json',deletion)
            if deletion['state'] in ('completed','partial','failed','cancelled','interrupted'):break
            time.sleep(.2)
        driver.diagnostic_event({'event':'delete_terminal','deleteOperationId':ident,'operationState':deletion['state'],'items':[{'historyId':x['historyId'],'status':x['status'],'progressPhase':x.get('progress',{}).get('phase'),'error':driver.bounded_error(x.get('error') or {})} for x in deletion['items']],'workerExitCodeUnavailable':True})
        assert deletion['state']=='completed' and {i['historyId'] for i in deletion['items']}==set(m['historyIds']) and all(i['status']=='removed' and i['progress']['phase']=='done' for i in deletion['items']);save(root/'cleanup-completed.json',deletion)
        details=[lab.cli(RUN,'history.detail',{'id':i}) for i in m['historyIds']];assert all(not d['linkAvailable'] and d['remoteStatus']=='removed' for d in details);save(root/'cleanup-history.json',details)
        # Remove exact synthetic archive copies only after verifying every file.
        targets=[]
        for p in (root/'final-operations').glob('*.json'):
            for item in json.loads(p.read_text())['items']:
                assert item['local']['status']=='saved'
                targets.append({'path':item['local']['path'],'bytes':item['artifact']['bytes'],'sha256':item['artifact']['sha256']})
        save(root/'cleanup-local-input.json',targets)
        code="""from pathlib import Path
import json,os,stat,hashlib
root=Path.home()/'putgd-lab/archive';targets=INPUT;checked=[]
for target in targets:
 p=Path(target['path']);s=p.lstat()
 assert stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and p.resolve().is_relative_to(root.resolve())
 assert p.name.startswith(PREFIX) and p.stat().st_size==target['bytes'] and hashlib.sha256(p.read_bytes()).hexdigest()==target['sha256']
 checked.append(p)
assert len(set(checked))==len(targets)
for p in checked:p.unlink()
print(json.dumps({'localArchiveRemoved':len(checked)}))
""".replace('INPUT',repr(targets)).replace('PREFIX',repr(m['prefix']))
        local=json.loads(lab.ssh(RUN,"python3 - <<'PY'\n"+code+"\nPY\n"));save(root/'cleanup-local.json',local)
        cache=lab.cli(RUN,'cache.clear');assert cache['removedOperations']==len(m['operationIds']);save(root/'cleanup-cache.json',cache)
        save(root/'cleanup-verified.json',{'remoteObjectsRemoved':len(deletion['items']),**local,'successfulArtifactSpoolsCleared':cache['removedOperations'],'historyMetadataPreserved':True,'allHistoryLinksUnavailable':True})
    else:
        if a.phase=='batch':
            # Reserve conservative bytes for the verified synthetic fixture and
            # fixed1280x800 display before starting any publication. Failed or
            # ambiguous attempts keep their reservation; never silently replay.
            assert 1<=a.count<=20
            bounds={'clipboard-text':65536,'clipboard-image':65536,'clipboard-files':131072,'region':65536,'window':2097152,'desktop':5242880}
            assert a.kind in bounds
            ledger=CAMPAIGN/'budget.json'
            budget_api=load('locked_budget',CAMPAIGN/'budget.py')
            manifest=json.loads((root/'baseline102.json').read_text());attempted=manifest.setdefault('attemptedCounts',{});expected={'clipboard-text':11,'clipboard-image':1,'clipboard-files':1,'region':15,'window':1,'desktop':1}
            assert attempted.get(a.kind,0)+a.count<=expected[a.kind],'scenario_count_exceeded_no_replay'
            reservation=budget_api.reserve(ledger,root,a.kind,a.count);attempted[a.kind]=attempted.get(a.kind,0)+a.count;manifest.setdefault('budgetReservations',[]).append(reservation);save(root/'baseline102.json',manifest)
        sys.argv=[str(ADAPTER/'lab_run.py'),a.phase,'--count',str(a.count),'--kind',a.kind];driver.main()
    print(json.dumps({'phase':a.phase,'bundle':str(root),'passed':True}))
if __name__=='__main__':main()
