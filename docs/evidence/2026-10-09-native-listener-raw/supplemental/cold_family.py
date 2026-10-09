#!/usr/bin/env python3
"""Supplement the frozen cold harness with owned-family samples; no GUI input."""
import argparse,datetime,hashlib,json,subprocess,sys,time
from pathlib import Path
ROOT=Path('/home/patrick/work/put.gd');sys.path.insert(0,str(ROOT/'tests/performance'))
from bench_ui_process import ProcessFamily, EvidenceError
p=argparse.ArgumentParser();p.add_argument('--app',required=True);p.add_argument('--cli',required=True);p.add_argument('--helper',required=True);p.add_argument('--output',required=True);p.add_argument('--receipt',required=True);a=p.parse_args()
out=Path(a.receipt);out.mkdir(parents=True,exist_ok=False);data={'schemaVersion':1,'scope':'Supplemental physical cold whole-family observer; frozen primary harness unchanged','observerSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'familyHarnessSha256':hashlib.sha256((ROOT/'tests/performance/bench_ui_process.py').read_bytes()).hexdigest(),'samplingSeconds':1,'samples':[],'passed':False}
argv=['python3',str(ROOT/'tests/performance/observe_idle.py'),'--app',a.app,'--cli',a.cli,'--output',a.output,'--seconds','300'];data['command']=argv
(out/'command.json').write_text(json.dumps(data,indent=2)+'\n');family=None
with (out/'stdout').open('x') as s,(out/'stderr').open('x') as e:
 proc=subprocess.Popen(argv,cwd=ROOT,stdout=s,stderr=e)
 try:
  while proc.poll() is None:
   progress=Path(a.output)/'progress.json';sidecar=Path(a.output+'-windows.json')
   if family is None and progress.exists() and sidecar.exists():
    observed=json.loads(sidecar.read_text())
    for row in reversed(observed['observations']):
     pid=row['residentPid']
     try:
      if Path('/proc') .joinpath(str(pid),'exe').resolve(strict=True)==Path(a.app).resolve(strict=True):family=ProcessFamily(pid,a.app,a.helper);break
     except FileNotFoundError:pass
   if family is not None:
    try: data['samples'].append(family.sample())
    except EvidenceError as error:
     completed=Path(a.output)/'results.json'
     if str(error)=='resident_exited' and completed.exists() and json.loads(completed.read_text()).get('budgetPassed'):
      break
     raise
    except FileNotFoundError:
     if proc.poll() is not None:break
     time.sleep(.1);continue
    (out/'progress.json').write_text(json.dumps(data,indent=2)+'\n')
   time.sleep(1)
  rc=proc.wait()
  data['exitCode']=rc;data['passed']=rc==0 and bool(data['samples']) and all(len(x['processes'])==1 for x in data['samples'])
 finally:
  if proc.poll() is None:proc.terminate();proc.wait(timeout=30)
  (out/'result.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps({'receipt':str(out),'exitCode':rc,'familySamples':len(data['samples']),'oneProcessThroughout':data['passed']}));raise SystemExit(0 if data['passed'] else 1)
