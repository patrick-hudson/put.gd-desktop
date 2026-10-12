#!/usr/bin/env python3
"""One planned continuation, preserving the frozen controller and stopping failure."""
import datetime, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path
os.umask(0o077)
r=Path(__file__).resolve().parent
b=r/'a1'
commit='0258600b752dae6304ba61d48b67d56883dfb6b2'
commands=b/'workload-commands';commands.mkdir(mode=0o700,exist_ok=False)
sequence=[('clipboard-text',1),('clipboard-text',5),('clipboard-text',5),('region',5),('region',5),('region',5),('clipboard-image',1),('clipboard-files',1),('window',1),('desktop',1)]
for index,(kind,count) in enumerate(sequence):
    interval={'seconds':20 if index else 0,'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    time.sleep(20 if index else 0)
    interval['finishedUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    argv=['python3',str(r/'live-prepared/candidate_run.py'),'batch','--kind',kind,'--count',str(count),'--go',str(r/'live-go.json'),'--bundle',str(b)]
    started=datetime.datetime.now(datetime.timezone.utc).isoformat()
    z=subprocess.run(argv,capture_output=True,text=True,timeout=600)
    record={'index':index,'kind':kind,'count':count,'interval':interval,'argv':argv,'startedUtc':started,'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'exitCode':z.returncode,'stdout':z.stdout,'stderr':z.stderr}
    (commands/f'{index:02d}.json').write_text(json.dumps(record,indent=2)+'\n')
    normalized=[]
    for p in sorted(b.glob('batch-*/operation-*.json')):
        if not re.fullmatch(r'operation-[0-9]+\.json',p.name):continue
        dest=b/'safe-operations'/p.parent.name/(p.stem+'-safe.json')
        dest.parent.mkdir(parents=True,mode=0o700,exist_ok=True)
        if dest.exists():continue
        subprocess.run(['python3',str(r/'normalize_operation.py'),'--operation',str(p),'--source-commit',commit,'--output',str(dest)],check=True,stdout=subprocess.DEVNULL)
        normalized.append(json.loads(dest.read_text()))
    m=json.loads((b/'baseline102.json').read_text())
    print(json.dumps({'index':index,'kind':kind,'count':count,'exitCode':z.returncode,'acceptedIds':len(m['operationIds']),'historyIds':len(m['historyIds']),'safeOperations':normalized if z.returncode else [],'commandReceipt':str(commands/f'{index:02d}.json')}),flush=True)
    if z.returncode:
        print(json.dumps({'stoppedFirstFailure':True,'noAutomaticRetry':True}),flush=True)
        raise SystemExit(z.returncode)
print(json.dumps({'plannedFullWorkloadCompleted':True,'operations':30,'objects':31}),flush=True)
