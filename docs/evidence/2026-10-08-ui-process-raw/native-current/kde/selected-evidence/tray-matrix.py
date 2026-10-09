#!/usr/bin/env python3
"""Use existing native fixture/oracles, replacing only action hotkeys with visible tray clicks."""
import argparse,json,time
from PIL import Image
from common import ROOT,OUT,RUN,now,save,sha,source,load_lab,assert_resident,ui
lab=load_lab();expected=assert_resident(lab)
menu=json.loads((OUT/'tray-menu-map.json').read_text());assert sha(menu['witness'])==menu['witnessSha256'];assert menu['viewport']==[1280,800]
output=OUT/'tray-matrix.json';assert not output.exists();receipt={'schemaVersion':1,'startedUtc':now(),'sourceCommit':source()['commit'],'runningApp':expected,'runnerSha256':sha(ROOT/'tools/lab/lab.py'),'menuMap':menu,'vncPort':5934,'attempts':[],'ok':False}
original_key=lab.key;hotkeys=lab.load_profile('kde-wayland')['fixture_hotkeys']
try:
 for action,scenario in [('region','region'),('window','window'),('desktop','desktop'),('clipboard','clipboard-text')]:
  assert ui(lab)['pid']==0
  triggers=[]
  def trigger(run,keys):
   if list(keys)!=hotkeys[action]['keys']:return original_key(run,keys)
   assert not triggers,'One fixture action must invoke exactly one tray action'
   lab.pointer(run,*menu['tray'],1280,800)
   lab.input_events(run,[{'type':'btn','data':{'down':True,'button':'right'}}]);time.sleep(.05);lab.input_events(run,[{'type':'btn','data':{'down':False,'button':'right'}}]);time.sleep(.35)
   witness=lab.screenshot(run,'u11-tray-'+action+'-menu');png=OUT/('tray-'+action+'-menu.png');Image.open(witness['path']).save(png)
   target=menu['actions'][action];lab.pointer(run,*target,1280,800,True);time.sleep(.05);lab.pointer(run,*target,1280,800,False)
   triggers.append({'timestampUtc':now(),'originalHotkeyReplaced':list(keys),'trayRightClick':menu['tray'],'rowClick':target,'menuWitness':witness,'png':str(png),'ui':ui(lab)})
  lab.key=trigger
  print('TRAY',action,flush=True)
  result=lab.fixture_run(argparse.Namespace(run=str(RUN),scenario=scenario,settings=None,history=False,clipboard_client='qt',timeout=90,keep=True));result['actualTrigger']='visible QMP tray context-menu click';result['triggers']=triggers;result['uiAfter']=ui(lab)
  receipt['attempts'].append(result);save(output,receipt)
  assert len(triggers)==1 and result.get('ok'),'Tray fixture failed; all evidence retained without retry'
  assert result['uiAfter']['pid']==0 and assert_resident(lab)==expected
 receipt['ok']=True
except Exception as error:receipt['error']=str(error);raise
finally:lab.key=original_key;receipt['finishedUtc']=now();save(output,receipt)
print(json.dumps({'ok':True,'cases':len(receipt['attempts'])}))
