from pathlib import Path
from PIL import Image
import sys,json,time,datetime
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';dest=out/'tray-actions';dest.mkdir(exist_ok=False)
r={'schemaVersion':1,'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Actual GNOME tray menu invokes all four production local-only actions with no UI helper','cases':[],'passed':False}
def save():lab.write_json(dest/'results.json',r)
def click(x,y):lab.pointer(run,x,y,1280,800,True);lab.pointer(run,x,y,1280,800,False)
def witness(label):
 w=lab.screenshot(run,'u11-gnome-tray-'+label);p=dest/(label+'.png');Image.open(w['path']).save(p);return {'path':str(p),'sha256':lab.hash_file(p)}
try:
 lab.key(run,['esc'])
 initial=lab.verify_running_app(run);r['resident']=initial
 for action,y in [('region',62),('window',99),('desktop',135),('clipboard',170)]:
  before={x['id'] for x in lab.cli(run,'operation.recent')['operations']};d=lab.cli(run,'ui.snapshot')['diagnostics'];assert d['ui']['pid']==0 and d['trayAvailable']
  click(450,300)
  if action=='clipboard':lab.key(run,['ctrl','alt','shift','t']);time.sleep(.3)
  click(1158,16);time.sleep(.2);menu=witness(action+'-menu');click(1130,y)
  if action=='region':
   time.sleep(.7);lab.pointer(run,383,273,1280,800,True);lab.pointer(run,463,333,1280,800);lab.pointer(run,463,333,1280,800,False)
  deadline=time.monotonic()+30;op=None
  while time.monotonic()<deadline:
   added=[x for x in lab.cli(run,'operation.recent')['operations'] if x['id'] not in before];assert len(added)<=1
   if added:
    op=lab.cli(run,'operation.inspect',{'id':added[0]['id']})
    if op['state'] in ['completed','failed','cancelled','partial'] and op.get('copyStatus') in ['copied','failed','suppressed','no_result']:break
   time.sleep(.2)
  entry={'action':action,'menuWitness':menu,'operationId':op['id'] if op else None,'operationState':op['state'] if op else None,'copyStatus':op.get('copyStatus') if op else None};r['cases'].append(entry);save()
  assert op and op['state']=='completed' and op['copyStatus']=='copied',entry
  artifact=dest/(action+'.artifact');lab.get(run,op['items'][0]['local']['path'],artifact);assert lab.hash_file(artifact)==op['items'][0]['artifact']['sha256'];entry['artifactSha256']=lab.hash_file(artifact)
  if action=='clipboard':assert artifact.read_bytes()==b'putgd synthetic clipboard text'
  else:
   im=Image.open(artifact);entry['artifactDimensions']=list(im.size)
   if action=='region':assert im.size==(80,60) and im.convert('RGB').getpixel((20,20))==(220,30,40)
   if action=='desktop':assert im.size==(1280,800)
  d=lab.cli(run,'ui.snapshot')['diagnostics'];assert not d['ui']['pid'] and d['trayAvailable'];assert lab.verify_running_app(run)==initial;entry['uiAfter']=d['ui'];entry['afterWitness']=witness(action+'-completed');save()
 r['passed']=True;save();print(json.dumps({'passed':True,'cases':len(r['cases'])}))
except BaseException as e:r['errorType']=type(e).__name__;r['error']=str(e);save();raise
