"""Visible production tray acceptance against the offline S3 wire oracle."""
import sys,json,time,subprocess,copy,threading,traceback
from pathlib import Path
from PIL import Image
SRC=Path('/home/patrick/work/put.gd/work/i33');ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(SRC/'tools/lab'));import lab
run=Path(sys.argv[1]).resolve();out=ROOT/run.name/('visual'+(sys.argv[2] if len(sys.argv)>2 else ''));out.mkdir(parents=True,mode=0o700,exist_ok=False)
if lab.state(run)['profile']=='gnome-wayland':
 old=lab.guest_session
 def session(r,body):return old(r,'test -S /run/user/1000/wayland-0\nexport WAYLAND_DISPLAY=wayland-0\n'+body)
 lab.guest_session=session
COLORS={'blue':(155,188,255),'green':(98,214,155),'red':(255,180,171)}
rows=[];box=None;counter=0;phase='idle';stop=False;failures=[]
def save(n,v):(out/n).write_text(json.dumps(v,indent=2)+'\n')
def cli(m,p=None):return lab.cli(run,m,p)
def guest(code):return lab.ssh(run,"python3 - <<'INNER'\n"+code+"\nINNER\n")
def shot(label):
 p=out/(label+'.ppm')
 with lab.QMP(run/'qmp.sock') as q:q.execute('screendump',{'filename':str(p)})
 im=Image.open(p).convert('RGB');im.save(p.with_suffix('.png'));return im
