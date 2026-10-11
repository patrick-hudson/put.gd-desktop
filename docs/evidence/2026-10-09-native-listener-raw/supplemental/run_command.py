#!/usr/bin/env python3
"""Run one explicit normal-tool command with immutable structured receipts."""
import argparse,datetime,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True);p.add_argument('--cwd',required=True);p.add_argument('argv',nargs=argparse.REMAINDER);a=p.parse_args();v=a.argv[1:] if a.argv[:1]==['--'] else a.argv
assert v
out=Path(a.output);out.mkdir(parents=True,exist_ok=False)
now=lambda:datetime.datetime.now(datetime.timezone.utc).isoformat()
(out/'command.json').write_text(json.dumps({'argv':v,'cwd':a.cwd,'startedUtc':now()},indent=2)+'\n')
with (out/'stdout').open('x') as s,(out/'stderr').open('x') as e:r=subprocess.run(v,cwd=a.cwd,stdout=s,stderr=e)
(out/'result.json').write_text(json.dumps({'exitCode':r.returncode,'finishedUtc':now()},indent=2)+'\n');print(json.dumps({'receipt':str(out),'exitCode':r.returncode}));raise SystemExit(r.returncode)
