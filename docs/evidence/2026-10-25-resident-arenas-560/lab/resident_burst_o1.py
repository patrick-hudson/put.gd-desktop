#!/usr/bin/env python3
"""Private #560 headless attribution harness (Opus copy o1 of v7), NOT physical/frozen acceptance.
Adds: per-call latency record, optional per-process THP disable (I560_THP_OFF), post-score smaps.
Borrows the frozen source/pixel/receipt verification functions unchanged.
No GUI/tray/paint/quiet-desktop claim. 50 original PNGs, 10s PUT; explicit diagnostic settle duration.
"""
import hashlib, json, os, socket, subprocess, sys, time, uuid, types
from pathlib import Path
from urllib.request import Request, urlopen
sys.path.insert(0,str(Path('/home/patrick/work/i560o/tests/performance')))
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
  subprocess.run(['python3','tools/diagnostics/check_socket_paths.py','--socket',self.socket],check=True,stdout=(self.root/'preflight.json').open('w'))
  self.log=(self.root/'resident.log').open('wb');self.fixturelog=(self.root/'fixture.err').open('wb')
  files=[Path(__file__),Path('/home/patrick/work/i560o-h/harness/campaign_workloads.py'),Path('/home/patrick/work/i560o-h/harness/campaign_fixture.py'),Path('/home/patrick/work/i560o-h/harness/tray_stub.py'),Path('/home/patrick/work/i560o-h/harness/x11_diagnostic.py'),Path('/home/patrick/work/i560o/tests/performance/bench_ui_process.py')]
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
  if os.environ.get('I560_THP_OFF'):command=[sys.executable,'/home/patrick/work/i560o-h/harness/thp_off.py']+command
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
  self.tray=subprocess.Popen([sys.executable,'/home/patrick/work/i560o-h/harness/tray_stub.py'],env=self.environment,stdout=subprocess.PIPE,stderr=self.fixturelog)
  assert self.tray.stdout.readline().strip()==b'ready','private_tray_unavailable'
  self.server=subprocess.Popen([sys.executable,'/home/patrick/work/i560o/tests/fixtures/s3_server.py'],cwd=self.root,stdout=subprocess.PIPE,stderr=self.fixturelog,start_new_session=True)
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
 def close(self):
  if self.resident and self.resident.poll() is None:
   try:self.call('ui.quit');self.resident.wait(timeout=60)
   except (OSError,EvidenceError,subprocess.TimeoutExpired):
    self.event('cleanup-fallback',reason='normal_quit_failed')
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
  self.event('cleanup',remaining=remaining,residentExit=self.resident.returncode if self.resident else None,fixtureExit=self.server.returncode if self.server else None)
  (self.root/'results.json').write_text(json.dumps({'scope':'headless attribution only','rows':self.rows,'events':self.events,'exit':self.resident.returncode if self.resident else None,'settleSeconds':int(os.environ.get('I560_SETTLE','300')),'callLatencies':self.latencies,'trackedIdentities':[{'root':f.root,'exited':f.exited,'known':list(f.known.values())} for f in self.families]},indent=2))
os.umask(0o077);run=Run()
try:run.run()
except BaseException as error:
 run.event('failure',errorCode=str(error) if isinstance(error,EvidenceError) else type(error).__name__);raise
finally:run.close()
