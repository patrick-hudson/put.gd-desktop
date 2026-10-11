#!/usr/bin/env python3
"""Synthetic GNOME live-selector witness using the existing packaged native fixture."""
from pathlib import Path
from PIL import Image
import sys,json,time,datetime,shlex
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';out=Path(__file__).resolve().parent/'live-region';out.mkdir(exist_ok=False)
remote='/home/lab/putgd-lab/live-region';started=time.monotonic()
result={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Synthetic GNOME live background before mouse-down plus outline-free saved80x60 capture; one extra local-only region action; no memory measurement','scriptSha256':lab.hash_file(__file__),'runnerSha256':lab.hash_file(root/'tools/lab/lab.py'),'passed':False,'events':[],'parameters':{'initialRgb':[12,87,160],'updatedRgb':[173,48,109],'width':80,'height':60,'fixturePollMs':50,'visibleWaitTimeoutSeconds':10,'operationTimeoutSeconds':15}}
def save():lab.write_json(out/'results.json',result)
def event(name,**more):result['events'].append({'event':name,'elapsedSeconds':time.monotonic()-started,**more});save()
def witness(label):
 item=lab.screenshot(run,label);png=out/(label+'.png');Image.open(item['path']).save(png);return {'path':str(png),'sha256':lab.hash_file(png)}
def guestjson(name):return json.loads(lab.ssh(run,'cat '+shlex.quote(remote+'/'+name)+'\n'))
def setcolor(revision,color):
 doc={'revision':revision,'rgb':color};lab.ssh(run,'printf %s '+shlex.quote(json.dumps(doc))+' > '+shlex.quote(remote+'/color.json')+'\n');event('color-control-written',revision=revision,rgb=color)
 deadline=time.monotonic()+10
 while time.monotonic()<deadline:
  try:
   ack=guestjson('live-painted.json')
   if ack=={'schemaVersion':1,**doc}:event('fixture-painted',revision=revision);return
  except Exception:pass
  time.sleep(.1)
 raise RuntimeError('Fixture did not acknowledge color')
def visible(label,color,point):
 deadline=time.monotonic()+10
 while True:
  item=witness(label);image=Image.open(item['path']).convert('RGB');actual=list(image.getpixel(point))
  if actual==color:event(label,witness=item,sample={'x':point[0],'y':point[1],'expected':color,'actual':actual});return
  if time.monotonic()>deadline:raise RuntimeError('Independent QMP witness did not show live color')
  time.sleep(.1)
try:
 result['runningAppBefore']=lab.verify_running_app(run)
 lab.guest_session(run,"pkill -x putgd-desktop-f || true\npkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true\nmkdir -p "+remote+"\nnohup putgd-desktop-fixture --output "+remote+" --live-color-file "+remote+"/color.json > "+remote+"/fixture.log 2>&1 < /dev/null &\n")
 deadline=time.monotonic()+12
 while True:
  try:ready=guestjson('ready.json');break
  except Exception:
   if time.monotonic()>deadline:raise
   time.sleep(.2)
 assert ready['session']=='wayland' and ready['qt_platform']=='wayland'
 result['fixtureReady']=ready;screen=ready['display_layout'][0];width,height=screen['width'],screen['height']
 geometry=None
 for attempt in range(3):
  time.sleep(.3);initial=lab.screenshot(run,'live-fixture-locate');geometry=lab.locate_fixture_paint(initial['path'])
  if geometry:break
  lab.key(run,['alt','tab'])
 if not geometry:raise RuntimeError('Independent four-color fixture not visibly located')
 result['independentFixtureGeometry']=geometry
 x,y=geometry['x']+30,geometry['y']+30;point=(geometry['x']+150,geometry['y']+100)
 lab.pointer(run,x,y,width,height,True);lab.pointer(run,x,y,width,height,False)
 before={v['id'] for v in lab.cli(run,'operation.recent')['operations']};result['operationCountBefore']=len(before)
 a=result['parameters']['initialRgb'];b=result['parameters']['updatedRgb'];setcolor('A',a);visible('before-selector',a,point)
 hotkey=lab.load_profile(lab.state(run)['profile']).get('fixture_hotkeys',{'region':{'keys':['ctrl','shift','1']}})['region']['keys'];result['regionHotkey']=hotkey
 lab.key(run,hotkey);event('selector-hotkey-sent');time.sleep(.5);visible('selector-before-update',a,point)
 setcolor('B',b);visible('selector-live-updated-before-drawing',b,point)
 lab.pointer(run,x,y,width,height,True);lab.pointer(run,x+80,y+60,width,height);event('selection-drawn',start={'x':x,'y':y},end={'x':x+80,'y':y+60});time.sleep(.1)
 result['outlineWitness']=witness('selector-outline');lab.pointer(run,x+80,y+60,width,height,False);event('selection-released')
 deadline=time.monotonic()+15;operation=None
 while time.monotonic()<deadline:
  recent=lab.cli(run,'operation.recent')['operations'];found=[row for row in recent if row['id'] not in before]
  if len(found)>1:raise RuntimeError('Gesture created multiple operations')
  if found:
   operation=lab.cli(run,'operation.inspect',{'id':found[0]['id']})
   if operation['state'] in ['completed','failed','cancelled','partial'] and operation.get('copyStatus') in ['copied','failed','suppressed','no_result']:break
  time.sleep(.2)
 if not operation:raise RuntimeError('No completed operation')
 result['operation']=operation;result['operationCountAfter']=len(lab.cli(run,'operation.recent')['operations'])
 assert operation['state']=='completed' and len(operation['items'])==1
 path=operation['items'][0]['local']['path'];assert path.startswith('/home/lab/putgd-lab/archive/') and '..' not in Path(path).parts
 artifact=out/'capture.png';lab.get(run,path,artifact);im=Image.open(artifact).convert('RGB');colors=im.getcolors(im.width*im.height)
 result['capturedImage']={'path':str(artifact),'sha256':lab.hash_file(artifact),'width':im.width,'height':im.height,'colorCounts':[{'pixels':count,'rgb':list(rgb)} for count,rgb in colors],'everyPixelMatchesUpdatedColor':colors==[(80*60,tuple(b))]}
 result['runningAppAfter']=lab.verify_running_app(run);result['afterCaptureWitness']=witness('after-capture')
 result['passed']=im.size==(80,60) and colors==[(4800,tuple(b))] and result['runningAppAfter']==result['runningAppBefore']
 event('finished',passed=result['passed']);print(json.dumps({'passed':result['passed'],'operationId':operation['id'],'capturedImage':result['capturedImage']}))
except BaseException as error:
 result['errorType']=type(error).__name__;result['error']=str(error);save();raise
finally:
 lab.key(run,['esc']);save()
