#!/usr/bin/env python3
"""Run one observation and invalidate its result if the resident's operation set changes."""
import argparse,datetime,json,os,socket,struct,subprocess,time,hashlib
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--plan',required=True);a=p.parse_args();plan=json.loads(Path(a.plan).read_text());out=Path(plan['monitorOutput']);assert not out.exists();expected=plan['resident'];rows=[];invalid=[];started=time.monotonic();complete=False
def save():
 value={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scriptSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'resident':expected,'samplerCommand':plan['samplerCommand'],'intervalSeconds':5,'polls':rows,'invalidReasons':invalid,'complete':complete,'validQuietSample':complete and not invalid,'scope':'Read-only operation IDs/states; no payloads or operation detail exported'}
 out.write_text(json.dumps(value,indent=2)+'\n')
def poll(label):
 proc=Path('/proc')/str(expected['pid']);f=(proc/'stat').read_text().rsplit(')',1)[1].split();assert int(f[19])==expected['startTicks'] and proc.stat().st_uid==os.getuid()
 assert hashlib.sha256((proc/'exe').read_bytes()).hexdigest()==expected['sha256']
 with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as connection:
  connection.settimeout(3);connection.connect(plan['socket']);pid,uid,_=struct.unpack('3i',connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12));assert pid==expected['pid'] and uid==os.getuid()
 raw=subprocess.run([plan['cli'],'--socket',plan['socket'],'operation.recent','{}'],capture_output=True,text=True,timeout=10)
 assert raw.returncode==0;value=json.loads(raw.stdout);assert value['ok'];operations=value['result']['operations']
 states=sorted([{'id':op['id'],'state':op['state'],'copyStatus':op.get('copyStatus')} for op in operations],key=lambda x:x['id'])
 ui_raw=subprocess.run([plan['cli'],'--socket',plan['socket'],'ui.snapshot','{}'],capture_output=True,text=True,timeout=10);assert ui_raw.returncode==0;ui_value=json.loads(ui_raw.stdout);assert ui_value['ok'];ui=ui_value['result']['diagnostics']['ui']
 terminal=all(op['state'] in ['completed','cancelled','failed','partial','interrupted'] and op.get('copyStatus')!='pending' for op in states)
 if ui['pid']!=0 or ui['state']!='stopped':
  if 'UI helper became active' not in invalid:invalid.append('UI helper became active')
 if rows and ui!=rows[0]['ui'] and 'UI lifecycle changed during sample' not in invalid:invalid.append('UI lifecycle changed during sample')
 row={'label':label,'elapsedSeconds':time.monotonic()-started,'operationCount':len(states),'terminal':terminal,'operations':states,'ui':ui};rows.append(row)
 if not terminal and 'nonterminal operation observed' not in invalid:invalid.append('nonterminal operation observed')
 if len(rows)>1 and states!=rows[0]['operations'] and 'operation IDs/states changed during sample' not in invalid:invalid.append('operation IDs/states changed during sample')
 save()
poll('before');assert not invalid,'Resident must start with terminal operations'
with Path(plan['driverLog']).open('w') as log:
 child=subprocess.Popen(plan['samplerCommand'],stdout=log,stderr=subprocess.STDOUT)
 while child.poll() is None:
  time.sleep(5)
  try:poll('during')
  except Exception as error:
   invalid.append('operation guard failed: '+type(error).__name__);save()
 code=child.wait()
try:poll('after')
except Exception as error:invalid.append('final operation guard failed: '+type(error).__name__)
if code:invalid.append('sampler exit code '+str(code))
complete=True;save();raise SystemExit(0 if not invalid else 1)
