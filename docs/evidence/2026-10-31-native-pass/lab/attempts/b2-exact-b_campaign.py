#!/usr/bin/env python3
"""Physical #379 campaign wrapper around unchanged 00ab777f observers.

Published native app only. Clipboard lease and builder lock are mandatory.
Python X11 capture observer is separate, identified supplemental tooling.
"""
import json,os,sys,subprocess,threading,time,traceback,hashlib,signal
from pathlib import Path
OUTPUT=Path(os.environ.get('NATIVE_OUTPUT','/tmp/npb2'));GUARD=os.environ.get('NATIVE_GUARD','/tmp/bg2')
BASE=Path('/tmp/np31');FROZEN=BASE/'frozen-src';CURRENT=BASE/'src'
sys.path[:0]=[str(CURRENT),str(FROZEN/'tests/performance'),str(BASE)]
from tools.lab.surface_driver.target import Target
from tools.lab.surface_driver.run import guard_target
from tools.lab.record_run import session_environment
from tools.lab import desktop_lock
from types import SimpleNamespace
import physical_campaign as pc
import b_native

def phase(name,fixture,desktop_session):
 scenario={'capture-empty':'capture','capture-10k':'capture','links':'capture','native-burst':'burst','slow100':'slow','seed':'seed','memory-10k':'memory','history-10k':'history','slow':'slow'}[name]
 output=str(OUTPUT/name)
 argv=[scenario,'--output',output,'--package','/tmp/np31/putgd-26.1031.3384-nightly-ubuntu24.04-x86_64.deb','--package-sha256','c0ec0359d3533c6cf693fb64f071f9dabb018710ad6fbf261f62f8c5111ed278','--version','26.1031.3384-nightly','--source-revision','4d3840c6108c22546d83c43a1b7d4c77b1df9d6d','--desktop-session',desktop_session,'--lock-session','np31-ddd3576a','--driver-revision','00ab777f2413a1fbd7670a7259dc614eced896e6','--endpoint','http://127.0.0.1:'+str(fixture['port']),'--fixture-pid',str(fixture['pid']),'--fixture-source',str(FROZEN/'tests/fixtures/s3_server.py'),'--threshold-policy',str(BASE/'history-thresholds.json'),'--native-driver',str(FROZEN/'tools/lab/native_acceptance.py'),'--native-driver-revision','00ab777f2413a1fbd7670a7259dc614eced896e6','--native-driver-sha256',pc.sha256(FROZEN/'tools/lab/native_acceptance.py')]
 if name in ('capture-10k','memory-10k','history-10k'):argv+=['--fixture-root',str(OUTPUT/'seed')]
 if name in ('history-10k','slow','slow100'):argv+=['--enable-accessibility']
 args=pc.parser().parse_args(argv);run=pc.Campaign(args);complete=False
 try:
  run.initialize();run.fixture_identity=pc.verify_fixture(args);run.configure();run.static_cover();run.start();cap=run.checked_live()
  run.result['supplementalObserver']={'sha256':pc.sha256(BASE/'b_native.py'),'path':str(BASE/'b_native.py'),'scope':'Python XTest real hotkey and owned native overlay drawable/root paint; distinct from frozen C++ driver'}
  if name.startswith('capture-') or name=='links':b_native.latency(run,with_links=name=='links')
  elif name=='native-burst':b_native.burst(run)
  elif name=='seed':run.seed()
  elif name=='memory-10k':run.memory()
  elif name=='history-10k':
   import b_history
   b_history.run(run)
  else:
   import campaign_workloads
   import b_history,campaign_ui
   campaign_ui.Accessible=b_history.Accessible
   if name=='slow100':
    import b_tc
    with b_tc.scoped(run):getattr(campaign_workloads,scenario)(run)
   else:getattr(campaign_workloads,scenario)(run)
  complete=True
 except BaseException as error:
  if isinstance(error,KeyboardInterrupt):run.result['interrupted']=True
  run.result['error']={'type':type(error).__name__,'reason':str(error),'traceback':traceback.format_exc()}
  try:run.visual('failed-workload')
  except BaseException as visual:run.result['failureVisualUnavailable']=type(visual).__name__
 finally:
  cleanup=True
  for close in (run.stop,run.close_fixtures):
   try:cleanup=bool(close()) and cleanup
   except BaseException as error:cleanup=False;run.result.setdefault('cleanupErrors',[]).append(type(error).__name__)
  run.result['status']='complete' if complete and cleanup else 'failed-or-interrupted';run.result['gates']['cleanup']={'status':'pass' if cleanup else 'fail'};run.summarize_processes();run.persist()
 if run.result.get("interrupted"):raise KeyboardInterrupt()
 return run.result

