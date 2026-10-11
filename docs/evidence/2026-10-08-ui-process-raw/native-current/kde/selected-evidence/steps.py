#!/usr/bin/env python3
"""Drive visible QMP UI steps; preserve a fresh witness and before/after state for each."""
import json,sys,time
from PIL import Image
from common import OUT,RUN,now,save,load_lab,assert_resident,ui,helper_identity
lab=load_lab();expected=assert_resident(lab)
for step in json.loads(sys.argv[1]):
 name=step['label'];kind=step['action'];assert name.replace('-','').isalnum()
 png=OUT/('native-'+name+'.png');assert not png.exists(),'Preserve all attempts; choose a new label'
 before=ui(lab);started=time.monotonic();row={'timestampUtc':now(),'step':step,'uiBefore':before}
 if kind=='show':row['response']=lab.cli(RUN,'ui.show',{'view':step.get('view','settings')})
 elif kind=='key':lab.key(RUN,step['keys'])
 elif kind=='click':
  lab.pointer(RUN,step['x'],step['y'],1280,800,True);time.sleep(.05);lab.pointer(RUN,step['x'],step['y'],1280,800,False)
 elif kind=='right-click':
  lab.pointer(RUN,step['x'],step['y'],1280,800);lab.input_events(RUN,[{'type':'btn','data':{'down':True,'button':'right'}}]);time.sleep(.05);lab.input_events(RUN,[{'type':'btn','data':{'down':False,'button':'right'}}])
 elif kind=='text':
  codes={'/':'slash','-':'minus','.':'dot',' ':'spc'}
  for char in step['text']:
   assert char in 'abcdefghijklmnopqrstuvwxyz0123456789/-. '
   lab.key(RUN,[codes.get(char,char)])
 elif kind=='wheel':
  lab.pointer(RUN,step['x'],step['y'],1280,800)
  for _ in range(step.get('count',3)):
   lab.input_events(RUN,[{'type':'btn','data':{'down':True,'button':'wheel-down'}},{'type':'btn','data':{'down':False,'button':'wheel-down'}}]);time.sleep(.1)
 elif kind!='screenshot':raise ValueError(kind)
 time.sleep(step.get('settle',.7));w=lab.screenshot(RUN,'u11-'+name);Image.open(w['path']).save(png)
 after=ui(lab);row.update(witness=w,png=str(png),uiAfter=after,elapsedSeconds=time.monotonic()-started,residentUnchanged=assert_resident(lab)==expected)
 if after['pid']:row['helperIdentity']=helper_identity(lab,after['pid'])
 with (OUT/'ui-steps.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
 print(json.dumps(row),flush=True)
