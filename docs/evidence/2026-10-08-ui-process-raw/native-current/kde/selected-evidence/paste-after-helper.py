#!/usr/bin/env python3
import json,time
from PIL import Image
from common import OUT,RUN,now,save,load_lab,assert_resident,ui
lab=load_lab();expected=assert_resident(lab);state=ui(lab);assert state['pid']==0 and state['lastExit']['code']==0
receipt={'schemaVersion':1,'startedUtc':now(),'resident':expected,'ui':state,'source':'Activity Copy result of final synthetic clipboard-text tray case, followed by Activity and About helper exits','ok':False}
try:
 w=lab.screenshot(RUN,'u11-paste-focus');geometry=lab.locate_fixture_paint(w['path']);assert geometry
 lab.guest_session(RUN,'rm -f "$HOME/putgd-lab/fixture/paste.json"\n')
 x,y=geometry['x']+30,geometry['y']+30;lab.pointer(RUN,x,y,1280,800,True);lab.pointer(RUN,x,y,1280,800,False);lab.key(RUN,['ctrl','v']);time.sleep(.4)
 lab.get(RUN,'/home/lab/putgd-lab/fixture/paste.json',OUT/'paste-after-helper.json');paste=json.loads((OUT/'paste-after-helper.json').read_text());comparison=lab.compare_paste(paste,{'text':'putgd synthetic clipboard text'});assert comparison['ok'];assert ui(lab)['pid']==0 and assert_resident(lab)==expected
 final=lab.screenshot(RUN,'u11-paste-after-helper');Image.open(final['path']).save(OUT/'paste-after-helper.png');receipt.update(ok=True,fixtureGeometry=geometry,keys=['ctrl','v'],comparison=comparison,witness=final,uiAfter=ui(lab))
except Exception as error:receipt['error']=str(error);raise
finally:receipt['finishedUtc']=now();save(OUT/'clipboard-survival.json',receipt)
print(json.dumps(receipt))
