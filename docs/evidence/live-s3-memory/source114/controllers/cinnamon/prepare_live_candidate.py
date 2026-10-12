#!/usr/bin/env python3
"""Fresh isolated live profile; use only after current-version native acceptance.

Host-only stdlib orchestration. Does not change the frozen workload or oracles.
Stops after synthetic fixture preparation so the console can be reviewed before
any publication. Protected credentials stay in the existing guarded setup phase.
"""
import argparse,importlib.util,json,shutil,subprocess,sys,time
from pathlib import Path
base=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('--bundle',type=Path,required=True);ap.add_argument('--native-receipt',type=Path,required=True);ap.add_argument('--commit',required=True);ap.add_argument('--dev-junit',type=Path,required=True);a=ap.parse_args();root=a.bundle.resolve()
assert root.is_relative_to(base) and root!=base
native=json.loads(a.native_receipt.read_text());assert native['functionalPassed'] is True and native['candidateBuild']['gitCommit']==a.commit,'current_native_parity_required'
assert a.dev_junit.is_file() and not (root/'baseline102.json').exists()
root.mkdir(mode=0o700,exist_ok=True);shutil.copy(a.dev_junit,root/'baseline-dev.xml')
spec=importlib.util.spec_from_file_location('candidate',base/'candidate_run.py');c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
c.save(root/'prior-native-normal-exit.json',c.stop_old())
def phase(name):subprocess.run(['python3',str(base/'candidate_run.py'),name,'--bundle',str(root)],check=True)
phase('prepare');m=json.loads((root/'baseline102.json').read_text());assert m['build']['gitCommit']==a.commit and m['build']==native['candidateBuild'],'live_build_mismatch'
phase('observe');phase('credentials');phase('fixture');time.sleep(20)
c.save(root/'prepublication-ready.json',{'sourceCommit':a.commit,'nativeReceipt':str(a.native_receipt.resolve()),'devJUnit':str(a.dev_junit.resolve()),'operationIds':m['operationIds'],'consoleReviewRequired':True,'firstBatch':['python3',str(base/'candidate_run.py'),'batch','--bundle',str(root),'--kind','clipboard-text','--count','1'],'remainingWorkload':['python3',str(base/'run_remaining.py'),'--bundle',str(root)]})
print(json.dumps({'prepared':True,'bundle':str(root),'publishedOperations':0,'visibleConsoleReviewPending':True}))
