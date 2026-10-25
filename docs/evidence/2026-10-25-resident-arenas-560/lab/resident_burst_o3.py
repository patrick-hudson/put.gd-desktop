#!/usr/bin/env python3
"""Private #560 headless attribution harness o3 (revision of o2), NOT physical/frozen acceptance.
Revision r3: nonzero exit for abnormal resident exit, forced cleanup or leftovers (r2: self-test PUT no longer delayed). I560_MODE=cancel3 replaces cancel2's filename inventory, which could not see renamed editor exports, with payload
attribution: every stored object and every PUT body is decoded and matched by pixel SHA-256 to its source.
The earlier o2 description follows.
Roots are explicit: I560_SRC (source tree) and I560_HARNESS (this script directory), both validated.
I560_MODE=cancel2: queued targets are cancelled first, each after a fresh witness, while transfers are held.
Adds: per-call latency record, optional per-process THP disable (I560_THP_OFF), post-score smaps.
Borrows the frozen source/pixel/receipt verification functions unchanged.
No GUI/tray/paint/quiet-desktop claim. 50 original PNGs, 10s PUT; explicit diagnostic settle duration.
"""
import hashlib, json, os, socket, subprocess, sys, time, uuid, types
from pathlib import Path
def _root(name, marker):
 p = Path(os.environ.get(name, ''))
 if not p.is_absolute() or not (p / marker).is_file(): raise SystemExit(f'{name} must be an absolute directory containing {marker}')
 return p.resolve()
