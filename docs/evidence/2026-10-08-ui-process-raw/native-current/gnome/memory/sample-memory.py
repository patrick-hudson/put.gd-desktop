#!/usr/bin/env python3
"""Run only after the root confirms every build/functional worker is quiescent."""
from pathlib import Path
import json,sys,shlex,subprocess,datetime,time
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
assert sys.argv[1:]==['--functional-wave-quiescent'],'Explicit root-controlled phase gate required'
run=root/'work/lab-runs/current-gnome-a3d70dc';source=json.loads((out.parent/'source.json').read_text());guest='/home/lab/putgd-u11-'+source['commit'][:12]
receipt=out/'receipt.json';assert receipt.is_file();acceptance=json.loads(receipt.read_text());assert acceptance['commit']==source['commit'] and acceptance['nativeFunctionalPassed'] is True
resident=lab.verify_running_app(run);assert resident==acceptance['runningApp']
session=lab.ssh(run,'loginctl show-session 1 -p Name -p User -p Type -p LockedHint\n');assert 'Name=lab' in session and 'User=1000' in session and 'Type=wayland' in session and 'LockedHint=no' in session,'Restore the owned session visibly before sampling; do not alter lock policy'
snapshot=lab.cli(run,'ui.snapshot');assert snapshot['build']['gitCommit']==source['commit'];assert snapshot['diagnostics']['ui']['pid']==0 and snapshot['diagnostics']['trayAvailable']
assert not (out/'memory').exists();(out/'memory').mkdir()
lab.put(run,receipt,guest+'/native-acceptance.json')
args=['python3',guest+'/source/tests/performance/bench_ui_process.py','--mode','split','--pid',str(resident['pid']),'--start-ticks',str(resident['startTicks']),'--app','/usr/bin/putgd','--helper','/usr/bin/putgd-ui','--cli','/usr/bin/putgd-cli','--socket','/run/user/1000/putgd/session.sock','--source-revision',source['commit'],'--source-archive',guest+'/source.tar','--output',guest+'/memory-after-native','--label','after-native-actions-and-helper-close','--seconds','120','--interval','5','--acceptance-receipt',guest+'/native-acceptance.json']
record={'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'resident':resident,'nativeSessionBefore':session,'argv':args,'functionalReceiptSha256':lab.hash_file(receipt),'scope':'Existing-process observation only; entire observed resident family, no graphical interaction, no process launch/stop'}
lab.write_json(out/'memory/command.json',record)
def guard():
 values=lab.cli(run,'operation.recent')['operations'];d=lab.cli(run,'ui.snapshot')['diagnostics']
 state={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'count':len(values),'operations':sorted([{'id':x['id'],'state':x['state'],'copyStatus':x.get('copyStatus')} for x in values],key=lambda x:x['id']),'uiPid':d['ui']['pid'],'ui':d['ui'],'trayAvailable':d['trayAvailable'],'gnomeHelperAvailable':d['capabilities']['source']['helper']['available']}
 with (out/'memory/quiet-guards.jsonl').open('a') as g:g.write(json.dumps(state)+'\n')
 return state
before=guard();assert before['uiPid']==0 and before['trayAvailable'] and before['gnomeHelperAvailable'] and all(x['state'] in ['completed','failed','cancelled','partial','interrupted'] for x in before['operations']) and all(x['copyStatus'] in ['copied','failed','suppressed','no_result'] for x in before['operations'])
quiet=True
with (out/'memory/observer.log').open('w') as f:
 p=subprocess.Popen(lab.ssh_args(run)+[shlex.join(args)],stdout=f,stderr=subprocess.STDOUT)
 nextGuard=time.monotonic()+5
 while p.poll() is None:
  try:p.wait(timeout=max(0,nextGuard-time.monotonic()))
  except subprocess.TimeoutExpired:pass
  nextGuard+=5
  try:
   current=guard()
   if current['operations']!=before['operations'] or current['count']!=before['count'] or current['ui']!=before['ui'] or current['uiPid'] or not current['trayAvailable'] or not current['gnomeHelperAvailable']:quiet=False
  except (lab.LabError,subprocess.TimeoutExpired,KeyError,ValueError) as error:
   quiet=False
   with (out/'memory/quiet-guards.jsonl').open('a') as g:g.write(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'errorType':type(error).__name__})+'\n')
record['exitCode']=p.returncode;record['quietAccepted']=quiet;record['quietScope']='operation IDs/count/terminal state/copyStatus, complete bounded UI lifecycle and tray checked before, every 5 seconds and after; changing user activity invalidates this sample only'
lab.write_json(out/'memory/command.json',record)
for name in ['results.json','progress.json','samples.jsonl']:lab.get(run,guest+'/memory-after-native/'+name,out/'memory'/name)
p.check_returncode()
# Separate post-interval attribution; the module exposes basenames only.
code=r'''from pathlib import Path
import datetime,hashlib,importlib.util,json
expected=EXPECTED
pid=expected['pid'];proc=Path('/proc')/str(pid)
def identity():return {'pid':pid,'startTicks':int((proc/'stat').read_text().rsplit(')',1)[1].split()[19]),'sha256':hashlib.sha256((proc/'exe').read_bytes()).hexdigest()}
assert identity()==expected
path=Path(SCRIPT)
spec=importlib.util.spec_from_file_location('bench_idle',path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
rows=module.footprint(pid);assert identity()==expected
print(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'identity':expected,'scriptSha256':hashlib.sha256(path.read_bytes()).hexdigest(),'scope':'Separate post-interval resident mapping attribution; basenames only','footprint':rows},indent=2))
'''.replace('EXPECTED',repr(resident)).replace('SCRIPT',repr(guest+'/source/tests/performance/bench_idle.py'))
p=subprocess.run(lab.ssh_args(run)+['python3','-'],input=code,text=True,capture_output=True,timeout=20)
(out/'memory/footprint.json').write_text(p.stdout);(out/'memory/footprint.stderr').write_text(p.stderr);p.check_returncode()
print(json.dumps({'status':'sampled' if quiet else 'invalidated-by-concurrent-activity','output':str(out/'memory'),'quietAccepted':quiet}))
