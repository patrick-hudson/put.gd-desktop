#!/usr/bin/env python3
"""Separate initial activation, repeated reopen and additional-view readiness."""
from pathlib import Path
import hashlib,json,statistics
root=Path(__file__).resolve().parent
report={'schemaVersion':1,'scope':'Post-measurement analysis of saved events only; no workload changes or app interaction','limits':['Mapped-window readiness includes observer/CLI overhead and does not prove completed pixel painting.','Small sample counts; no p95 performance certification.','Retained and split acknowledgements have different semantics; mapped readiness is the common condition.'],'builds':{}}
for mode in ['retained','split']:
 groups={'initialSettings':[],'reopenedSettings':[],'additionalViews':[],'allViewsClosed':[]};inputs=[]
 for repeat in [1,2]:
  p=root/f'{mode}-{repeat}'/'results.json';x=json.loads(p.read_text());assert x['passed']
  inputs.append({'path':str(p),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
  for event in x['events']:
   if event['kind']=='view-shown':
    group='initialSettings' if event['view']=='settings' and event['cycle']==1 else 'reopenedSettings' if event['view']=='settings' else 'additionalViews'
    groups[group].append(event['mappedReadyMilliseconds'])
   elif event['kind']=='all-views-closed':groups['allViewsClosed'].append(event['closeMilliseconds'])
 assert {k:len(v) for k,v in groups.items()}=={'initialSettings':2,'reopenedSettings':4,'additionalViews':12,'allViewsClosed':6}
 report['builds'][mode]={'inputs':inputs,'groups':{k:{'count':len(v),'milliseconds':v,'medianMilliseconds':statistics.median(v),'maximumMilliseconds':max(v)} for k,v in groups.items()}}
p=root/'latency-breakdown.json';assert not p.exists();p.write_text(json.dumps(report,indent=2)+'\n');assert json.loads(p.read_text())==report
print(json.dumps({mode:{group:values['medianMilliseconds'] for group,values in r['groups'].items()} for mode,r in report['builds'].items()}))
