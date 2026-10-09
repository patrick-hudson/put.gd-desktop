from pathlib import Path
import json,statistics,math,hashlib
p=Path(__file__).with_name('phases.log');rows=[]
for line in p.read_text().splitlines():
 t,pid,parent,tid,phase,edge=line.split();rows.append({'t':int(t),'pid':int(pid),'parent':int(parent),'tid':int(tid),'phase':phase,'edge':edge})
starts=[r['t'] for r in rows if r['phase']=='invoke'];groups=[]
for i,begin in enumerate(starts):
 end=starts[i+1] if i+1<len(starts) else 10**30;points={}
 for r in rows:
  if begin<=r['t']<end:points.setdefault(r['phase']+':'+r['edge'],r['t'])
 groups.append(points)
spans={'historyOpen':('history-open:begin','history-open:end'),'historyInitialize':('history-initialize:begin','history-initialize:end'),'historySave':('history-save:begin','history-save:end'),'historyCommit':('history-commit-before:point','history-commit-after:point'),'cursorSeed':('cursor-seed:begin','cursor-seed:end'),'prepare':('prepare-before:point','prepare-after:point'),'acceptedWorker':('accepted-worker:begin','accepted-worker:end'),'acquiringWorker':('acquiring-worker:begin','acquiring-worker:end'),'qtInit':('app-before:point','app-after:point'),'prewarmCreate':('prewarm-create-before:point','prewarm-cursor-before:point'),'prewarmCursor':('prewarm-cursor-before:point','prewarm-cursor-after:point'),'prewarmDestroy':('prewarm-cursor-after:point','ready-child:point'),'nativeShow':('show-before:point','show-after:point'),'nativeInit':('native-init:point','native-map-flushed:point'),'hotkeyToApp':('invoke:point','app-before:point'),'hotkeyToReady':('invoke:point','ready-child:point'),'hotkeyToMap':('invoke:point','native-map-flushed:point'),'readyToBegin':('ready-child:point','begin-child:point'),'acquiringEndToReady':('acquiring-worker:end','ready-child:point')}
def stats(v):return {'count':len(v),'medianMs':statistics.median(v),'p95Ms':sorted(v)[math.ceil(.95*len(v))-1],'maxMs':max(v)}
result={'schemaVersion':1,'unscored':True,'traceSha256':hashlib.sha256(p.read_bytes()).hexdigest(),'groups':len(groups),'warmGroups':len(groups[5:]),'spans':{}}
for name,(a,b) in spans.items():
 vals=[(g[b]-g[a])/1e6 for g in groups[5:] if a in g and b in g]
 if vals:result['spans'][name]=stats(vals)
result['relativeWarmGroups']=[{k:(v-g['invoke:point'])/1e6 for k,v in g.items()} for g in groups[5:]]
Path(__file__).with_name('summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='relativeWarmGroups'},indent=2))
