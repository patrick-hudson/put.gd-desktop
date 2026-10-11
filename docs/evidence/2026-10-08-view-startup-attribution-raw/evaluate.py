#!/usr/bin/env python3
"""Aggregate the frozen production family workload; never contacts a display."""
import argparse, hashlib, json, statistics
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--result', action='append', required=True)
a=p.parse_args(); assert len(a.result)==2, 'two independent confirmation runs required'
latency=[];rss=[];pss=[];peak=[];inputs=[]
for value in a.result:
 path=Path(value); x=json.loads(path.read_text()); assert x['passed']
 assert x['harnessSha256']=='a28c94b075036ea65f8eca8e068d480cff46ee959d4680593f5c4d55cce3fc40'
 assert x['completeViewCycles']==3 and x['allViews']==['settings','history','activity']
 events=[e['mappedReadyMilliseconds'] for e in x['events'] if e['kind']=='view-shown' and e['view']=='settings' and e['cycle']>1]
 assert len(events)==2
 latency.extend(events);s=x['summary']
 rss.append(s['finalClosedTotals']['rssBytes']);pss.append(s['finalClosedTotals']['pssBytes']);peak.append(s['peakOpenTotals']['pssBytes'])
 inputs.append({'path':str(path.resolve()),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'binaries':x['binaries'],'sourceRevision':x['sourceRevision']})
print(json.dumps({'settingsReopenMilliseconds':statistics.median(latency),'closedRssBytes':statistics.median(rss),'closedPssBytes':statistics.median(pss),'peakOpenPssBytes':max(peak),'functionalPassed':1,'normalHelperExitPassed':1,'reopenSamples':latency,'inputs':inputs,'limits':['Mapped readiness includes observer overhead; no p95 or paint certification.','functionalPassed reports workload success only; current-source native receipts must be verified separately.']}))
