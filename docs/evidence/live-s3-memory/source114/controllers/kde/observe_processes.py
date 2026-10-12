#!/usr/bin/env python3
"""Supplemental numeric product process events; stop BEFORE fixed quiet.

No cmdline, environment, socket, heap, SDK trace or payload reads. A process that
disappears is NOT assumed to have exited normally. Only an actually sampled
zombie's Linux wait status yields an independently observed exit code.
"""
import argparse,datetime,hashlib,json,os,re,stat,time
from pathlib import Path

def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def fields(p):
 f=(p/'stat').read_text().rsplit(')',1)[1].split();return {'state':f[0],'startTicks':int(f[19]),'cpuTicks':int(f[11])+int(f[12]),'rawWaitStatus':int(f[49]) if f[0]=='Z' and len(f)>=50 else None}
def main():
 a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);a.add_argument('--expected-binaries',type=Path,required=True);a.add_argument('--seconds',type=int,default=3600);a.add_argument('--interval-ms',type=int,default=200);args=a.parse_args();assert 1<=args.seconds<=3600 and 100<=args.interval_ms<=1000;os.umask(0o077);s=args.expected_binaries.lstat();assert stat.S_ISREG(s.st_mode) and s.st_uid==os.getuid() and not s.st_mode&0o077;expected=json.loads(args.expected_binaries.read_text());assert expected and all(Path(k).name.startswith('putgd') and re.fullmatch('[0-9a-f]{64}',v) for k,v in expected.items());args.output.mkdir(mode=0o700,parents=True,exist_ok=False);metadata={'startedUTC':now(),'intervalMs':args.interval_ms,'maximumSeconds':args.seconds,'scope':'Supplemental diagnostic sampler; never used to replace1Hz/fixed300s acceptance; stoppedbeforequiet','expectedExecutableHashes':expected,'rawMessagesCredentialsPayloadsIncluded':False};(args.output/'parameters.json').write_text(json.dumps(metadata,indent=2)+'\n');cache={};seen={};start=time.monotonic();rows=0
 with (args.output/'events.jsonl').open('x',buffering=1) as sink:
  while time.monotonic()-start<=args.seconds and not (args.output/'stop').exists():
   tick=time.monotonic();current={}
   for p in Path('/proc').iterdir():
    if not p.name.isdecimal():continue
    try:
     if p.stat().st_uid!=os.getuid():continue
     comm=(p/'comm').read_text().strip()
     if not comm.startswith('putgd') or comm.startswith('putgd-desktop'):continue
     before=fields(p);identity=(int(p.name),before['startTicks'])
     if before['state']=='Z' and identity in seen and before['rawWaitStatus'] is not None:
      value=dict(seen[identity]);value.update({'state':'Z','elapsedSeconds':tick-start,'utc':now(),'rawWaitStatus':before['rawWaitStatus'],'independentExitCode':os.waitstatus_to_exitcode(before['rawWaitStatus']),'independentExitSignal':(before['rawWaitStatus']&127) or None});current[identity]=value;sink.write(json.dumps({'event':'zombieWaitStatusObserved',**value})+'\n');rows+=1;continue
     exe=p/'exe';info=exe.stat();name=Path(os.readlink(exe)).name;expectedHash=expected.get('/usr/bin/'+name);assert expectedHash,'unrecognized_owned_product_executable';key=(info.st_dev,info.st_ino,info.st_size)
     if key not in cache:
      with exe.open('rb') as f:cache[key]=hashlib.file_digest(f,'sha256').hexdigest()
     after=fields(p);assert after['startTicks']==before['startTicks'];assert cache[key]==expectedHash,'owned_product_binary_changed';identity=(int(p.name),before['startTicks']);value={'pid':identity[0],'startTicks':identity[1],'comm':comm,'executableName':name,'executableSha256':cache[key],'state':after['state'],'cpuTicks':after['cpuTicks'],'elapsedSeconds':tick-start,'utc':now(),'independentExitCode':None,'independentExitSignal':None}
     if after['rawWaitStatus'] is not None:
      value['rawWaitStatus']=after['rawWaitStatus'];value['independentExitCode']=os.waitstatus_to_exitcode(after['rawWaitStatus']);value['independentExitSignal']=(after['rawWaitStatus']&127) or None
     current[identity]=value
     if identity not in seen or after['rawWaitStatus'] is not None:sink.write(json.dumps({'event':'firstObserved' if identity not in seen else 'zombieWaitStatusObserved',**value})+'\n');rows+=1
    except (FileNotFoundError,ProcessLookupError,PermissionError):continue
   for identity,last in seen.items():
    if identity not in current:sink.write(json.dumps({'event':'noLongerPresent','pid':identity[0],'startTicks':identity[1],'lastObservedExecutableSha256':last['executableSha256'],'elapsedSeconds':tick-start,'utc':now(),'independentExitCode':None,'reason':'disappearance alone does not establish exit code'})+'\n');rows+=1
   seen=current;delay=args.interval_ms/1000-(time.monotonic()-tick)
   if delay>0:time.sleep(delay)
 (args.output/'completed.json').write_text(json.dumps({'completedUTC':now(),'elapsedSeconds':time.monotonic()-start,'eventCount':rows,'remainingObservedProductPids':[x[0] for x in seen],'scope':'Diagnostic evidence only; exitcode null unless actualkernelwaitstatus observed'},indent=2)+'\n')
if __name__=='__main__':main()
