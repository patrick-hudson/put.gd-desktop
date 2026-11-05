import subprocess,concurrent.futures,json,sys,shlex,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent
HOSTS={'f44':'192.168.68.70','d69':'192.168.68.69','k72':'192.168.68.72','m74':'192.168.68.74'}
def ssh(host):return ['ssh','-i','/home/patrick/.ssh/awskey_110724','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','BatchMode=yes','-o','ConnectTimeout=8','-o','UserKnownHostsFile=/home/patrick/work/put.gd/work/remote-builders/'+host+'/known_hosts','patrick@'+HOSTS[host]]
def run(host,args,timeout=1200):
 r=subprocess.run(ssh(host)+[shlex.join(args)],capture_output=True,text=True,timeout=timeout);return {'host':host,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
def stage(host):
 r=run(host,['install','-d','-m','0700','/home/patrick/work/p382n']);assert not r['exit'],r
 for src,dst in [(ROOT/'i382.py','i382.py'),(Path('docs/evidence/2026-10-23-resident-session-490/lab/r490.py'),'r490.py'),(Path('config/development.example.json'),'template.json'),(Path('tools/diagnostics/check_socket_paths.py'),'check_socket_paths.py')]:
  r=subprocess.run(ssh(host)+['cat > '+shlex.quote('/home/patrick/work/p382n/'+dst)],input=src.read_bytes(),capture_output=True);assert not r.returncode,r.stderr
 return {'host':host,'staged':True}
def call(host,mode,args):return run(host,['python3','/home/patrick/work/p382n/i382.py',mode,*args])
if __name__=='__main__':
 hosts=sys.argv[1].split(',');action=sys.argv[2]
 def one(host):
  if action=='stage':return stage(host)
  return call(host,action,sys.argv[3:])
 results=list(concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)).map(one,hosts))
 for r in results:print(json.dumps(r),flush=True)
 sys.exit(1 if any(r.get("exit",0) for r in results) else 0)
