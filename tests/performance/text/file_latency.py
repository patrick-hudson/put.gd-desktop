#!/usr/bin/env python3
"""Measure file-source recognition and clipboard acknowledgement on a private resident.
Manual lab receipt, no automatic acceptance verdict. Run under a held desktop
lease with synthetic/public images; this explicitly copies to that clipboard.
Export PUTGD_TEXT_LAB_SESSION to match the owned, freshly heartbeating lease.
"""
import argparse,hashlib,json,math,time,os,subprocess
from datetime import datetime,timezone
from pathlib import Path
from workload import validate_socket,identity,call


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli',required=True);p.add_argument('--pid',type=int,required=True)
    p.add_argument('--socket',type=Path,required=True);p.add_argument('--file',type=Path,required=True)
    p.add_argument('--runs',type=int,default=30);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.runs<1:p.error('runs must be positive')
    original=identity(a.pid);path=validate_socket(a.socket,a.pid);image=a.file.resolve(strict=True)
    receipt=dict(status='blocked',source='file',image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),rows=[],assessment='Manual comparison with issue bars; file dispatch replaces selection end.')
    with a.output.open('x') as out:
        try:
            receipt['resident']=dict(pid=original[0],startTicks=original[1],exe_sha256=hashlib.sha256((Path('/proc')/str(a.pid)/'exe').read_bytes()).hexdigest())
            receipt['cli_build']=json.loads(subprocess.check_output([a.cli,'--version-json'],text=True,timeout=5))
            for i in range(a.runs):
                fields=dict(line.split(': ',1) for line in (Path.home()/'putgd-lab/locks/desktop.lock').read_text().splitlines() if ': ' in line)
                now=datetime.now(timezone.utc)
                if not os.environ.get('PUTGD_TEXT_LAB_SESSION') or fields.get('session')!=os.environ['PUTGD_TEXT_LAB_SESSION']:raise RuntimeError('desktop lease not owned')
                if datetime.fromisoformat(fields['expires'].replace('Z','+00:00'))<=now or (now-datetime.fromisoformat(fields['heartbeat'].replace('Z','+00:00'))).total_seconds()>300:raise RuntimeError('lease/heartbeat expired')
                if identity(a.pid)!=original:raise RuntimeError('resident changed')
                start=time.monotonic_ns();op=call(a.cli,path,'text.recognize',dict(source='file',path=str(image),copy=True))['operationId']
                deadline=time.monotonic()+30
                while True:
                    v=call(a.cli,path,'text.inspect',dict(id=op))
                    if v['state'] in ('completed','cancelled','failed'):break
                    if time.monotonic()>deadline:
                        call(a.cli,path,'text.cancel',dict(id=op));raise RuntimeError('job did not finish')
                    time.sleep(.05)
                if v['state']!='completed' or v.get('copyStatus')!='copied':raise RuntimeError('recognition/copy acknowledgement missing')
                timings=v['timings']
                for key in ('spawnMs','engineStartMs','recognizeMs','copyMs','totalMs'):
                    if type(timings.get(key))!=int or timings[key]<0:raise RuntimeError('missing phase '+key)
                receipt['rows'].append(dict(operationId=op,timings=timings,dispatch_to_observed_ack_ms=(time.monotonic_ns()-start)/1e6,characterCount=v['characterCount'],truncated=v['truncated']))
            receipt['p95']={key:sorted(r['timings'][key] for r in receipt['rows'])[math.ceil(.95*a.runs)-1] for key in receipt['rows'][0]['timings']}
            receipt['observed_ack_p95_ms']=sorted(r['dispatch_to_observed_ack_ms'] for r in receipt['rows'])[math.ceil(.95*a.runs)-1]
            receipt['observation_note']='Client duration includes CLI calls and up to50ms polling; server timings carry the actual clipboard acknowledgement.'
            receipt['status']='measured'
        except Exception as e:receipt['reason']=str(e)
        json.dump(receipt,out,indent=2)
    return 0 if receipt['status']=='measured' else 2


if __name__=='__main__':raise SystemExit(main())
