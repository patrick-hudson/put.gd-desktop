#!/usr/bin/env python3
"""Assert a normal helper exit or one exact newly accepted native operation."""
import argparse,json,time
from common import OUT,RUN,now,save,load_lab,assert_resident,ui,wait_ui,helper_identity
p=argparse.ArgumentParser();p.add_argument('mode',choices=['closed','operation']);p.add_argument('--name',required=True);p.add_argument('--keys',nargs='+');p.add_argument('--expected-state',choices=['completed','cancelled','failed']);p.add_argument('--expected-error');a=p.parse_args()
assert a.name.replace('-','').isalnum();output=OUT/(a.name+'.json');assert not output.exists()
lab=load_lab();expected=assert_resident(lab);receipt={'timestampUtc':now(),'mode':a.mode,'resident':expected,'ok':False}
try:
 if a.mode=='closed':
  state=wait_ui(lab,'stopped');assert state['pid']==0 and state['lastExit']['status']=='normal' and state['lastExit']['code']==0
  residue=json.loads(lab.ssh(RUN,"python3 - <<'PY2'\nfrom pathlib import Path\nimport os,json\nfound=[]\nfor p in Path('/proc').iterdir():\n try:\n  if p.name.isdecimal() and p.stat().st_uid==os.getuid() and (p/'comm').read_text().strip()=='putgd-ui':found.append(int(p.name))\n except (FileNotFoundError,ProcessLookupError):pass\nprint(json.dumps(found))\nPY2\n"))
  assert not residue
  receipt.update(ui=state,remainingHelpers=residue)
 else:
  assert a.keys and a.expected_state
  before=lab.cli(RUN,'operation.recent')['operations'];ids={x['id'] for x in before};receipt['beforeOperationIds']=sorted(ids)
  receipt['uiBefore']=ui(lab);receipt['keys']=a.keys;lab.key(RUN,a.keys)
  deadline=time.monotonic()+20
  while True:
   recent=lab.cli(RUN,'operation.recent')['operations'];new=[x for x in recent if x['id'] not in ids];assert len(new)<=1
   if new:
    op=lab.cli(RUN,'operation.inspect',{'id':new[0]['id']})
    receipt['operation']=op
    if op['state'] in ['completed','cancelled','failed','partial','interrupted']:break
   if time.monotonic()>=deadline:raise RuntimeError('New native operation did not settle')
   time.sleep(.2)
  assert op['state']==a.expected_state
  if a.expected_error:
   assert op.get('error',{}).get('code')==a.expected_error,op
   assert op['items']==[] and op.get('copyStatus') in ['no_result','suppressed']
  receipt['uiAfter']=ui(lab)
 assert assert_resident(lab)==expected;receipt['ok']=True
except Exception as error:receipt['error']=str(error);raise
finally:receipt['finishedUtc']=now();save(output,receipt)
print(json.dumps(receipt))
