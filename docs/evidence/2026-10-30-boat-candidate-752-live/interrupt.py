import json,os,signal,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parent
source=Path('/home/patrick/.t3/worktrees/put.gd/t3-c14e8bd4')
sha=(root/'nightly-sha.txt').read_text().strip()
argv=['python3',str(source/'tools/lab/boat/candidate.py'),sha,'--output',str(root/'interrupted')]
env=dict(os.environ,PATH=str(root/'bin')+os.pathsep+os.environ['PATH'])
with (root/'interrupted-controller.log').open('w') as log:
 p=subprocess.Popen(argv,stdout=log,stderr=subprocess.STDOUT,env=env)
 meta={'argv':argv,'pid':p.pid,'startedAt':time.time()}
 deadline=time.monotonic()+60
 while time.monotonic()<deadline:
  events=[json.loads(s) for s in (root/'commands.jsonl').read_text().splitlines()]
  new=[e for e in events if e['controller']==p.pid and 'new' in e['argv']]
  if new:
   meta['newCommandsAtSignal']=new
   meta['creationAtSignal']={str(f.relative_to(root)):f.read_text() for f in (root/'interrupted').glob('*/creation.jsonl')}
   meta['signalAt']=time.time();p.send_signal(signal.SIGINT);break
  if p.poll() is not None:raise SystemExit('controller exited before new')
  time.sleep(.01)
 else:
  p.send_signal(signal.SIGTERM);raise SystemExit('no new within 60s')
 (root/'interruption.json').write_text(json.dumps(meta,indent=2)+'\n')
 print('SIGINT during boat new',p.pid,'visible creates',len(new),flush=True)
 deadline=time.monotonic()+400
 while p.poll() is None:
  if time.monotonic()>deadline:raise SystemExit('interrupted cleanup exceeded hard timeout')
  print(time.strftime('%H:%M:%S'),'interrupted controller cleaning up',flush=True)
  time.sleep(10)
 meta.update(returncode=p.returncode,finishedAt=time.time())
 (root/'interruption.json').write_text(json.dumps(meta,indent=2)+'\n')
 print('interrupted return',p.returncode,flush=True)
