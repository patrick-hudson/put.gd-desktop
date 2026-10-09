#!/usr/bin/env python3
"""Normal close/reopen on native GNOME, with whole helper-process lifetime checks."""
from pathlib import Path
from PIL import Image
import json,sys,time,datetime
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';dest=out/'views';dest.mkdir(exist_ok=False)
r={'schemaVersion':1,'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'sourceCommit':json.loads((out.parent/'source.json').read_text())['commit'],'scope':'Normal GNOME window-manager close/reopen for Settings, History, Activity and About; activation through production CLI. Actual tray clicks are separate evidence.','runnerSha256':lab.hash_file(Path(lab.__file__)),'scriptSha256':lab.hash_file(__file__),'cases':[],'passed':False}
def save():lab.write_json(dest/'results.json',r)
def snapshot():return lab.cli(run,'ui.snapshot')['diagnostics']
def witness(label):
 w=lab.screenshot(run,'u11-gnome-'+label);p=dest/(label+'.png');Image.open(w['path']).save(p);return {'path':str(p),'sha256':lab.hash_file(p)}
def helper_identity(pid):
 code="""from pathlib import Path
import hashlib,json
p=Path('/proc')/str(PID)
print(json.dumps({'pid':PID,'startTicks':int((p/'stat').read_text().rsplit(')',1)[1].split()[19]),'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest(),'comm':(p/'comm').read_text().strip()}))
""".replace('PID',str(pid))
 import subprocess
 result=subprocess.run(lab.ssh_args(run)+['python3','-'],input=code,text=True,capture_output=True,timeout=10);result.check_returncode();return json.loads(result.stdout)
try:
 initial=lab.verify_running_app(run);r['resident']=initial;d=snapshot();assert d['trayAvailable'] and not d['ui']['pid'],d
 for cycle in range(1,3):
  for view in ['settings','history','activity','about']:
   started=time.monotonic();reply=lab.cli(run,'ui.show',{'view':view});d=snapshot();assert d['trayAvailable'] and d['ui']['state']=='ready' and d['ui']['pid']>0,d
   identity=helper_identity(d['ui']['pid']);assert identity['comm']=='putgd-ui' and identity['sha256']==json.loads((out/'installed.json').read_text())['binarySha256']['/usr/bin/putgd-ui']
   time.sleep(.7);opened=witness(f'{cycle}-{view}-open');lab.key(run,['alt','f4']);deadline=time.monotonic()+12
   while True:
    d=snapshot()
    if d['ui']['pid']==0:break
    if time.monotonic()>deadline:raise RuntimeError('Normal close did not release helper')
    time.sleep(.15)
   assert d['trayAvailable'] and d['ui']['state']=='stopped' and d['ui']['lastExit']['status']=='normal' and d['ui']['lastExit']['code']==0,d
   assert lab.ssh(run,'test ! -e /proc/'+str(identity['pid'])+' && echo absent\n').strip()=='absent'
   assert lab.verify_running_app(run)==initial
   r['cases'].append({'cycle':cycle,'view':view,'show':reply,'helper':identity,'afterClose':d['ui'],'openedWitness':opened,'closedWitness':witness(f'{cycle}-{view}-closed'),'seconds':time.monotonic()-started});save()
 r['passed']=True;r['visualReview']='pending';r['finishedUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save();print(json.dumps({'cases':len(r['cases']),'proceduralPassed':True,'visualReview':'pending'}))
except BaseException as e:r['errorType']=type(e).__name__;r['error']=str(e);save();raise
