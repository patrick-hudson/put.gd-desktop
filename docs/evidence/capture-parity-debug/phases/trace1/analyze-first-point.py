import json,collections,statistics,sys
from pathlib import Path
rows=[]
for line in Path(sys.argv[1]).read_text().splitlines():
 t,p,pp,phase,edge=line.split();rows.append((int(t),int(p),int(pp),phase,edge))
groups=collections.defaultdict(list)
for row in rows:groups[row[1]].append(row)
stages=collections.defaultdict(list)
for pid,g in groups.items():
 pairs={}
 for t,_,_,phase,edge in g:
  if edge=='begin':pairs[phase]=t
  elif edge=='end' and phase in pairs:stages[phase].append((t-pairs.pop(phase))/1e6)
 stamps={}
 for t,_,_,phase,edge in g:
  if edge=='point':stamps.setdefault(phase,t)
 if 'child-main' in stamps:
  for a,b in [('child-main','app-before'),('app-before','app-after'),('adapters-before','adapters-after'),('app-after','resolve-before'),('resolve-before','resolve-after'),('resolve-after','acquire-frozen'),('acquire-frozen','surface-first-paint')]:
   if a in stamps and b in stamps:stages[a+' -> '+b].append((stamps[b]-stamps[a])/1e6)
summary={s:{'n':len(v),'medianMs':statistics.median(v),'p95Ms':sorted(v)[max(0,int(len(v)*.95+.999999)-1)],'minMs':min(v),'maxMs':max(v)} for s,v in stages.items()}
print(json.dumps(summary,indent=2))
