#!/usr/bin/env python3
"""Explicit serial, frozen-source physical memory confirmation after native pass."""
import datetime,hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path('/home/patrick/work/put.gd');WT=Path('/home/patrick/.codex/worktrees/resident-native-boundary/put.gd');COHORT=ROOT/'work/ui-refinement/native-replay/nativecore-exp001-c49';EVIDENCE=ROOT/'work/resident-native-boundary/physical-c49';COMMIT='676f6e6d6e0b026dcdd1b4ef2ae7dabc8a29ded3'
def read(p):return json.loads(Path(p).read_text())
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def run(label,argv):
 d=EVIDENCE/label;d.mkdir(parents=True,exist_ok=False);(d/'command.json').write_text(json.dumps({'argv':argv,'cwd':str(ROOT),'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n')
 with (d/'stdout').open('x') as s,(d/'stderr').open('x') as e:r=subprocess.run(argv,cwd=ROOT,stdout=s,stderr=e)
 (d/'result.json').write_text(json.dumps({'exitCode':r.returncode,'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat()},indent=2)+'\n');r.check_returncode()
 # Crash-safe receipt accumulation after EACH completed measurement.
 p=ROOT/'.context/compound-engineering/ce-optimize/resident-native-boundary/experiment-log.yaml';x=read(p);entry=x['experiments'][0];entry.setdefault('physicalMeasurements',[]).append({'label':label,'sourceCommit':COMMIT,'receipt':str(d/'result.json'),'sha256':sha(d/'result.json')});p.write_text(json.dumps(x,indent=2)+'\n');assert read(p)['experiments'][0]['physicalMeasurements'][-1]['label']==label
 print(json.dumps({'completed':label,'exitCode':r.returncode}),flush=True)
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=WT,text=True).strip()==COMMIT
assert not subprocess.check_output(['git','status','--porcelain'],cwd=WT,text=True).strip()
manifest=read(COHORT/'host/build.json')['manifest'];assert manifest['gitCommit']==COMMIT and manifest['dirty'] is False
native=read(COHORT/'cinnamon/functional.json');assert native['passed'] is True and native['sourceCommit']==COMMIT
for receipt in native['receipts']:assert sha(receipt['path'])==receipt['sha256']
for index in (1,2):
 cold=f'/tmp/pg-nc1-cold{index}';view=f'/tmp/pg-nc1-view{index}'
 run(f'cold{index}',['python3',str(ROOT/'work/resident-native-boundary/cold_family.py'),'--app',str(WT/'build/release/putgd'),'--cli',str(WT/'build/release/putgd-cli'),'--helper',str(WT/'build/release/putgd-ui'),'--output',cold,'--receipt',str(EVIDENCE/f'cold{index}-family')])
 assert read(Path(cold)/'results.json')['budgetPassed'] and read(cold+'-windows.json')['coldIdleUninterrupted']
 run(f'view{index}',['python3',str(ROOT/'tests/performance/bench_ui_process.py'),'--mode','split','--app',str(WT/'build/release/putgd'),'--cli',str(WT/'build/release/putgd-cli'),'--helper',str(WT/'build/release/putgd-ui'),'--source-revision',COMMIT,'--source-archive',str(COHORT/'source.tar'),'--fixture',cold,'--output',view,'--views','settings','history','activity','--cycles','3','--warmup-seconds','5','--visible-seconds','2','--quiet-seconds','30','--interval','5','--acceptance-receipt',str(COHORT/'cinnamon/functional.json')])
 assert read(Path(view)/'results.json')['passed']
print('Two serial physical cold/view confirmation pairs completed.',flush=True)
