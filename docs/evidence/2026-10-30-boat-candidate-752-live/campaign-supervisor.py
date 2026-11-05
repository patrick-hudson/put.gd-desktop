import hashlib,json,os,signal,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parent
source=Path('/home/patrick/.t3/worktrees/put.gd/t3-c14e8bd4')
sha=(root/'nightly-sha.txt').read_text().strip()
env=dict(os.environ,PATH=str(root/'bin')+os.pathsep+os.environ['PATH'])
processes=[]
meta={'source':sha,'controllerCommit':subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),'controllerSha256':hashlib.sha256((source/'tools/lab/boat/candidate.py').read_bytes()).hexdigest(),'runs':[],'startedAt':time.time()}
for name in ['run-a','run-b']:
 argv=['python3',str(source/'tools/lab/boat/candidate.py'),sha,'--output',str(root/name)]
 log=(root/f'{name}-controller.log').open('w')
 p=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,env=env)
 processes.append((name,p,log));meta['runs'].append({'name':name,'pid':p.pid,'argv':argv})
(root/'concurrent.json').write_text(json.dumps(meta,indent=2)+'\n')
deadline=time.monotonic()+11200
while True:
 listing=subprocess.run(['boat','--no-update','--json','list','--all'],capture_output=True,text=True,timeout=120)
 event={'time':time.time(),'returncode':listing.returncode}
 if not listing.returncode:
  x=json.loads(listing.stdout)
  event.update(pageInfo=x.get('pageInfo'),sandboxes=[{k:i.get(k) for k in ['id','name','state','createdAt','ttlSeconds','hydrated']} for i in x['sandboxes'] if (i.get('name') or '').startswith('putgd-'+sha[:12])],unrelatedCount=sum(not (i.get('name') or '').startswith('putgd-'+sha[:12]) for i in x['sandboxes']))
 else:event['error']=listing.stderr
 with (root/'observations.jsonl').open('a') as stream:stream.write(json.dumps(event)+'\n')
 status={name:p.poll() for name,p,_ in processes}
 print(time.strftime('%H:%M:%S'),status,'owned visible',len(event.get('sandboxes',[])),flush=True)
 if all(code is not None for code in status.values()):break
 if time.monotonic()>deadline:
  for _,p,_ in processes:
   if p.poll() is None:p.send_signal(signal.SIGTERM)
  raise SystemExit('campaign hard timeout; cancellation sent')
 time.sleep(15)
for name,p,log in processes:
 log.close()
 entry=next(i for i in meta['runs'] if i['name']==name);entry['returncode']=p.returncode
meta['finishedAt']=time.time()
(root/'concurrent.json').write_text(json.dumps(meta,indent=2)+'\n')
