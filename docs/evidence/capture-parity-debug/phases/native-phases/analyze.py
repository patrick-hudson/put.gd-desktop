from pathlib import Path
import json,statistics,hashlib
p=Path(__file__).with_name('phases.log');rows=[]
for line in p.read_text().splitlines():
 t,pid,parent,phase,edge=line.split();rows.append({'t':int(t),'pid':int(pid),'parent':int(parent),'phase':phase,'edge':edge})
starts=[r['t'] for r in rows if r['phase']=='invoke'];groups=[]
for i,begin in enumerate(starts):
 end=starts[i+1] if i+1<len(starts) else 10**30;points={}
 for r in rows:
  if begin<=r['t']<end:points.setdefault(r['phase']+':'+r['edge'],r['t'])
 groups.append(points)
spans={'acceptedWorker':('accepted-worker:begin','accepted-worker:end'),'acquiringWorker':('acquiring-worker:begin','acquiring-worker:end'),'acceptedDelivery':('accepted-queue:point','accepted-deliver:point'),'qtInit':('app-before:point','app-after:point'),'postQtAdapters':('app-after:point','adapters-after:point'),'resolve':('resolve-before:point','resolve-after:point'),'cursor':('cursor-before:point','cursor-after:point'),'nativeWindowCreate':('winid-before:point','winid-after:point'),'show':('show-before:point','show-after:point'),'managedExposeWait':('show-after:point','exposed-queued:point'),'nativeInit':('native-init:point','native-map-flushed:point'),'hotkeyToQt':('invoke:point','app-before:point'),'hotkeyToNativeMap':('invoke:point','native-map-flushed:point')}
result={'schemaVersion':1,'scope':'unscored fixed-label monotonic phase attribution; first point per phase; unchanged35 invocation workload','traceSha256':hashlib.sha256(p.read_bytes()).hexdigest(),'groups':len(groups),'scoredWarmGroups':max(0,len(groups)-5),'spans':{}}
for name,(a,b) in spans.items():
 vals=[(g[b]-g[a])/1e6 for g in groups[5:] if a in g and b in g]
 if vals:result['spans'][name]={'count':len(vals),'medianMs':statistics.median(vals),'p95Ms':sorted(vals)[max(0,int(.95*len(vals))-1)],'maxMs':max(vals)}
Path(__file__).with_name('summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
