import sys,json,subprocess,shlex
from pathlib import Path
import control
GUESTS={'u382a':22282,'a382a':22482}
BASE='/home/patrick/work/a382/work/lab-runs/'
def guest(n,args,stdin=None,timeout=1200):
 key=BASE+n+'/identity';pin=BASE+n+'/known_hosts'
 argv=['ssh','-i',key,'-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+pin,'-o','BatchMode=yes','-p',str(GUESTS[n]),'lab@127.0.0.1',shlex.join(args)]
 remote=shlex.join(argv)
 r=subprocess.run(control.ssh('f44')+[remote],input=stdin,capture_output=True,text=True,timeout=timeout)
 return {'host':n,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
if __name__=='__main__':
 n=sys.argv[1]
 if sys.argv[2]=='stage':
  r=guest(n,['install','-d','-m','0700','/home/lab/work/p382n']);assert not r['exit'],r
  for f in ['i382.py','r490.py','template.json','check_socket_paths.py']:
   src=Path('work/i382n')/f
   if f=='r490.py':src=Path('docs/evidence/2026-10-23-resident-session-490/lab/r490.py')
   if f=='template.json':src=Path('config/development.example.json')
   if f=='check_socket_paths.py':src=Path('tools/diagnostics/check_socket_paths.py')
   # stdin is bounded source only, no secrets
   r=guest(n,['sh','-c','cat > /home/lab/work/p382n/'+f],src.read_text());assert not r['exit'],r
 else:r=guest(n,['python3','/home/lab/work/p382n/i382.py',*sys.argv[2:]])
 print(json.dumps(r));sys.exit(r["exit"])
