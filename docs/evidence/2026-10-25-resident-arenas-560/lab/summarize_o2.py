#!/usr/bin/env python3
"""Summarize private o-series runs (headless diagnostics only).
Revision of summarize_o.py (#579 review 4372122106): the final-270 s window uses sample timestamps and is
reported only when at least 300 s of settle were observed after the drained sample; shorter runs report
settleObservedSeconds and final values only."""
import json, re, statistics, sys
from pathlib import Path
MIB=1024**2
def arenas(smaps):
    rows=[];cur=None
    for line in smaps.splitlines():
        m=re.match(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ (\d+)\s*(.*)$',line)
        if m:cur=dict(s=int(m[1],16),e=int(m[2],16),name=m[5].strip(),rss=0,ahp=0);rows.append(cur);continue
        if cur and line.startswith('Rss:'):cur['rss']=int(line.split()[1])*1024
        if cur and line.startswith('AnonHugePages:'):cur['ahp']=int(line.split()[1])*1024
    groups={}
    for r in rows:
        if r['name']=='':groups.setdefault(r['s']&~(64*MIB-1),[]).append(r)
    heaps=[g for b,g in groups.items() if g[0]['s']==b and max(x['e'] for x in g)-b>=60*MIB]
    return {'nonMainArenaHeaps':len(heaps),'nonMainArenaHeapRss':sum(x['rss'] for g in heaps for x in g),
            'mainHeapRss':sum(r['rss'] for r in rows if r['name']=='[heap]'),
            'anonHugePages':sum(r['ahp'] for r in rows)}
def settle_window(rows):
    """Settle starts at the 'drained' sample. Report the final 270 s only after >= 300 s were observed."""
    drained=[x for x in rows if x['label']=='drained']
    settle=[x for x in rows if x['label'].startswith('settle-')]
    if not drained or not settle:return {'settleObservedSeconds':0.0}
    start=drained[-1]['elapsedSeconds'];end=settle[-1]['elapsedSeconds'];observed=round(end-start,3)
    result={'settleObservedSeconds':observed}
    if observed>=300:
        last=[x for x in settle if x['elapsedSeconds']>=end-270]
        result['last270MaxFamily']={k:max(x['family']['totals'][k] for x in last) for k in('rssBytes','pssBytes')}
        result['last270Samples']=len(last)
    return result
def main():
  for root in map(Path,sys.argv[1:]):
      r=json.loads((root/'results.json').read_text());meta=json.loads((root/'metadata.json').read_text())
      rows=r['rows'];settle=[x for x in rows if x['label'].startswith('settle-')]
      fam=lambda x:x['family']['totals']
      out={'run':root.name,'build':meta['build']['version'],'arenaEnv':meta.get('mallocArenaMaxEnv'),'thpOff':meta.get('thpOff'),'exit':r['exit'],
           'failure':[e for e in r['events'] if e['kind'] in('failure','cleanup-fallback')],
           'cleanupRemaining':[e.get('remaining') for e in r['events'] if e['kind']=='cleanup'],
           'verified':sum(e['kind']=='upload-independent-verification' for e in r['events'])}
      base=[x for x in rows if x['label']=='baseline']
      if base:out['baselineFamily']={k:fam(base[0])[k] for k in('rssBytes','pssBytes')}
      if settle:
          window=settle_window(rows)
          out.update(window)
          out['settleSamples']=len(settle);out['singleProcessThroughout']=all(x['family']['processCount']==1 for x in settle)
          out['finalFamily']={k:fam(settle[-1])[k] for k in('rssBytes','pssBytes','privateBytes')}
          out['gate40/29']={'rss':out['finalFamily']['rssBytes']<=40*MIB,'pss':out['finalFamily']['pssBytes']<=29*MIB}
          if 'last270MaxFamily' in out:
              out['gate40/29'].update(rssAllLast270=out['last270MaxFamily']['rssBytes']<=40*MIB,pssAllLast270=out['last270MaxFamily']['pssBytes']<=29*MIB)
      sm=root/'post-score-smaps.txt'
      if sm.exists():out['postScore']=arenas(sm.read_text())
      lat=[x[2] for x in r.get('callLatencies',[])]
      if lat:
          q=statistics.quantiles(lat,n=100);out['callLatencySeconds']={'count':len(lat),'p50':round(q[49],4),'p95':round(q[94],4),'p99':round(q[98],4),'max':round(max(lat),4)}
      canc=[e for e in r['events'] if e['kind']=='cancel-control']
      if canc:out['cancel']=canc[0]
      print(json.dumps(out))
if __name__=='__main__':main()
