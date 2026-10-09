import json,os,pathlib,subprocess,time
root=pathlib.Path.home()/'putgd-lab/u11-continuity'
p=json.loads((root/'parameters.json').read_text());before=set(p['operationIdsBefore']);pid=p['helperPid']
started=time.monotonic();observations=[];last=None;failure=None
def cli(method,params=None):
 r=subprocess.run(['putgd-cli',method,json.dumps(params or {})],capture_output=True,text=True,timeout=5)
 d=json.loads(r.stdout)
 if r.returncode or not d.get('ok'):raise RuntimeError('Observer CLI rejected '+method)
 return d['result']
try:
 while time.monotonic()-started<60:
  goneBefore=not pathlib.Path('/proc',str(pid)).exists()
  ops=[o for o in cli('operation.recent')['operations'] if o['id'] not in before]
  if len(ops)>1:raise RuntimeError('Unexpected extra native operation')
  if ops:
   last=cli('operation.inspect',{'id':ops[0]['id']})
   row={'monotonicSeconds':time.monotonic(),'helperGoneBeforeInspect':goneBefore,
        'operationId':last['id'],'state':last['state'],'copyStatus':last['copyStatus']}
   observations.append(row)
   with (root/'observations.jsonl').open('a') as stream:stream.write(json.dumps(row)+'\n')
   if goneBefore and last['state'] in ('completed','failed','partial','cancelled') and last['copyStatus']!='pending':break
  time.sleep(.03)
except Exception as e:failure=str(e)
finally:
 activeAfter=any(o['helperGoneBeforeInspect'] and o['state'] not in ('completed','failed','partial','cancelled','interrupted') for o in observations)
 (root/'result.json').write_text(json.dumps({'observations':observations,'operation':last,'failure':failure,
  'activeOperationObservedAfterHelperExit':activeAfter,'elapsedSeconds':time.monotonic()-started},indent=2)+'\n')
