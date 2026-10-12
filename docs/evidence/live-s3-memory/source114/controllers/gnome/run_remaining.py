#!/usr/bin/env python3
"""Recorded serial continuation after the first successful live text upload."""
import argparse,datetime,json,subprocess,time
from pathlib import Path
base=Path(__file__).resolve().parent
ap=argparse.ArgumentParser();ap.add_argument('--bundle',type=Path,required=True);ap.add_argument('--go',type=Path,required=True);a=ap.parse_args();root=a.bundle.resolve();m=json.loads((root/'baseline102.json').read_text());assert len(m['operationIds'])==len(m['historyIds'])==1
assert not (root/'remaining-command.json').exists()
sequence=[('clipboard-text',5),('clipboard-text',5),('region',5),('region',5),('region',5),('clipboard-image',1),('clipboard-files',1),('window',1),('desktop',1)]
commands=[['python3',str(base/'live-prepared/candidate_run.py'),'batch','--go',str(a.go.resolve()),'--bundle',str(root),'--kind',kind,'--count',str(count)] for kind,count in sequence]
(root/'remaining-command.json').write_text(json.dumps({'argv':commands,'betweenBatchesIdleSeconds':20,'initialOperationId':m['operationIds'][0],'workloadCounts':'Same30operations/31objects as baseline; intervals explicit, final300-second quiet unchanged'},indent=2)+'\n')
for argv in commands:
 time.sleep(20);subprocess.run(argv,check=True)
for phase in ('pixels','cleanup-prepare','cleanup-review','finalize'):
 subprocess.run(['python3',str(base/'live-prepared/candidate_run.py'),phase,'--go',str(a.go.resolve()),'--bundle',str(root)],check=True)
(root/'quiet-start.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'minimumSecondsBeforeFinish':310,'noActionsHistoryConfigurationOrViewsUntilObserverStops':True},indent=2)+'\n')
print(json.dumps({'quietStarted':True,'bundle':str(root)}))
