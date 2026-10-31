"""Replay saved P6 numeric evidence; performs no app, desktop or network operations."""
from pathlib import Path
import json,hashlib,sys
root=Path(sys.argv[1]);report=json.loads((root/'result.json').read_text());resident=report['resident'];observed=[]
for n in (1,2):
 data=json.loads((root/f'idle-{n}-results.json').read_text());rows=[json.loads(x) for x in (root/f'idle-{n}-samples.jsonl').read_text().splitlines()];assert data['passed'];assert data['residentIdentity']==resident;assert data['sourceRevision']==report['build']['gitCommit']
 assert data['harnessSha256']==hashlib.sha256((root/'bench_ui_process.py').read_bytes()).hexdigest()
 event=next(e for e in data['events'] if e['kind']=='diagnostics-identity-checked');assert event['trayAvailable'] is True and event['ui']['pid']==0 and event['ui']['lastExit']['code']==0
 assert len(rows)>=61;elapsed=rows[-1]['elapsedSeconds']-rows[0]['elapsedSeconds'];assert elapsed>=300
 assert max(b['elapsedSeconds']-a['elapsedSeconds'] for a,b in zip(rows,rows[1:]))<=7.5
 assert all(row['processCount']==1 and row['uiProcessCount']==0 and row['escapedProcessCount']==0 for row in rows)
 assert all(row['processes'][0]['pid']==resident['pid'] and row['processes'][0]['startTicks']==resident['startTicks'] for row in rows)
 rss=max(row['totals']['rssBytes'] for row in rows)/1048576;assert rss<=40
 assert all(p['swapBytes']==0 for row in rows for p in row['processes'])
 ticks=report['clockTicksPerSecond'];assert ticks>0
 cpu=(sum(p['cpuTicks'] for p in rows[-1]['processes'])-sum(p['cpuTicks'] for p in rows[0]['processes']))/ticks/elapsed*100;assert cpu<.5
 claimed=report['windows'][n-1];assert abs(claimed['seconds']-elapsed)<1e-9 and abs(claimed['rssPeakMiB']-rss)<1e-9 and abs(claimed['cpuPercentOneCore']-cpu)<1e-9
 observed.append({'window':n,'seconds':elapsed,'rssPeakMiB':rss,'cpuPercentOneCore':cpu})
w=json.loads((root/'continuous-watch.json').read_text());assert w['passed'] and w['resident']==resident
continuous=[json.loads(line) for line in (root/'continuous-samples.jsonl').read_text().splitlines()]
for label in ['idle-1','idle-2']:
 rows=[x for x in continuous if x['phase']==label];assert len(rows)>=1000
 assert all(x['sample']['totals']['rssBytes']<=40*1048576 and x['sample']['processCount']==1 and x['sample']['escapedProcessCount']==0 and not x['productHelpers'] for x in rows)
 assert all(p['swapBytes']==0 and p['pid']==resident['pid'] and p['startTicks']==resident['startTicks'] for x in rows for p in x['sample']['processes'])
for label in ['idle-1','idle-2']:
 x=w['windows'][label];assert x['samples']>=1000 and x['maxRssBytes']<=40*1048576 and x['maxSwapBytes']==0 and x['maxProcessCount']==1 and x['maxEscaped']==0 and not x['anyProductHelper']
assert report['passed'] and report['functionalPassed'] and report['sameUsedResidentForBothWindows']
print(json.dumps({'passed':True,'resident':resident,'windows':observed}))