SRC = _root('I560_SRC', 'tools/diagnostics/check_socket_paths.py')
HARNESS = _root('I560_HARNESS', 'campaign_workloads.py')
from urllib.request import Request, urlopen
sys.path.insert(0,str(HARNESS));sys.path.insert(0,str(SRC/'tests/performance'))
from bench_ui_process import EvidenceError, ProcessFamily, identity, same_identity
from campaign_workloads import upload_source, submit_file_upload, verify_uploaded
from campaign_fixture import png
from x11_diagnostic import owned_windows,close_window,save
class Run:
 def __init__(self):
  self.root=Path(sys.argv[1]); self.root.mkdir(mode=0o700);self.rows=[];self.events=[];self.resident=None;self.server=None;self.tray=None;self.app_pid=None;self.family=None;self.families=[];self.started=time.monotonic();self.latencies=[]
  self.environment=os.environ.copy()
  for key,name in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_CACHE_HOME','cache'),('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','runtime')]:
   p=self.root/name;p.mkdir(mode=0o700);self.environment[key]=str(p)
  self.environment.update(QT_QPA_PLATFORM='xcb',XDG_SESSION_TYPE='x11',XDG_CURRENT_DESKTOP='Cinnamon')
  for key in ('WAYLAND_DISPLAY','XDG_SESSION_ID'):self.environment.pop(key,None)
  self.socket=str(self.root/'runtime/putgd/session.sock')
  subprocess.run(['python3',str(SRC/'tools/diagnostics/check_socket_paths.py'),'--socket',self.socket],check=True,stdout=(self.root/'preflight.json').open('w'))
  self.log=(self.root/'resident.log').open('wb');self.fixturelog=(self.root/'fixture.err').open('wb')
  files=[Path(__file__),HARNESS/'campaign_workloads.py',HARNESS/'campaign_fixture.py',HARNESS/'tray_stub.py',HARNESS/'x11_diagnostic.py',SRC/'tests/performance/bench_ui_process.py']
  self.metadata={'sources':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},'build':json.loads(subprocess.check_output([sys.argv[2],'--version-json'])),'scope':'Xvfb with simulated private-bus SNI peer selecting X11 Core resident; not physical/compositor acceptance','settleSeconds':int(os.environ.get('I560_SETTLE','300')),'mallocArenaMaxEnv':os.environ.get('MALLOC_ARENA_MAX'),'glibcTunables':os.environ.get('GLIBC_TUNABLES'),'thpOff':bool(os.environ.get('I560_THP_OFF'))}
  (self.root/'metadata.json').write_text(json.dumps(self.metadata,indent=2))
 def call(self,method,params=None):
  began=time.monotonic()
  try:return self._call(method,params)
  finally:self.latencies.append((round(began-self.started,3),method,round(time.monotonic()-began,4)))
 def _call(self,method,params=None):
  with socket.socket(socket.AF_UNIX) as connection:
   connection.settimeout(30);connection.connect(self.socket)
   connection.sendall(json.dumps({'schemaVersion':1,'requestId':str(uuid.uuid4()),'session':self.socket,'method':method,'params':params or {}}).encode()+b'\n')
   data=bytearray()
   while not data.endswith(b'\n'):
    chunk=connection.recv(65536)
    if not chunk or len(data)>8*1024**2:raise RuntimeError('transport')
    data.extend(chunk)
  result=json.loads(data)
  if not result.get('ok'):raise EvidenceError('cli_'+result['error']['code'])
  return result['result']
 def event(self,kind,**values):self.events.append({'kind':kind,**values})
 def sample(self,label):
  fields={}
  for line in Path(f'/proc/{self.app_pid or self.resident.pid}/smaps_rollup').read_text().splitlines():
   pair=line.split()
   if len(pair)==3 and pair[2]=='kB':fields[pair[0].rstrip(':')]=int(pair[1])*1024
  deadline=time.monotonic()+5;unsettled=0
  while True:
   try:family=self.family.sample();break
   except EvidenceError as error:
    if str(error)!='process_family_did_not_settle' or time.monotonic()>=deadline:raise
    unsettled+=1;time.sleep(.05)
  if unsettled:family['unsettledRetries']=unsettled
  assert family['escapedProcessCount']==0,'escaped_product_descendant'
  for row in family['processes']:
   if Path(row['executable']).name=='putgd-s3-worker':row['role']='s3-worker'
  self.rows.append({'label':label,'elapsedSeconds':time.monotonic()-self.started,**fields,'family':family})
  print(json.dumps({'label':label,'residentRss':fields['Rss'],'residentPss':fields['Pss'],'familyTotals':family['totals'],'processCount':family['processCount']}),flush=True)
  return self.rows[-1]
 def fixture(self,path='/__state',value=None):
  req=Request(self.args.endpoint+path,data=json.dumps(value).encode() if value is not None else None,headers={'Content-Type':'application/json'})
  with urlopen(req,timeout=10) as response:return json.load(response)
 def start(self,profile):
  command=[sys.argv[2],'--background']
  if os.environ.get('I560_THP_OFF'):command=[sys.executable,str(HARNESS/'thp_off.py')]+command
  if profile:command=['heaptrack','--output',str(self.root/'heap')]+command
  self.resident=subprocess.Popen(command,env=self.environment,stdout=self.log,stderr=self.log,start_new_session=True)
  deadline=time.monotonic()+25
  while True:
   try:
    if self.call('diagnostics')['journalReady']:break
   except (OSError,EvidenceError):pass
   if time.monotonic()>deadline:raise RuntimeError('readiness')
   time.sleep(.1)
  if profile:
   for pid in Path('/proc').iterdir():
    try:
     if pid.name.isdigit() and (pid/'exe').resolve()==Path(sys.argv[2]).resolve():
      env=(pid/'environ').read_bytes()
      if ('XDG_RUNTIME_DIR='+self.environment['XDG_RUNTIME_DIR']).encode()+b'\0' in env:self.app_pid=int(pid.name)
    except (OSError,ValueError):pass
   assert self.app_pid
  self.family=ProcessFamily(self.app_pid or self.resident.pid,sys.argv[2],str(Path(sys.argv[2]).with_name('putgd-ui')));self.families.append(self.family)
 def history_checkpoint(self,label):
  assert self.call('ui.show',{'view':'history'})['shown']=='history'
  deadline=time.monotonic()+20
  while True:
   state=self.call('ui.snapshot')['diagnostics']['ui']
   if state.get('state')=='ready':
    windows=owned_windows(state['pid'])
    if len(windows)==1:break
   if time.monotonic()>deadline:raise EvidenceError('history_window_not_ready')
   time.sleep(.1)
  if hasattr(self,'ui_owner'):assert same_identity(identity(state['pid']),self.ui_owner)
  else:self.ui_owner=identity(state['pid']);self.ui_window=windows[0]
  assert windows==[self.ui_window]
  time.sleep(2);save(self.root/(label+'.png'))
  self.event('history-checkpoint',label=label,owner=self.ui_owner,window=self.ui_window)
  self.sample(label)
 def close_history(self):
  assert same_identity(identity(self.ui_owner['pid']),self.ui_owner)
  assert owned_windows(self.ui_owner['pid'])==[self.ui_window]
  close_window(self.ui_window)
  deadline=time.monotonic()+20
  while True:
   state=self.call('ui.snapshot')['diagnostics']['ui']
   if state.get('state')=='stopped':break
   if time.monotonic()>deadline:raise EvidenceError('history_close_not_normal')
   time.sleep(.1)
  self.event('history-closed',diagnostics=state)
  time.sleep(2)
 def run(self):
  self.tray=subprocess.Popen([sys.executable,str(HARNESS/'tray_stub.py')],env=self.environment,stdout=subprocess.PIPE,stderr=self.fixturelog)
  assert self.tray.stdout.readline().strip()==b'ready','private_tray_unavailable'
  self.server=subprocess.Popen([sys.executable,str(SRC/'tests/fixtures/s3_server.py')],cwd=self.root,stdout=subprocess.PIPE,stderr=self.fixturelog,start_new_session=True)
  port=int(self.server.stdout.readline());self.args=types.SimpleNamespace(endpoint=f'http://127.0.0.1:{port}')
  self.start(False)
  doc=self.call('settings.developmentTemplate')['document'];profile=doc['profiles'][0]
  profile.update(id='isolated-fixture',label='Synthetic loopback',credentials={'mode':'session','reference':None})
  profile['settings'].update(endpoint=self.args.endpoint,bucket='fixture',region='us-east-1',publicUrlBase=self.args.endpoint+'/fixture',objectPrefix='i560/',aclMode='public-read',requestTimeoutMs=120000,lowSpeedTimeSeconds=120)
  doc['profiles']=[profile];doc['archive'].update(root=str(self.root/'archive'),keepUploads=True)
  doc['preferences'].update(notifications=False,launchAtLogin=False,updates={'check':False,'channel':'build'})
  doc['limits'].update(pendingOperations=64,concurrentUploads=4);doc['naming']['mode']='original'
  for action in doc['actions'].values():
   if 'destination' in action:action.update(destination='upload',profileId='isolated-fixture',shortcut='')
  initial=json.loads(json.dumps(doc));initial['profiles']=[]
  for action in initial['actions'].values():
   if 'destination' in action:action.update(destination='local',profileId=None,shortcut='')
  self.call('settings.patch',{'patch':initial,'expectedRevision':self.call('settings.get')['metadata']['observedRevision']})
  self.sample('setup-before-normal-stop')
  self.call('ui.quit');self.resident.wait(timeout=60);assert self.resident.returncode==0,'setup_exit_not_normal'
  self.start(bool(os.environ.get('I560_HEAPTRACK')))
  self.call('settings.patch',{'patch':doc,'expectedRevision':self.call('settings.get')['metadata']['observedRevision']})
  self.call('setup.storeCredentials',{'profileId':'isolated-fixture','expectedRevision':self.call('settings.get')['metadata']['acceptedRevision'],'credentials':{'accessKey':'PUTGD_SYNTHETIC_ACCESS','secretKey':'putgd-synthetic-secret-not-a-real-key'}})
  time.sleep(3);diagnostics=self.call('ui.snapshot')['diagnostics'];assert diagnostics['trayAvailable'],'synthetic_tray_not_registered'
  self.event('headless-environment',kindOfDisplay='Xvfb',tray='private simulated SNI peer',diagnostics=diagnostics)
  self.sample('baseline');self.fixture('/__rules',{'rules':[{'method':'PUT','delay':10,'remaining':100}]})
  self.history_checkpoint('history-before-burst')
  if os.environ.get('I560_MODE')=='cancel':return self.cancel_workload()
  if os.environ.get('I560_MODE')=='cancel2':return self.cancel_workload2()
  if os.environ.get('I560_MODE')=='cancel3':return self.cancel_workload2(attribute=True)
  media=self.root/'burst-media';media.mkdir(mode=0o700);operations=[];sources=[]
  for index in range(50):
   path=media/f'queue-{index:02d}-synthetic.png';png(path,index+100,320,180);path.chmod(0o400)
   source=upload_source(path,self.root);sources.append(source);operations.append(submit_file_upload(self,source))
   if index%5==4:
    states=[self.call('operation.inspect',{'id':key}) for key in operations]
    self.event('queue',queued=sum(x.get('phase')=='queued' for x in states),uploading=sum(any(i.get('uploadStatus')=='uploading' for i in x['items']) for x in states))
    self.call('operation.recent');self.sample(f'submitted-{index+1}')
    if index in (4,24,49):self.history_checkpoint(f'history-progress-{index}')
  for key,source in zip(operations,sources):
   deadline=time.monotonic()+150
   while True:
    operation=self.call('operation.inspect',{'id':key})
    if operation['state'] in ('completed','failed','partial','cancelled'):break
    if time.monotonic()>deadline:raise RuntimeError('upload_timeout')
    time.sleep(.2)
   verify_uploaded(self,operation,source)
  self.history_checkpoint('history-completed');self.close_history()
  self.fixture('/__rules',{'rules':[]});self.sample('drained')
  for index in range(int(os.environ.get('I560_SETTLE','300'))//5):time.sleep(5);self.sample(f'settle-{5*(index+1)}')
  self.event('complete',verified=50,diagnostics=self.call('diagnostics'))
  (self.root/'post-score-smaps.txt').write_text(Path(f'/proc/{self.app_pid or self.resident.pid}/smaps').read_text())
  (self.root/'post-score-status.txt').write_text(Path(f'/proc/{self.app_pid or self.resident.pid}/status').read_text())
  assert self.rows[-1]['family']['processCount']==1,'helpers_not_drained'
 def cancel_workload(self):
  """Bounded cancellation/normal-cleanup control: 8 uploads at concurrency 4, 10 s PUT delay;
  cancel two transferring and two queued operations, the other four must complete and verify."""
  media=self.root/'burst-media';media.mkdir(mode=0o700);operations=[];sources=[]
  for index in range(8):
   path=media/f'queue-{index:02d}-synthetic.png';png(path,index+300,320,180);path.chmod(0o400)
   source=upload_source(path,self.root);sources.append(source);operations.append(submit_file_upload(self,source))
  deadline=time.monotonic()+30
  while True:
   states={key:self.call('operation.inspect',{'id':key}) for key in operations}
   uploading=[k for k,x in states.items() if any(i.get('uploadStatus')=='uploading' for i in x['items'])]
   queued=[k for k,x in states.items() if x.get('phase')=='queued' and k not in uploading]
   if len(uploading)>=2 and len(queued)>=2:break
   if time.monotonic()>deadline:raise EvidenceError('cancel_setup_states')
   time.sleep(.2)
  targets=uploading[:2]+queued[:2];timings={}
  for key in targets:
   began=time.monotonic();self.call('operation.cancel',{'id':key})
   while self.call('operation.inspect',{'id':key})['state'] not in ('cancelled','failed','partial','completed'):
    if time.monotonic()-began>30:raise EvidenceError('cancel_not_settled')
    time.sleep(.05)
   timings[key]=round(time.monotonic()-began,3)
  self.sample('cancelled')
  outcomes={}
  for key,source in zip(operations,sources):
   deadline=time.monotonic()+150
   while True:
    operation=self.call('operation.inspect',{'id':key})
    if operation['state'] in ('completed','failed','partial','cancelled'):break
    if time.monotonic()>deadline:raise RuntimeError('upload_timeout')
    time.sleep(.2)
   outcomes[key]=operation['state']
   if key in targets:assert operation['state']=='cancelled',('cancel_state',operation['state'])
   else:verify_uploaded(self,operation,source)
  self.event('cancel-control',targets=targets,transferringCancelled=uploading[:2],queuedCancelled=queued[:2],cancelSettleSeconds=timings,outcomes=outcomes)
  self.history_checkpoint('history-completed');self.close_history()
  self.fixture('/__rules',{'rules':[]});self.sample('drained')
  for index in range(6):time.sleep(5);self.sample(f'settle-{5*(index+1)}')
  self.event('complete',verified=4,cancelled=4,diagnostics=self.call('diagnostics'))
  assert self.rows[-1]['family']['processCount']==1,'helpers_not_drained'
 def wire(self):
  """Fixture inventory without headers or bodies: method/key/size per request, stored object keys."""
  state=self.fixture()
  return [{'method':e['method'],'key':e['key'],'bodyBytes':e['bodyBytes']} for e in state['events']],sorted(state['objects'])
 def cancel_workload2(self,attribute=False):
  """Corrected cancellation control (#579 review 4372113423/4372122113).
  Concurrency 2 and a 30 s PUT delay hold two transfers; four more operations queue. Each queued target
  gets a fresh operation.inspect witness immediately before its cancel, and queued targets are cancelled
  before any transfer is cancelled. The witness and the cancel are separate requests, so this does not
  prove atomicity; the held transfers make promotion in between implausible, and the fixture inventory
  afterwards checks that no PUT reached a cancelled queued target."""
  settings=self.call('settings.get');doc=settings['document'];doc['limits']['concurrentUploads']=2
  self.call('settings.patch',{'patch':doc,'expectedRevision':settings['metadata']['observedRevision']})
  self.fixture('/__rules',{'rules':[{'method':'PUT','delay':30,'remaining':100}]})
  media=self.root/'burst-media';media.mkdir(mode=0o700);operations=[];sources=[]
  for index in range(6):
   path=media/f'queue-{index:02d}-synthetic.png';png(path,index+700,320,180);path.chmod(0o400)
   source=upload_source(path,self.root);sources.append(source);operations.append(submit_file_upload(self,source))
  deadline=time.monotonic()+30
  while True:
   states={key:self.call('operation.inspect',{'id':key}) for key in operations}
   uploading=[k for k,x in states.items() if any(i.get('uploadStatus')=='uploading' for i in x['items'])]
   queued=[k for k,x in states.items() if x.get('phase')=='queued' and k not in uploading]
   if len(uploading)==2 and len(queued)==4:break
   if time.monotonic()>deadline:raise EvidenceError('cancel2_setup_states')
   time.sleep(.2)
  names={key:Path(source['path']).name for key,source in zip(operations,sources)}
  queuedTargets=queued[:2];transferTargets=uploading[:2];witnesses=[];timings={}
  def cancel(key,expected):
   witness=self.call('operation.inspect',{'id':key});sent=time.monotonic()
   item=[{'uploadStatus':i.get('uploadStatus'),'phase':i.get('phase')} for i in witness['items']]
   witnesses.append({'id':key,'expected':expected,'state':witness['state'],'phase':witness.get('phase'),'items':item,'witnessToCancelSeconds':None})
   if expected=='queued':assert witness.get('phase')=='queued' and not any(i['uploadStatus']=='uploading' for i in item),('not_queued_at_cancel',witness.get('phase'),item)
   else:assert any(i['uploadStatus']=='uploading' for i in item),('not_transferring_at_cancel',item)
   self.call('operation.cancel',{'id':key});witnesses[-1]['witnessToCancelSeconds']=round(time.monotonic()-sent,4)
   while self.call('operation.inspect',{'id':key})['state'] not in ('cancelled','failed','partial','completed'):
    if time.monotonic()-sent>40:raise EvidenceError('cancel2_not_settled')
    time.sleep(.05)
   timings[key]=round(time.monotonic()-sent,3)
  for key in queuedTargets:cancel(key,'queued')
  for key in transferTargets:cancel(key,'transferring')
  self.sample('cancelled')
  outcomes={}
  for key,source in zip(operations,sources):
   deadline=time.monotonic()+150
   while True:
    operation=self.call('operation.inspect',{'id':key})
    if operation['state'] in ('completed','failed','partial','cancelled'):break
    if time.monotonic()>deadline:raise RuntimeError('upload_timeout')
    time.sleep(.2)
   outcomes[key]=operation['state']
   if key in queuedTargets+transferTargets:assert operation['state']=='cancelled',('cancel_state',operation['state'])
   else:verify_uploaded(self,operation,source)
  if attribute:return self.attribute_cancel(operations,sources,queuedTargets,transferTargets,witnesses,timings,outcomes)
  events,objects=self.wire()
  queuedPuts={key:[e for e in events if e['method']=='PUT' and e['key'].endswith(names[key])] for key in queuedTargets}
  queuedObjects={key:[o for o in objects if o.endswith(names[key])] for key in queuedTargets}
  assert not any(queuedPuts.values()) and not any(queuedObjects.values()),('queued_target_reached_destination',queuedPuts,queuedObjects)
  transferObjects={key:[o for o in objects if o.endswith(names[key])] for key in transferTargets}
  self.event('cancel2-control',queuedTargets=queuedTargets,transferTargets=transferTargets,witnesses=witnesses,cancelSettleSeconds=timings,outcomes=outcomes,
   queuedTargetPuts={k:len(v) for k,v in queuedPuts.items()},queuedTargetObjects={k:len(v) for k,v in queuedObjects.items()},
   transferTargetObjectsAfterCancel={k:len(v) for k,v in transferObjects.items()},wireRequests=len(events),
   limitation='Witness and cancel are separate requests; a held 30 s transfer makes promotion in between implausible, not impossible. Transfer-target objects are reported, not asserted: an in-flight PUT may have taken effect.')
  self.history_checkpoint('history-completed');self.close_history()
  self.fixture('/__rules',{'rules':[]});self.sample('drained')
  for index in range(6):time.sleep(5);self.sample(f'settle-{5*(index+1)}')
  self.event('complete',verified=len(operations)-4,cancelled=4,diagnostics=self.call('diagnostics'))
  assert self.rows[-1]['family']['processCount']==1,'helpers_not_drained'
 @staticmethod
 def pixel_digest(data):
  import io
  from PIL import Image
  try:
   with Image.open(io.BytesIO(data)) as image:return hashlib.sha256(image.convert('RGB').tobytes()).hexdigest()
  except Exception:return None
 def attribution(self,state,pixels):
  """Map every stored object version and every PUT request body to a source operation by pixel SHA-256."""
  import base64
  owner={digest:key for key,digest in pixels.items()}
  objects=[];requests=[]
  for key,versions in state['objects'].items():
   for version in versions:
    if version.get('marker'):continue
    if version.get('blob'):objects.append({'key':key,'operation':None,'reason':'blob_not_in_state'});continue
    digest=self.pixel_digest(base64.b64decode(version['body']))
    objects.append({'key':key,'operation':owner.get(digest),'decoded':digest is not None})
  for event in state['events']:
   if event['method']!='PUT' or not event['bodyBytes']:continue
   if not event['body']:requests.append({'key':event['key'],'operation':None,'reason':'body_not_inline'});continue
   digest=self.pixel_digest(base64.b64decode(event['body']))
   requests.append({'key':event['key'],'operation':owner.get(digest),'decoded':digest is not None})
  return objects,requests
 def attribute_cancel(self,operations,sources,queuedTargets,transferTargets,witnesses,timings,outcomes):
  import base64
  pixels={key:source['pixelSha256'] for key,source in zip(operations,sources)}
  assert len(set(pixels.values()))==len(pixels),'source_pixels_not_unique'
  survivors=[k for k in operations if k not in queuedTargets+transferTargets]
  state=self.fixture()
  objects,requests=self.attribution(state,pixels)
  unattributed=[x for x in objects+requests if x['operation'] is None]
  count=lambda rows,key:sum(x['operation']==key for x in rows)
  # Positive sanity: the checker must find every verified survivor's object and request.
  assert all(count(objects,k)==1 and count(requests,k)>=1 for k in survivors),('survivor_not_attributed',objects,requests)
  assert not unattributed,('unattributed_payloads',unattributed)
  queuedHits={k:{'objects':count(objects,k),'puts':count(requests,k)} for k in queuedTargets}
  assert not any(v['objects'] or v['puts'] for v in queuedHits.values()),('queued_target_published',queuedHits)
  transferHits={k:{'objects':count(objects,k),'puts':count(requests,k)} for k in transferTargets}
  # Negative self-test: publish a queued target's own source PNG under a generated-style key, then
  # repeat the same attribution. It must report that target; an unmatched filename proves nothing.
  # r2: the run's 30 s PUT delay rule also delayed this unsigned self-test PUT past its client timeout (r1 failed
  # here after the real assertions had passed). Clear the rules first; the real inventory is already taken.
  self.fixture('/__rules',{'rules':[]})
  victim=queuedTargets[0];victimPath=Path(sources[operations.index(victim)]['path'])
  key='fixture/i560/Screen-Capture-INJECTED-self-test.png'
  request=Request(self.args.endpoint+'/'+key,data=victimPath.read_bytes(),method='PUT')
  with urlopen(request,timeout=60) as response:assert response.status==200
  injected,injectedRequests=self.attribution(self.fixture(),pixels)
  detected={'objects':count(injected,victim),'puts':count(injectedRequests,victim)}
  assert detected=={'objects':1,'puts':1},('injection_not_detected',detected)
  self.event('cancel3-control',queuedTargets=queuedTargets,transferTargets=transferTargets,survivors=survivors,witnesses=witnesses,
   cancelSettleSeconds=timings,outcomes=outcomes,objectsInventoried=len(objects),putRequestsInventoried=len(requests),unattributed=len(unattributed),
   survivorAttribution={k:{'objects':count(objects,k),'puts':count(requests,k)} for k in survivors},queuedTargetHits=queuedHits,
   transferTargetHits=transferHits,injectionSelfTest={'victim':victim,'key':key,'detected':detected,'recordedAfterRealResult':True},
   limitation='Attribution is by decoded pixel SHA-256 of every stored object version and PUT body in the fixture state; witness and cancel are separate requests. Transfer-target hits are reported, not asserted: an in-flight PUT may have taken effect.')
  self.history_checkpoint('history-completed');self.close_history()
  self.fixture('/__rules',{'rules':[]});self.sample('drained')
  for index in range(6):time.sleep(5);self.sample(f'settle-{5*(index+1)}')
  self.event('complete',verified=len(survivors),cancelled=4,diagnostics=self.call('diagnostics'))
  assert self.rows[-1]['family']['processCount']==1,'helpers_not_drained'
 def close(self):
  if self.resident and self.resident.poll() is None:
   try:self.call('ui.quit');self.resident.wait(timeout=60)
   except (OSError,EvidenceError,subprocess.TimeoutExpired):
    self.event('cleanup-fallback',reason='normal_quit_failed');self.fallback=True
    if self.app_pid:
     import signal
     try:os.kill(self.app_pid,signal.SIGTERM)
     except ProcessLookupError:pass
    else:self.resident.terminate()
    self.resident.wait(timeout=30)
  if self.server and self.server.poll() is None:self.server.terminate();self.server.wait(timeout=30)
  if self.tray and self.tray.poll() is None:self.tray.terminate();self.tray.wait(timeout=10)
  remaining=[]
  for family in self.families:
   for expected in family.known.values():
    try:
     if same_identity(identity(expected['pid']),expected):remaining.append(expected)
    except FileNotFoundError:pass
  problem=cleanup_failure(self.resident.returncode if self.resident else None,getattr(self,'fallback',False),remaining)
  self.event('cleanup',problem=problem,remaining=remaining,residentExit=self.resident.returncode if self.resident else None,fixtureExit=self.server.returncode if self.server else None)
  (self.root/'results.json').write_text(json.dumps({'scope':'headless attribution only','rows':self.rows,'events':self.events,'exit':self.resident.returncode if self.resident else None,'settleSeconds':int(os.environ.get('I560_SETTLE','300')),'callLatencies':self.latencies,'cleanupProblem':problem,'trackedIdentities':[{'root':f.root,'exited':f.exited,'known':list(f.known.values())} for f in self.families]},indent=2))
  return problem
def cleanup_failure(resident_exit,fallback,remaining):
 """r3 (#579 Cubic 4372241070): a run whose resident exited abnormally, needed the forced fallback, or left
 tracked processes behind must not report success, even if its workload passed."""
 if fallback:return 'forced_cleanup_fallback'
 if resident_exit not in (0,None):return f'resident_exit_{resident_exit}'
 if remaining:return 'tracked_processes_remaining'
 return None
def main(factory=None):
 os.umask(0o077);run=(factory or Run)();primary=None;problem=None
 try:run.run()
 except BaseException as error:
  primary=error;run.event('failure',errorCode=str(error) if isinstance(error,EvidenceError) else type(error).__name__)
 finally:problem=run.close()
 # The workload's own failure stays primary; cleanup evidence is already in results.json.
 if primary is not None:raise primary
 if problem:print(json.dumps({'cleanupProblem':problem}),file=sys.stderr);raise SystemExit(3)
if __name__=='__main__':main()
