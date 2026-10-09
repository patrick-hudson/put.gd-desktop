#!/usr/bin/env python3
"""Explicit visible-console actions with append-only replay parameters and witnesses."""
from pathlib import Path
from PIL import Image
import sys,json,time,datetime
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';dest=out/'ui';dest.mkdir(exist_ok=True)
action,label,*args=sys.argv[1:];record={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'action':action,'label':label,'args':args,'runnerSha256':lab.hash_file(Path(lab.__file__))}
path=dest/(label+'.png');assert not path.exists(),'Preserve previous witness'
try:
 if action=='click':
  x,y=map(int,args);lab.pointer(run,x,y,1280,800,True);lab.pointer(run,x,y,1280,800,False)
 elif action=='key':lab.key(run,args)
 elif action=='type':
  assert len(args)==1 and all(c in 'abcdefghijklmnopqrstuvwxyz0123456789/-.' for c in args[0]),'Only fixed synthetic lowercase text'
  for char in args[0]:lab.key(run,[{'/':'slash','-':'minus','.':'dot'}.get(char,char)])
 elif action=='show':record['show']=lab.cli(run,'ui.show',{'view':args[0]})
 elif action=='patch':
  settings=lab.cli(run,'settings.get');record['patch']=lab.cli(run,'settings.patch',{'expectedRevision':settings['metadata']['observedRevision'],'patch':json.loads(args[0])})
 elif action=='witness':pass
 else:raise ValueError('Unknown explicit console action')
 time.sleep(.7)
 frame=lab.screenshot(run,'u11-gnome-'+label);Image.open(frame['path']).save(path)
 record['witness']={'path':str(path),'sha256':lab.hash_file(path)}
 record['resident']=lab.verify_running_app(run);snap=lab.cli(run,'ui.snapshot');record['ui']=snap['diagnostics']['ui'];record['trayAvailable']=snap['diagnostics']['trayAvailable'];record['settingsRevision']=snap['settings']['metadata']['acceptedRevision']
except BaseException as e:record['errorType']=type(e).__name__;record['error']=str(e);raise
finally:
 with (dest/'actions.jsonl').open('a') as f:f.write(json.dumps(record)+'\n')
 print(json.dumps(record))
