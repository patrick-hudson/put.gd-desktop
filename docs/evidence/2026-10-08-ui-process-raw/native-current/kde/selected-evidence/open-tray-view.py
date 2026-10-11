#!/usr/bin/env python3
import argparse,json,time
from PIL import Image
from common import OUT,RUN,now,save,sha,load_lab,assert_resident,ui,wait_ui,helper_identity
p=argparse.ArgumentParser();p.add_argument('--view',choices=['settings','history','activity','about'],required=True);p.add_argument('--name',required=True);a=p.parse_args();assert a.name.replace('-','').isalnum();output=OUT/(a.name+'.json');assert not output.exists()
lab=load_lab();expected=assert_resident(lab);menu=json.loads((OUT/'tray-menu-map.json').read_text());assert sha(menu['witness'])==menu['witnessSha256'];before=ui(lab);assert before['pid']==0
receipt={'schemaVersion':1,'startedUtc':now(),'view':a.view,'uiBefore':before,'resident':expected,'trigger':'visible tray context-menu click','ok':False};started=time.monotonic()
try:
 lab.pointer(RUN,*menu['tray'],1280,800);lab.input_events(RUN,[{'type':'btn','data':{'down':True,'button':'right'}}]);time.sleep(.05);lab.input_events(RUN,[{'type':'btn','data':{'down':False,'button':'right'}}]);time.sleep(.35)
 row=menu['views'][a.view];lab.pointer(RUN,*row,1280,800,True);time.sleep(.05);lab.pointer(RUN,*row,1280,800,False);state=wait_ui(lab,'ready');identity=helper_identity(lab,state['pid']);time.sleep(.6);w=lab.screenshot(RUN,'u11-'+a.name);png=OUT/(a.name+'.png');Image.open(w['path']).save(png)
 receipt.update(tray=menu['tray'],row=row,uiAfter=state,helperIdentity=identity,witness=w,png=str(png),elapsedRequestToWitnessSeconds=time.monotonic()-started,latencyScope='Includes QMP/SSH and explicit settle waits; not a render-latency benchmark',ok=True)
 assert assert_resident(lab)==expected
except Exception as error:receipt['error']=str(error);raise
finally:receipt['finishedUtc']=now();save(output,receipt)
print(json.dumps(receipt))