def main():
 os.umask(0o077)
 def interrupted(_signal,_frame):raise KeyboardInterrupt()
 for sig in (signal.SIGTERM,signal.SIGHUP):signal.signal(sig,interrupted)
 native,_=session_environment();os.environ.update(native)
 roots=OUTPUT;roots.mkdir(mode=0o700,exist_ok=True)
 t=Target(host_profile=str(BASE/'host.json'),session_root=GUARD)
 guard_target(SimpleNamespace(placement='native',host='m74',lock_session='np31-ddd3576a'),t)
 # The lease alone is needed: no extra resident competes with the benchmark.
 t.preflight()
 import shlex
 r=t.execute('test ! -e '+shlex.quote(GUARD)+' && mkdir -m700 '+shlex.quote(GUARD)+' && mkdir -m700 '+' '.join(shlex.quote(GUARD+'/'+x) for x in ('runtime','config','data','state','cache','files')));assert r['returncode']==0
 t._created_root=True;t._owns_root=True;t.preserve_clipboard()
 stop=threading.Event()
 def heartbeat():
  while not stop.wait(60):
   try:
    value=desktop_lock.Desktop(Path.home()/'putgd-lab/locks','m74').extend('np31-ddd3576a',45);t.heartbeat(force=True)
   except BaseException:
    (roots/'heartbeat-failed').write_text('lock renewal failed');os.kill(os.getpid(),signal.SIGTERM);return
 thread=threading.Thread(target=heartbeat,daemon=True);thread.start()
 fixture=None
 try:
  sessions=subprocess.check_output(['loginctl','list-sessions','--no-legend'],text=True).splitlines();matches=[]
  for line in sessions:
   sid=line.split()[0]
   text=subprocess.check_output(['loginctl','show-session',sid,'-p','Active','-p','Remote','-p','Type','-p','Desktop','-p','Display'],text=True)
   if all(v in text.splitlines() for v in ['Active=yes','Remote=no','Type=x11','Display=:0']) and 'cinnamon' in text.lower():matches.append(sid)
  assert len(matches)==1
  fixture=subprocess.Popen([sys.executable,str(FROZEN/'tests/fixtures/s3_server.py')],stdout=subprocess.PIPE,stderr=open(roots/'fixture.log','ab'),text=True,start_new_session=True)
  fixture_info={'pid':fixture.pid,'port':int(fixture.stdout.readline())};(roots/'fixture-identity.json').write_text(json.dumps(fixture_info))
  results={};fixture_ready=False;native_observer_failed=False
  for name in sys.argv[1:]:
   print('PHASE',name,flush=True)
   if name=='grow':
    args=['python3',str(CURRENT/'tools/lab/native_acceptance.py'),'--output',str(roots/'growth.json'),'grow','--fixture-root',str(roots/'seed'),'--db',str(roots/'seed/data/putgd/history.sqlite'),'--runtime',str(roots/'seed/runtime'),'--to','10000']
    r=subprocess.run(args,capture_output=True,text=True,timeout=60);(roots/'growth.log').write_text(r.stdout+r.stderr);results[name]={'exit':r.returncode};
    fixture_ready=(r.returncode==0)
   elif name in ('links','capture-10k','native-burst') and native_observer_failed:
    results[name]={'status':'blocked','reason':'second native paint observer attempt failed; no further desktop guesses'}
   elif name in ('capture-10k','memory-10k','history-10k') and not fixture_ready:
    results[name]={'status':'blocked','reason':'10k fixture growth failed; original failure retained'}
   else:
    result=phase(name,fixture_info,matches[0]);results[name]={'status':result['status'],'error':result.get('error'),'gates':result.get('gates')}
    if name=='capture-empty' and result['status']!='complete':native_observer_failed=True
    # Every phase retains its own failed attempt. Later independent phases run.
   (roots/'summary.json').write_text(json.dumps(results,indent=2))
   print(name,json.dumps(results[name]),flush=True)
   if results[name].get('gates',{}).get('cleanup',{}).get('status')=='fail':raise RuntimeError('phase cleanup requires investigation')
 finally:
  if fixture is not None and fixture.poll() is None:fixture.terminate();fixture.wait(timeout=15)
  stop.set();thread.join(timeout=10)
  cleanup=t.close();(roots/'clipboard-cleanup.json').write_text(json.dumps({'errors':cleanup,'lease':t.clipboard_report},indent=2));assert not cleanup
if __name__=='__main__':main()

