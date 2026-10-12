#!/usr/bin/env python3
"""Private issue-7 driver. Uses production APIs; never saves credential values.

Every phase has a fresh receipt. Remote cleanup uses only this run's exact IDs.
No bucket listing, policy changes, broad deletes or user payloads.
"""
import argparse, datetime, hashlib, importlib.util, json, os, shlex, stat, subprocess, time, uuid, urllib.request
from pathlib import Path

SRC = Path('/home/patrick/.codex/worktrees/kde-native-boundary/put.gd')
ROOT = Path('/home/patrick/work/put.gd/work/live-s3-memory')
RUN = Path('/home/patrick/work/put.gd/work/lab-runs/p2c-a1')
os.umask(0o077)
spec = importlib.util.spec_from_file_location('lab', SRC/'tools/lab/lab.py')
lab = importlib.util.module_from_spec(spec); spec.loader.exec_module(lab)
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
save = lambda p,v: Path(p).write_text(json.dumps(v,indent=2)+'\n')

def shot(path):
    with lab.QMP(RUN/'qmp.sock') as q:
        assert q.execute('query-status')['running']
        q.execute('screendump', {'filename':str(path),'device':'video0','head':0})
    from PIL import Image
    Image.open(path).save(path.with_suffix('.png'))

def wait_ready():
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        try:
            d=lab.cli(RUN,'diagnostics')
            if d['journalReady']: return d
        except lab.LabError: pass
        time.sleep(.2)
    raise RuntimeError('journal_readiness_timeout')

