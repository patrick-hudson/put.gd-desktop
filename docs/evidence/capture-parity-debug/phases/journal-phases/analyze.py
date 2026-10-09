from pathlib import Path
import json,statistics,hashlib,math
p=Path(__file__).with_name('phases.log');rows=[]
for line in p.read_text().splitlines():
 t,pid,parent,tid,phase,edge=line.split();rows.append({'t':int(t),'pid':int(pid),'tid':int(tid),'phase':phase,'edge':edge})
rows.sort(key=lambda r:r['t']);groups=[]
for begin in [r for r in rows if r['phase']=='accepted-worker' and r['edge']=='begin']:
 end=next((r['t'] for r in rows if r['tid']==begin['tid'] and r['phase']=='accepted-worker' and r['edge']=='end' and r['t']>=begin['t']),None)
 if end is None:continue
 points={}
 for r in rows:
  if r['tid']==begin['tid'] and begin['t']<=r['t']<=end:points.setdefault(r['phase']+':'+r['edge'],r['t'])
 groups.append(points)
spans={'acceptedWorker':('accepted-worker:begin','accepted-worker:end'),'constructorAndInitialize':('history-open-initialize:begin','history-open-initialize:end'),'databaseOpen':('database-open-before:point','database-open-after:point'),'schemaRead':('history-schema:begin','history-schema:end'),'save':('history-save:begin','history-save:end'),'commit':('history-commit-before:point','history-commit-after:point')}
result={'schemaVersion':1,'scope':'unscored fixed-label accepted-worker attribution; matched by numeric thread identity, first point per phase','traceSha256':hashlib.sha256(p.read_bytes()).hexdigest(),'acceptedWorkers':len(groups),'warmSamples':max(0,len(groups)-5),'p95Definition':'nearest-rank ceil(0.95*n)-1,29th of30','spans':{},'samples':[]}
for g in groups[5:]:result['samples'].append({name:(g[b]-g[a])/1e6 for name,(a,b) in spans.items() if a in g and b in g})
for name in spans:
 vals=[r[name] for r in result['samples'] if name in r]
 if vals:result['spans'][name]={'count':len(vals),'medianMs':statistics.median(vals),'p95Ms':sorted(vals)[math.ceil(.95*len(vals))-1],'maxMs':max(vals)}
Path(__file__).with_name('summary.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='samples'},indent=2))
