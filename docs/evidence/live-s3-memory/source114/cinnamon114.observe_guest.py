#!/usr/bin/env python3
"""Supplemental live-S3 attribution; does not replace frozen acceptance gates."""
import argparse,datetime,hashlib,json,os,socket,time,uuid
from pathlib import Path

def identity(p):
    f=(p/'stat').read_text().rsplit(')',1)[1].split()
    return int(f[19]),int(f[11])+int(f[12])

def footprint(p):
    groups={};name=None
    for line in (p/'smaps').read_text().splitlines():
        f=line.split()
        if f and '-' in f[0] and len(f)>=5:
            mapped=f[5] if len(f)>5 else '[anonymous]';name=Path(mapped).name if mapped.startswith('/') else mapped
            groups.setdefault(name,{})
        elif name and f and f[0] in ('Rss:','Pss:','Private_Clean:','Private_Dirty:','Anonymous:','Swap:','SwapPss:'):
            k=f[0][:-1];groups[name][k]=groups[name].get(k,0)+int(f[1])*1024
    return [{'mapping':n,**v} for n,v in sorted(groups.items(),key=lambda x:-x[1].get('Rss',0))]

def call(method):
    path='/run/user/1000/putgd/session.sock'
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        s.settimeout(15);s.connect(path);s.sendall(json.dumps(dict(schemaVersion=1,requestId=str(uuid.uuid4()),session=path,method=method,params={})).encode()+b'\n');data=b''
        while not data.endswith(b'\n'):
            more=s.recv(65536)
            if not more:raise RuntimeError('transport_closed')
            data+=more
            if len(data)>1048576:raise RuntimeError('reply_limit')
        v=json.loads(data)
        if not v['ok']:raise RuntimeError(v['error']['code'])
        return v['result']

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--pid',type=int,required=True);ap.add_argument('--start-ticks',type=int,required=True);ap.add_argument('--sha256',required=True);ap.add_argument('--output',required=True);ap.add_argument('--seconds',type=int,default=1200);a=ap.parse_args()
    os.umask(0o077);out=Path(a.output);out.mkdir(mode=0o700,parents=True,exist_ok=False);p=Path('/proc',str(a.pid));assert identity(p)[0]==a.start_ticks
    digest=hashlib.sha256((p/'exe').read_bytes()).hexdigest();assert digest==a.sha256
    meta={'schemaVersion':1,'scope':'Supplemental live-S3 attribution; original memory gates remain unchanged','argv':os.sys.argv,'observerSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'pid':a.pid,'startTicks':a.start_ticks,'executableSha256':digest,'build':call('diagnostics')['build'],'intervalSeconds':1,'diagnosticsIntervalSeconds':10,'mappingIntervalSeconds':10,'clockTicksPerSecond':os.sysconf('SC_CLK_TCK'),'pageSize':os.sysconf('SC_PAGE_SIZE'),'startedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'seconds':a.seconds}
    (out/'parameters.json').write_text(json.dumps(meta,indent=2)+'\n');start=time.monotonic();lastDiag=-10;stage='cold';rows=0
    with (out/'samples.jsonl').open('x',buffering=1) as sink:
        while time.monotonic()-start<=a.seconds and not (out/'stop').exists():
            tick=time.monotonic();assert identity(p)[0]==a.start_ticks
            control=out.parent/'stage.json'
            if control.exists():stage=json.loads(control.read_text())['stage']
            members=[]
            for q in Path('/proc').iterdir():
                if not q.name.isdecimal():continue
                try:
                    comm=(q/'comm').read_text().strip()
                    if q.stat().st_uid!=os.getuid() or not comm.startswith('putgd') or comm.startswith('putgd-desktop'):continue
                    born,ticks=identity(q);roll={}
                    for line in (q/'smaps_rollup').read_text().splitlines():
                        f=line.split()
                        if len(f)==3 and f[2]=='kB':roll[f[0][:-1]]=int(f[1])*1024
                    status=(q/'status').read_text().splitlines();threads=int(next(l.split()[1] for l in status if l.startswith('Threads:')))
                    members.append({'pid':int(q.name),'comm':comm,'startTicks':born,'cpuTicks':ticks,'threads':threads,'memoryBytes':roll})
                except (FileNotFoundError,ProcessLookupError):pass
            row={'elapsedSeconds':tick-start,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'stage':stage,'family':members,'residentStatmRssBytes':int((p/'statm').read_text().split()[1])*os.sysconf('SC_PAGE_SIZE')}
            if tick-start>=lastDiag+10:
                d=call('ui.snapshot')['diagnostics'];row['diagnostics']={'activeOperations':d['activeOperations'],'journalReady':d['journalReady'],'memory':d['memory'],'ui':d['ui']};row['residentMappings']=footprint(p);lastDiag=tick-start
            sink.write(json.dumps(row)+'\n');rows+=1
            temp=out/'progress.tmp';temp.write_text(json.dumps({'samples':rows,'elapsedSeconds':row['elapsedSeconds'],'stage':stage,'rssMiB':row['residentStatmRssBytes']/1048576}));temp.replace(out/'progress.json')
            time.sleep(max(0,1-(time.monotonic()-tick)))
    (out/'completed.json').write_text(json.dumps({'samples':rows,'elapsedSeconds':time.monotonic()-start,'residentIdentityUnchanged':identity(p)[0]==a.start_ticks})+'\n')

if __name__=='__main__':main()
