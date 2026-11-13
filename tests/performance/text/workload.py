#!/usr/bin/env python3
"""Drive #1428's private-resident workload; never starts a resident itself.

Requires an owned private XDG runtime, a synthetic covering desktop, a held
lab lease, and an external region driver (argv program, no shell). The driver
receives operationId and must visibly select a region. Helper lifecycle events
must come from the exact candidate's observer as JSONL: {pid,startTicks,event,
exitCode?}; event is spawn or exit. Missing lifecycle evidence blocks completion
of the measurement. A person assesses the receipt against the plan's bars.
No recognized payload is retained in this receipt; there is no acceptance verdict.
"""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import stat
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

# libgomp is legitimate through Qt Core -> libb2 on several targets (#1428).
EXTRA = re.compile(r'lib(?:tesseract|lept(?:onica)?|ZXing|curl|archive)(?:\.|-)')


def identity(pid):
    p = Path('/proc') / str(pid)
    if p.stat().st_uid != os.getuid():
        raise RuntimeError('resident is not owned')
    return (pid, (p/'stat').read_text().rsplit(') ',1)[1].split()[19])


def validate_socket(path, pid):
    path = path.resolve()
    if len(str(path).encode()) > 107:
        raise RuntimeError('Unix socket exceeds 107 UTF-8 bytes')
    if path.name != 'session.sock' or path.parent.name != 'putgd':
        raise RuntimeError('expected putgd/session.sock suffix')
    runtime = path.parent.parent
    info = runtime.stat()
    if info.st_uid != os.getuid() or info.st_mode & 0o077 or not stat.S_ISDIR(info.st_mode):
        raise RuntimeError('runtime must be owned and private')
    env = (Path('/proc')/str(pid)/'environ').read_bytes().split(b'\0')
    if b'XDG_RUNTIME_DIR='+str(runtime).encode() not in env:
        raise RuntimeError('resident does not use supplied private runtime')
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.connect(str(path))
        got, uid, _ = struct.unpack('3i',peer.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
        if got != pid or uid != os.getuid(): raise RuntimeError('socket peer mismatch')
    return path


def call(cli, path, method, params):
    run = subprocess.run([cli,'--socket',str(path),'--params-stdin',method],
                         input=json.dumps(params),capture_output=True,text=True,timeout=30)
    # Only fixed error codes and metadata survive; stdout may contain screen data.
    response = json.loads(run.stdout)
    if not response.get('ok'):
        raise RuntimeError('method unavailable or failed: '+response.get('error',{}).get('code','unknown'))
    return response['result']


def lifecycle(path):
    events = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    spawned, exited = {}, {}
    for e in events:
        key=(e['pid'],str(e['startTicks']))
        if e['event']=='spawn': spawned[key]=e
        elif e['event']=='exit': exited[key]=e
    if len(spawned) != 25 or spawned.keys() != exited.keys():
        raise RuntimeError('need paired spawn/exit evidence for exactly 25 helpers')
    if any(e.get('exitCode') != 0 for e in exited.values()):
        raise RuntimeError('a helper did not exit successfully')
    return [dict(pid=k[0],startTicks=k[1],exitCode=exited[k]['exitCode']) for k in spawned]


def run(a, receipt):
    original = identity(a.pid)
    path = validate_socket(a.socket, a.pid)
    receipt.update(resident=dict(pid=original[0],startTicks=original[1]),socket_bytes=len(str(path).encode()),
                   build=json.loads(subprocess.check_output([a.cli,'--version-json'],text=True,timeout=5)))
    receipt['capabilities']=call(a.cli,path,'capabilities',{})
    for index in range(25):
        if identity(a.pid) != original: raise RuntimeError('resident changed')
        if index < 20:
            response=call(a.cli,path,'text.recognize',dict(source='monitor',copy=False))
        else:
            response=call(a.cli,path,'action.invoke',dict(action='text'))
        op=response['operationId']
        record=dict(index=index,operationId=op,source='monitor' if index<20 else 'region')
        receipt['jobs'].append(record)
        if index >= 20:
            subprocess.run([a.region_driver,op],check=True,timeout=20)
        deadline=time.monotonic()+25
        while True:
            state=call(a.cli,path,'text.inspect',dict(id=op))
            record['state']=state['state']
            if state['state'] in ('completed','failed','cancelled'):
                if state['state']!='completed': raise RuntimeError('text job did not complete')
                break
            if time.monotonic()>deadline:
                call(a.cli,path,'text.cancel',dict(id=op))
                raise RuntimeError('text job timed out')
            time.sleep(.1)
    # Standalone CLI can wait here; the interactive caller should run asynchronously.
    receipt['idle_wait_seconds']=180
    time.sleep(180)
    if identity(a.pid)!=original: raise RuntimeError('resident changed during wait')
    proc=Path('/proc')/str(a.pid)
    names=sorted({line.split(None,5)[5].rsplit('/',1)[-1] for line in (proc/'maps').read_text().splitlines()
                  if len(line.split(None,5))==6 and line.split(None,5)[5].startswith('/')})
    reading=dict(rollup=(proc/'smaps_rollup').read_text(), names=names, load=os.getloadavg()[0],cpus=os.cpu_count())
    from tools.lab.surface_driver.recipes.outcomes import idle_summary
    summary=idle_summary(reading,os.getloadavg()[0],os.cpu_count())
    summary['forbidden']+= [n for n in names if EXTRA.match(n)]
    receipt.update(memory=summary,mapped_names=names)
    receipt['helpers']=lifecycle(a.helper_events)
    receipt['status']='measured'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cli',required=True)
    p.add_argument('--socket',type=Path,required=True)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--region-driver',required=True)
    p.add_argument('--helper-events',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    receipt=dict(schemaVersion=1,status='blocked',jobs=[],
                 assessment='Manual measurement receipt; no acceptance verdict.')
    # Reserve before any actions; never overwrite an earlier attempt.
    with a.output.open('x') as out:
        try: run(a,receipt)
        except Exception as error: receipt['reason']=str(error)
        if a.helper_events.is_file():
            try:
                events=[json.loads(line) for line in a.helper_events.read_text().splitlines() if line.strip()]
                receipt['helper_events']=[{k:e[k] for k in ('pid','startTicks','event','exitCode') if k in e}
                                          for e in events]
            except (OSError, ValueError):
                receipt['helper_events_error']='Lifecycle observations could not be read.'
        json.dump(receipt,out,indent=2); out.write('\n')
    return 0 if receipt['status']=='measured' else 2


if __name__=='__main__': raise SystemExit(main())
