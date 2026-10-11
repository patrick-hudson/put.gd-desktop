#!/usr/bin/env python3
"""One real bounded local-file operation across visible helper close, no retry."""
import json,shlex,subprocess,time
from pathlib import Path
from PIL import Image
from common import ROOT,OUT,RUN,now,save,sha,load_lab,assert_resident,ui,wait_ui,helper_identity
lab=load_lab();directory=OUT/'continuity-run1';directory.mkdir(mode=0o700,exist_ok=False)
receipt={'startedUtc':now(),'verdict':'incomplete','scope':'512 MiB synthetic file plus small second file; actual Wayland clipboard/local archive, no S3 or injected delay'}
def witness(name):
 w=lab.screenshot(RUN,'u11-'+name);Image.open(w['path']).save(directory/(name+'.png'));return w
try:
 resident=assert_resident(lab);assert ui(lab)['pid']==0
 settings=lab.cli(RUN,'settings.get')['document'];limits=settings['limits']
 assert settings['actions']['clipboard']['destination']=='local'
 assert settings['archive']['root']=='/home/lab/putgd-lab/archive'
 size=512*1024*1024;assert min(limits['maxFileBytes'],limits['maxBatchBytes'],limits['pendingArtifactBytes'])>size+100
 lab.guest_session(RUN,'''set -eu
pkill -x putgd-desktop-f || true
pkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true
rm -f "$HOME/putgd-lab/fixture/ready.json"
nohup putgd-desktop-fixture --output "$HOME/putgd-lab/fixture" > "$HOME/putgd-lab/fixture.log" 2>&1 < /dev/null &
''')
 deadline=time.monotonic()+15
 while lab.guest_session(RUN,'if test -s "$HOME/putgd-lab/fixture/ready.json"; then echo ready; fi\n').strip()!='ready':
  assert time.monotonic()<deadline;time.sleep(.2)
 preparation=(ROOT/'work/u11/native/cinnamon/continuity-run1/prepare.sh').read_text()
 (directory/'prepare.sh').write_text(preparation)
 payload=json.loads(lab.guest_session(RUN,preparation))
 frame=witness('fixture-ready');geometry=lab.locate_fixture_paint(frame['path']);assert geometry
 x,y=geometry['x']+30,geometry['y']+30
 lab.pointer(RUN,x,y,1280,800,True);time.sleep(.05);lab.pointer(RUN,x,y,1280,800,False)
 lab.key(RUN,['ctrl','alt','shift','f']);time.sleep(.2)
 subprocess.run(['python3',str(OUT/'open-tray-view.py'),'--view','activity','--name','tray-activity-continuity'],check=True)
 state=wait_ui(lab,'ready');helper=helper_identity(lab,state['pid'])
 before={o['id'] for o in lab.cli(RUN,'operation.recent')['operations']}
 parameters={'schemaVersion':1,'timestampUtc':now(),'payload':payload,'limits':limits,'archive':settings['archive'],'resident':resident,'helperPid':state['pid'],'helperIdentity':helper,'operationIdsBefore':sorted(before),'fixtureGeometry':geometry,'hotkey':['ctrl','shift','f12'],'closeKeys':['alt','f4'],'scope':receipt['scope']}
 save(directory/'parameters.json',parameters)
 observer=(ROOT/'work/u11/native/cinnamon/continuity-run1/observer.py').read_text()
 (directory/'observer.py').write_text(observer)
 lab.guest_session(RUN,'mkdir -p "$HOME/putgd-lab/u11-continuity"\n')
 for name in ['parameters.json','observer.py']:lab.put(RUN,directory/name,'/home/lab/putgd-lab/u11-continuity/'+name)
 lab.guest_session(RUN,'nohup python3 "$HOME/putgd-lab/u11-continuity/observer.py" > "$HOME/putgd-lab/u11-continuity/observer.log" 2>&1 < /dev/null &\n')
 witness('before-trigger');save(directory/'trigger.json',{'timestampUtc':now(),'keys':['ctrl','shift','f12'],'thenKeys':['alt','f4'],'helperIdentity':helper})
 lab.key(RUN,['ctrl','shift','f12']);lab.key(RUN,['alt','f4'])
 after=wait_ui(lab,'stopped');assert after['pid']==0 and after['lastExit']['status']=='normal' and after['lastExit']['code']==0
 witness('after-close')
 deadline=time.monotonic()+65
 while True:
  if lab.guest_session(RUN,'if test -s "$HOME/putgd-lab/u11-continuity/result.json"; then echo ready; fi\n').strip()=='ready':break
  assert time.monotonic()<deadline;time.sleep(.2)
 lab.get(RUN,'/home/lab/putgd-lab/u11-continuity/result.json',directory/'observer-result.json')
 lab.get(RUN,'/home/lab/putgd-lab/u11-continuity/observations.jsonl',directory/'observations.jsonl')
 result=json.loads((directory/'observer-result.json').read_text());op=result['operation'];assert op and result['failure'] is None
 assert op['state']=='completed' and all(i['local']['status']=='saved' for i in op['items'])
 paths=[i['local']['path'] for i in op['items']];assert len(paths)==2 and all(p.startswith('/home/lab/putgd-lab/archive/') for p in paths)
 hashes=lab.ssh(RUN,'sha256sum '+' '.join(shlex.quote(p) for p in paths)+'\n')
 sizes=lab.ssh(RUN,"stat -c '%s' "+' '.join(shlex.quote(p) for p in paths)+'\n')
 assert hashes.splitlines()[0].split()[0]==payload['sha256'] and int(sizes.splitlines()[0])==size
 assert assert_resident(lab)==resident and ui(lab)['pid']==0
 receipt.update(result,normalExit=after['lastExit'],residentUnchanged=resident,artifactSha256Output=hashes,artifactSizes=sizes,verdict='pass' if result['activeOperationObservedAfterHelperExit'] else 'inconclusive-fast-operation')
 print(json.dumps({'verdict':receipt['verdict'],'operationId':op['id'],'bytes':size}),flush=True)
except Exception as error:receipt['error']=str(error);raise
finally:receipt['finishedUtc']=now();save(directory/'receipt.json',receipt)
