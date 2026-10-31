#!/usr/bin/env python3
"""Read saved results without modifying them; apply approved endpoint policies."""
import json,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def read(name):return json.loads((ROOT/name).read_text())
def quantiles(rows,key):
 values=sorted(row[key] for row in rows)
 assert values and all(isinstance(x,(int,float)) and math.isfinite(x) and x>=0 for x in values)
 return {'samples':len(values),'p50Ms':values[(len(values)-1)//2],'p95Ms':values[math.ceil(.95*len(values))-1],'maxMs':max(values)}
def bound(value,limit,unit='ms'):
 return {'value':value,'maximum':limit,'unit':unit,'status':'pass' if value<=limit else 'fail'}
def main():
 result={'schemaVersion':1,'testedAppSource':'4d3840c6108c22546d83c43a1b7d4c77b1df9d6d','scope':'Saved physical m74 published-nightly measurements; observer overhead included; no stable-asset certification','policyCorrections':[{'raw':'raw/m74/b3/*/results.json:gates.overlayPaint','reason':'Supplemental wrapper mistakenly used max(drawable,root). Frozen #379 gate is drawable p95 <=150ms. Root observations are retained and ungraded; raw receipts are never rewritten.'}],'capture':{}}
 for cohort in ('capture-empty','capture-10k','links'):
  d=read('raw/m74/b3/'+cohort+'/results.json');m=d['metrics']['nativeCapture'];rows=m['raw'];assert len(rows)==35 and all(r['warmup']==(i<5) for i,r in enumerate(rows));warm=rows[5:]
  paint=quantiles(warm,'drawablePaintMs');assert paint==m['drawablePaint']
  entry={'source':'raw/m74/b3/'+cohort+'/results.json','drawablePaint':paint,'drawableGate':bound(paint['p95Ms'],150),'rootObservedUngraded':quantiles(warm,'rootObservedPaintMs')}
  if cohort=='links':entry['captureToLinkUngraded']=quantiles(warm,'captureToLinkMs')
  result['capture'][cohort]=entry
 h=read('raw/m74/b3/history-10k/results.json')['metrics']['historyContent']
 result['history']={'source':'raw/m74/b3/history-10k/results.json','open':quantiles(h['openRaw'],'milliseconds'),'search':quantiles(h['searchRaw'],'milliseconds'),'scroll':quantiles(h['scrollRaw'],'milliseconds'),'traversedRows':h['traversedRows'],'pages':h['pages'],'traversalSeconds':h['traversalSeconds']}
 assert result['history']['open']['samples']==20 and result['history']['search']['samples']==30 and result['history']['scroll']['samples']==40 and h['traversedRows']==10000 and h['pages']==200
 result['history']['gates']={'openMax':bound(result['history']['open']['maxMs'],1000),'searchP95':bound(result['history']['search']['p95Ms'],250),'scrollP95':bound(result['history']['scroll']['p95Ms'],100)}
 for phase in ('slow',):
  d=read('raw/m74/b2/'+phase+'/results.json');result['slow256']={'status':d['status'],'metrics':d['metrics'],'gates':d['gates'],'source':'raw/m74/b2/'+phase+'/results.json','limitation':'UI observer saw generic drawable change, not content-specific first presentation; response/cancel/settle growth have no approved threshold'}
 d=read('raw/m74/b3/slow100/results.json');result['slow100']={'status':d['status'],'metrics':d['metrics'],'gates':d['gates'],'source':'raw/m74/b3/slow100/results.json','fixturePort':d['tcFixtureScope']['fixturePort'],'rateBytesPerSecond':d['tcFixtureScope']['rateBytesPerSecond'],'qdiscRestored':d['tcFixtureScope'].get('qdiscRestored'),'ownedTableAbsent':d['tcFixtureScope'].get('ownedTableAbsent')}
 d=read('raw/m74/b2/memory-10k/results.json');result['memory']={'metrics':d['metrics'],'gates':d['gates'],'source':'raw/m74/b2/memory-10k/results.json'}
 for phase in ('native-burst','history-functionals'):
  p=ROOT/'raw/m74/b4'/phase/'results.json'
  if p.exists():
   d=json.loads(p.read_text());result[phase]={'source':str(p.relative_to(ROOT)),'status':d['status'],'metrics':d['metrics'],'gates':d['gates'],'error':d.get('error')}
 result['overall']='blocked: native admission/browser gaps, 10k capture and scroll threshold failures, and populated functional gaps'
 events=[json.loads(line) for line in (ROOT/'raw/m74/b4/history-functionals/events.jsonl').read_text().splitlines()]
 previews=[r for r in events if r['kind']=='retained-preview-pixels-observed']
 result['partialImagePreview']={'source':'raw/m74/b4/history-functionals/events.jsonl','summary':quantiles(previews,'upperBoundMs'),'imageOnly':True,'scope':'ten warm reopens of one fixture image; video/copy/delete not reached; overall populated functional gate remains blocked','p95SubsetGate':bound(quantiles(previews,'upperBoundMs')['p95Ms'],1000)}
 result['historyStressSampledTelemetry']=read('raw/m74/b3/history-10k/results.json')['metrics'].get('sampledProcessFamily')
 result['historyStressTelemetryLimitation']='Mapping/process-family samples are lower bounds, not continuous CPU/memory observation of every full traversal step. Separate 10k open/closed memory gate is authoritative within its own scope.'
 return result
if __name__=='__main__':
 print(json.dumps(main(),indent=2))