def main():
    ap=argparse.ArgumentParser();ap.add_argument('phase',choices=['install','start','credentials','fixture','batch','cleanup-prepare','cleanup-confirm'])
    ap.add_argument('--count',type=int,default=5);ap.add_argument('--kind',choices=['clipboard-text','clipboard-image','clipboard-files','region','window','desktop'],default='region')
    a=ap.parse_args(); phase=ROOT/(a.phase+'-'+uuid.uuid4().hex[:10]);phase.mkdir(mode=0o700)
    r={'phase':a.phase,'argv':os.sys.argv,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'driverSha256':sha(__file__),'passed':False};save(phase/'receipt.json',r)
    pre=['python3',str(SRC/'tools/diagnostics/check_socket_paths.py'),'--socket',str(RUN/'qmp.sock'),'/run/user/1000/putgd/session.sock','/run/user/1000/bus','/tmp/.X11-unix/X0','/run/dbus/system_bus_socket']
    r['socketPreflight']=json.loads(subprocess.check_output(pre,text=True))
    try:
        if a.phase=='install':
            old=lab.verify_running_app(RUN);d=lab.cli(RUN,'diagnostics');assert d['activeOperations']==0 and lab.cli(RUN,'ui.snapshot')['diagnostics']['ui']['pid']==0
            r['oldResident']=old;r['normalQuit']=lab.cli(RUN,'ui.quit');time.sleep(2)
            lab.ssh(RUN,'set -eu\n[ ! -e "$HOME/putgd-lab-pre-issue7" ]\nmv "$HOME/putgd-lab" "$HOME/putgd-lab-pre-issue7"\numask 077\nmkdir -m700 "$HOME/putgd-lab"\n')
            package=next((ROOT/'packages102').glob('*.deb'));r['package']={'path':str(package),'sha256':sha(package)}
            p=subprocess.run(['python3',str(SRC/'tools/lab/lab.py'),'install','--run',str(RUN),'--deb',str(package)],capture_output=True,text=True)
            (phase/'install.stdout').write_text(p.stdout);(phase/'install.stderr').write_text(p.stderr);assert p.returncode==0,'package_install_failed'
            r['packageReceipt']=json.loads((RUN/'package.json').read_text())
        elif a.phase=='start':
            lab.guest_session(RUN,'nohup putgd-launcher --background > "$HOME/putgd-lab/app.log" 2>&1 < /dev/null &\n')
            r['diagnostics']=wait_ready();r['resident']=lab.verify_running_app(RUN)
            prefix='putgd-lab/issue7/'+uuid.uuid4().hex+'/'
            doc=lab.cli(RUN,'settings.developmentTemplate')['document'];p=doc['profiles'][0];p['id']='issue7-s3';p['label']='S3 memory test';p['credentials']={'mode':'session','reference':None};p['settings']['objectPrefix']=prefix
            for action,v in doc['actions'].items():v['profileId']=p['id'];v['destination']='upload';v['shortcut']='Ctrl+Shift+'+str(['region','window','desktop','clipboard'].index(action)+1)
            doc['archive']['root']='/home/lab/putgd-lab/archive';doc['archive']['keepUploads']=True;doc['preferences']={'notifications':False,'launchAtLogin':False}
            st=lab.cli(RUN,'settings.get');lab.cli(RUN,'settings.patch',{'patch':doc,'expectedRevision':st['metadata']['observedRevision']})
            config=lab.cli(RUN,'settings.get');assert config['document']==doc and config['metadata']['validation']['ok']
            manifest={'prefix':prefix,'profileId':p['id'],'resident':r['resident'],'build':r['diagnostics']['build'],'settings':config,'operationIds':[],'historyIds':[],'cleanup':[]}
            save(ROOT/'baseline102.json',manifest);shot(phase/'started.ppm')
        elif a.phase=='credentials':
            manifest=json.loads((ROOT/'baseline102.json').read_text());assert lab.verify_running_app(RUN)==manifest['resident']
            path=Path('/home/patrick/work/put.gd/credentials.txt');st=path.lstat();assert stat.S_ISREG(st.st_mode) and st.st_uid==os.getuid() and not st.st_mode&0o077
            values={k.strip():v.strip() for line in path.read_text().splitlines() if ':' in line for k,v in [line.split(':',1)]}
            document=lab.cli(RUN,'settings.get')['document'];settings=document['profiles'][0]['settings'];assert settings['bucket']==values['Bucket Name'] and settings['region']==values['AWS Region ID']
            payload={'profileId':manifest['profileId'],'expectedRevision':manifest['settings']['metadata']['acceptedRevision'],'credentials':{'accessKey':values['Access Key ID'],'secretKey':values['Secret Access Key']}}
            # Script is sent over encrypted SSH stdin; keys never enter argv or a file.
            code='''import json,socket,uuid,time
S='/run/user/1000/putgd/session.sock'
def call(method,params):
 with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
  s.settimeout(20);s.connect(S);s.sendall(json.dumps(dict(schemaVersion=1,requestId=str(uuid.uuid4()),session=S,method=method,params=params)).encode()+b'\\n');b=b''
  while not b.endswith(b'\\n'):
   c=s.recv(65536)
   if not c:raise RuntimeError('transport_closed')
   b+=c
  v=json.loads(b)
  if not v['ok']:raise RuntimeError(v['error']['code'])
  return v['result']
try:
 job=call('setup.storeCredentials',INPUT)
 for _ in range(100):
  status=call('setup.inspect',dict(id=job['setupId']))
  if status['state']!='running':break
  time.sleep(.1)
 print(json.dumps({'state':status['state'],'errorCode':status.get('error',{}).get('code')}))
except Exception:print(json.dumps({'state':'failed','errorCode':'credential_setup_failed'}))
'''
            code=code.replace('INPUT',repr(payload));status=json.loads(lab.ssh(RUN,"python3 - <<'PY'\n"+code+"\nPY\n"));payload.clear();values.clear();code=''
            r['credentialMode']='session';r['credentialState']=status;assert status['state']=='completed'
        elif a.phase=='fixture':
            lab.guest_session(RUN,'nohup putgd-desktop-fixture --output "$HOME/putgd-lab/i7-fixture" > /dev/null 2>&1 < /dev/null &\n')
            deadline=time.monotonic()+20
            while time.monotonic()<deadline:
                try:r['ready']=json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/i7-fixture/ready.json"\n'));break
                except lab.LabError:time.sleep(.2)
            shot(phase/'fixture.ppm');r['geometry']=lab.locate_fixture_paint(phase/'fixture.ppm');assert r['geometry']
            save(ROOT/'fixture.json',r)
        elif a.phase=='batch':
            assert 1<=a.count<=20;manifest=json.loads((ROOT/'baseline102.json').read_text());assert lab.verify_running_app(RUN)==manifest['resident'];g=json.loads((ROOT/'fixture.json').read_text())['geometry'];r['kind']=a.kind;r['count']=a.count;r['operations']=[]
            assert lab.cli(RUN,'ui.snapshot')['diagnostics']['ui']['pid']==0
            shot(phase/'before.ppm');assert lab.locate_fixture_paint(phase/'before.ppm')==g,'fixture_not_visible'
            lab.ssh(RUN,"python3 - <<'PY'\nfrom pathlib import Path\nimport json\n(Path.home()/'putgd-lab/stage.json').write_text(json.dumps({'stage':"+repr('upload-'+a.kind+'-'+phase.name)+"}))\nPY\n")
            for index in range(a.count):
                lab.pointer(RUN,g['x']+30,g['y']+30,1280,800,True);lab.pointer(RUN,g['x']+30,g['y']+30,1280,800,False)
                if a.kind.startswith('clipboard'):
                    lab.key(RUN,['ctrl','alt','shift',{'clipboard-text':'t','clipboard-image':'i','clipboard-files':'f'}[a.kind]]);time.sleep(.3);action='clipboard'
                    source=json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/i7-fixture/source.json"\n'));assert source['synthetic'] and source['key']==ord({'clipboard-text':'T','clipboard-image':'I','clipboard-files':'F'}[a.kind])
                else:action=a.kind
                invoked=lab.cli(RUN,'action.invoke',{'action':action});opid=invoked['operationId'];manifest['operationIds'].append(opid);save(ROOT/'baseline102.json',manifest)
                if action=='region':
                    time.sleep(.5);x,y=g['x']+40,g['y']+40;lab.pointer(RUN,x,y,1280,800,True);time.sleep(.1);lab.pointer(RUN,x+80,y+60,1280,800);time.sleep(.1);lab.pointer(RUN,x+80,y+60,1280,800,False)
                deadline=time.monotonic()+100
                while time.monotonic()<deadline:
                    op=lab.cli(RUN,'operation.inspect',{'id':opid})
                    if op['state'] in ('completed','failed','partial','cancelled','interrupted'):break
                    time.sleep(.2)
                save(phase/('operation-'+str(index)+'.json'),op)
                for item in op['items']:
                    if item['uploadStatus']=='uploaded':
                        key=item['receipt']['objectReference']['key'];assert key.startswith(manifest['prefix'])
                assert op['state']=='completed' and all(i['uploadStatus']=='uploaded' for i in op['items']),'upload_not_completed'
                rows=lab.cli(RUN,'history.list',{'limit':200,'offset':0,'profileId':manifest['profileId']})
                # Capture exact run-owned IDs immediately for receipt-guarded cleanup.
                r['historyResponse']=rows
                entries=rows['records']
                manifest['historyIds']=list(dict.fromkeys(manifest['historyIds']+[i['id'] for i in entries]));save(ROOT/'baseline102.json',manifest)
                verified=[]
                for item in op['items']:
                    receipt=item['receipt'];identity=receipt['objectReference'];limit=int(identity['bytes'])
                    assert 0<=limit<=8*1024*1024,'public_check_size_bound'
                    with urllib.request.urlopen(receipt['directUrl'],timeout=20) as response:
                        assert response.status==200;body=response.read(limit+1)
                    digest=hashlib.sha256(body).hexdigest();assert len(body)==limit and digest==identity['sha256'],'public_bytes_mismatch'
                    if a.kind=='clipboard-text':assert body==b'putgd synthetic clipboard text'
                    verified.append({'bytes':len(body),'sha256':digest,'publicExactBytesMatched':True});body=b''
                lab.ssh(RUN,'rm -f "$HOME/putgd-lab/i7-fixture/paste.json"\n');lab.key(RUN,['ctrl','v']);deadline=time.monotonic()+10
                while time.monotonic()<deadline:
                    try:paste=json.loads(lab.ssh(RUN,'cat "$HOME/putgd-lab/i7-fixture/paste.json"\n'));break
                    except lab.LabError:time.sleep(.2)
                expected='\n'.join(i['receipt']['directUrl'] for i in op['items']);assert paste.get('text')==expected,'native_public_link_paste_mismatch'
                save(phase/('verification-'+str(index)+'.json'),{'public':verified,'independentNativeUrlPaste':True})
                r['operations'].append(opid);save(phase/'receipt.json',r)
            r['diagnosticsAfter']=lab.cli(RUN,'diagnostics')
            lab.ssh(RUN,"python3 - <<'PY'\nfrom pathlib import Path\nimport json\n(Path.home()/'putgd-lab/stage.json').write_text(json.dumps({'stage':"+repr('idle-after-'+a.kind+'-'+phase.name)+"}))\nPY\n")
        elif a.phase=='cleanup-prepare':
            manifest=json.loads((ROOT/'baseline102.json').read_text());ids=manifest['historyIds'];assert ids
            for ident in ids:
                item=lab.cli(RUN,'history.detail',{'id':ident});save(phase/(ident+'.json'),item)
            r['ids']=ids;r['prepared']=lab.cli(RUN,'delete.prepare',{'ids':ids});save(ROOT/'cleanup-prepared.json',r)
        elif a.phase=='cleanup-confirm':
            p=json.loads((ROOT/'cleanup-prepared.json').read_text());assert p.get('reviewedExactRunOwnedEffects') is True
            r['ids']=p['ids'];r['delete']=lab.cli(RUN,'delete.confirm',{'ids':p['ids'],'acknowledgement':p['prepared']['acknowledgement']})
            save(ROOT/'cleanup-started.json',r)
        r['passed']=True
    except Exception as e:
        # Fixed exception types/codes only; never echo raw credential input or HTTP bodies.
        r['errorType']=type(e).__name__;r['errorCode']=str(e) if isinstance(e,AssertionError) else 'phase_failed'
        raise
    finally:
        save(phase/'receipt.json',r);print(json.dumps({'phase':a.phase,'passed':r['passed'],'receipt':str(phase/'receipt.json'),'errorType':r.get('errorType')}))

if __name__=='__main__':
    try:main()
    except Exception:raise SystemExit(1)