im=shot('idle')
points=[(x,y) for y in list(range(45))+list(range(im.height-45,im.height)) for x in range(im.width) if im.getpixel((x,y))==COLORS['blue']]
assert len(points)>5,'rendered_blue_icon_not_found'
box=(min(x for x,y in points)-3,min(y for x,y in points)-3,max(x for x,y in points)+4,max(y for x,y in points)+4)
assert box[2]-box[0]<40 and box[3]-box[1]<40,'ambiguous_tray_region'
save('region.json',{'box':box,'resident':lab.verify_running_app(run),'build':cli('diagnostics')['build'],'colors':COLORS})
def observer():
 global counter
 last=None
 try:
  while not stop:
   t=time.monotonic();p=out/'frame.ppm'
   with lab.QMP(run/'qmp.sock') as q:q.execute('screendump',{'filename':str(p)})
   im=Image.open(p).convert('RGB');crop=im.crop(box);pixels=list(crop.getdata());counts={k:sum(1 for c in pixels if c==rgb) for k,rgb in COLORS.items()};color=max(counts,key=counts.get) if max(counts.values())>5 else 'unknown'
   row={'time':t,'phase':phase,'color':color,'counts':counts};rows.append(row)
   if color!=last:
    label=f'transition-{counter:03d}-{phase}-{color}';counter+=1;im.save(out/(label+'.png'));row['witness']=label+'.png';last=color
   with (out/'samples.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
   time.sleep(max(0,.12-(time.monotonic()-t)))
 except BaseException as e:failures.append(type(e).__name__)
def control(value):return guest('import json,urllib.request\nr=urllib.request.Request('+repr(endpoint+'/__reset')+',data='+repr(json.dumps(value).encode())+',headers={"Content-Type":"application/json"})\nprint(urllib.request.urlopen(r).read().decode())')
def terminal(ident):
 for _ in range(160):
  op=cli('operation.inspect',{'id':ident})
  if op['state'] in ('completed','partial','failed','interrupted','cancelled'):return op
  time.sleep(.1)
 raise RuntimeError('operation_timeout')
def invoke():return cli('action.invoke',{'action':'clipboard'})['operationId']
def wait_color(color,timeout=4):
 deadline=time.monotonic()+timeout
 while time.monotonic()<deadline:
  if rows and rows[-1]['color']==color:return
  time.sleep(.08)
 raise RuntimeError('rendered_'+color+'_missing')
def focus():
 im=shot('fixture-focus');p=out/'fixture-focus.ppm';g=lab.locate_fixture_paint(p);assert g
 lab.pointer(run,g['x']+35,g['y']+35,im.width,im.height,True);lab.pointer(run,g['x']+35,g['y']+35,im.width,im.height,False)
 lab.key(run,['ctrl','alt','shift','t']);time.sleep(.3)
# Only synthetic source and credentials; no user credential file is read.
lab.put(run,str(SRC/'tests/fixtures/s3_server.py'),'/tmp/i33-s3.py')
lab.ssh(run,'nohup python3 /tmp/i33-s3.py > /tmp/i33-s3-port 2>/tmp/i33-s3-private.log < /dev/null &\n')
for _ in range(50):
 port=lab.ssh(run,'head -1 /tmp/i33-s3-port\n').strip()
 if port:break
 time.sleep(.1)
endpoint='http://127.0.0.1:'+port
original=cli('settings.get');doc=copy.deepcopy(original['document']);p=cli('settings.developmentTemplate')['document']['profiles'][0];p.update(id='issue33-offline',label='Issue 33 synthetic wire fixture',credentials={'mode':'session','reference':None});p['settings'].update(endpoint=endpoint,bucket='isolated-fixture',region='eu-west-3',objectPrefix='',aclMode='omit',publicUrlBase=None,connectTimeoutMs=500,requestTimeoutMs=9000,lowSpeedTimeSeconds=10)
doc['profiles']=[p];doc['actions']['clipboard'].update(destination='upload',profileId=p['id']);doc['naming'].update(mode='template',textTemplate='i33-{uuid}.{ext}');doc['limits']['concurrentUploads']=2;doc['preferences']['notifications']=False
assert cli('settings.validate',{'document':doc})['valid'];cli('settings.patch',{'patch':doc,'expectedRevision':original['metadata']['acceptedRevision']})
params={'profileId':p['id'],'expectedRevision':cli('settings.get')['metadata']['acceptedRevision'],'credentials':{'accessKey':'PUTGD_SYNTHETIC_ACCESS','secretKey':'putgd-synthetic-secret-not-a-real-key'}}
code='''import json,socket,uuid
S='/run/user/1000/putgd/session.sock'
with socket.socket(socket.AF_UNIX) as s:
 s.connect(S);s.sendall(json.dumps(dict(schemaVersion=1,requestId=str(uuid.uuid4()),session=S,method='setup.storeCredentials',params=PARAMS)).encode()+b'\\n');b=b''
 while not b.endswith(b'\\n'):b+=s.recv(65536)
 v=json.loads(b);assert v['ok'];print(json.dumps(v['result']))
'''.replace('PARAMS',repr(params));job=json.loads(guest(code));params.clear()
for _ in range(30):
 j=cli('setup.inspect',{'id':job['setupId']})
 if j['state']!='running':break
 time.sleep(.1)
assert j['state']=='completed';focus();t=threading.Thread(target=observer);t.start()
results={'operations':[],'checks':{},'scope':'rendered single-head native tray, production app and official SDK; offline S3 wire oracle is not cloud acceptance'}
try:
 wait_color('blue');time.sleep(.5)
 # Selection must remain blue, cancellation must not start an error pulse.
 phase='selecting';select=cli('action.invoke',{'action':'region'})['operationId'];time.sleep(.8);assert rows[-1]['color']=='blue';lab.key(run,['esc']);op=terminal(select);assert op['state']=='cancelled';time.sleep(.7);assert rows[-1]['color']=='blue';results['checks']['selectionCancellationBlue']=True;results['operations'].append(op)
 # HEAD preflight and PUT are both real SDK requests, held by the wire oracle.
 phase='upload-success';control({'rules':[{'method':'HEAD','delay':1.4},{'method':'PUT','delay':2}]});ident=invoke();wait_color('green');op=terminal(ident);assert op['state']=='completed';wait_color('blue');results['operations'].append(op);results['checks']['preflightUploadGreenThenBlue']=True
 # A terminal 403 generates a visible timed red/blue pulse.
 phase='error';control({'rules':[{'method':'HEAD','delay':.4},{'method':'PUT','status':403}]});ident=invoke();op=terminal(ident);assert op['state'] in ('failed','partial');results['operations'].append(op);wait_color('red');time.sleep(3.6);wait_color('blue');rr=[r for r in rows if r['phase']=='error'];red=[r for r in rr if r['color']=='red'];assert len(red)>=2 and any(r['color']=='blue' for r in rr if red[0]['time']<r['time']<red[-1]['time']);assert red[-1]['time']-red[0]['time']<3.4;results['checks']['temporaryRedBluePulse']=True
 # Overlap: one upload fails while another PUT is still in flight. Red must
 # recover to green until that remaining request completes.
 focus();phase='overlap';control({'rules':[{'method':'HEAD','delay':.5,'remaining':2},{'method':'PUT','status':403,'delay':.9},{'method':'PUT','delay':6.5}]});ids=[invoke(),invoke()];wait_color('red');time.sleep(3.4);wait_color('green',1.5);ops=[terminal(i) for i in ids];assert sum(o['state']=='completed' for o in ops)==1 and sum(o['state'] in ('failed','partial') for o in ops)==1;wait_color('blue');results['operations']+=ops;results['checks']['overlapRecoversGreen']=True
 # Local archive failure is also visible although no storage request runs.
 phase='local-error';cfg=cli('settings.get');local=copy.deepcopy(cfg['document']);local['actions']['clipboard'].update(destination='local',profileId=None);local['archive']['root']='/proc/i33-unwritable';cli('settings.patch',{'patch':local,'expectedRevision':cfg['metadata']['acceptedRevision']});focus();op=terminal(invoke());assert op['state']=='failed';results['operations'].append(op);wait_color('red');time.sleep(3.6);wait_color('blue');results['checks']['localFailurePulse']=True
 results['passed']=True
finally:
 stop=True;t.join();results['observerErrors']=failures;results['samples']=len(rows);results['box']=box;save('results.json',results)
 cfg=cli('settings.get');cli('settings.patch',{'patch':original['document'],'expectedRevision':cfg['metadata']['acceptedRevision']})
assert not failures
print(json.dumps({'passed':True,'checks':results['checks'],'samples':len(rows),'output':str(out)}))
