"""Real user-manager regression: deliberate stop is successful, rapid rearm is safe."""
import sys,os,json,hashlib,tempfile,subprocess,time,re,shutil
from pathlib import Path
out=Path(sys.argv[1]);out.mkdir(mode=0o700);source=Path(sys.argv[2]).read_text();root=Path(tempfile.mkdtemp(prefix='u382m-'));configured=root/'launcher';shutil.copy2('/bin/true',configured);entry=root/'putgd.desktop';script=root/'start.sh';identity=hashlib.sha256((str(entry)+'\n'+str(configured)).encode()).hexdigest();unit='putgd-login-startup-'+identity
quote=lambda v:"'"+v.replace("'","'\\''")+"'"
values={'CONFIGURED':quote(str(configured)),'UPDATE_INFORMATION':quote(''),'KIND':quote('native'),'ENTRY':quote(str(entry)),'SCRIPT':quote(str(script)),'ID':identity,'WATCH_UNIT':quote(unit)}
script.write_text(re.sub(r'__PUTGD_([A-Z_]+)__',lambda m:values[m[1]],source));script.chmod(0o700);entry.write_text('[Desktop Entry]\nX-putgd-Managed=true\nX-putgd-StartupId='+identity+'\n');entry.chmod(0o600)
preflight={'paths':[{'path':'/run/user/'+str(os.getuid())+'/bus','utf8Bytes':len(('/run/user/'+str(os.getuid())+'/bus').encode())}],'limit':107};assert all(p['utf8Bytes']<=107 for p in preflight['paths'])
started=int(time.time())-1;results=[]
def cmd(args):
 r=subprocess.run(args,capture_output=True,text=True,timeout=15);return {'argv':args,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
try:
 for i in range(10):
  start=cmd(['/bin/sh',str(script),'--install-watch']);assert start['exit']==0,start
  time.sleep(.05)
  properties=cmd(['systemctl','--user','show',unit+'.service','-p','SuccessExitStatus','-p','ActiveState'])
  stop=cmd(['/bin/sh',str(script),'--stop-watch']);results.append({'iteration':i+1,'start':start,'serviceBeforeStop':properties,'stop':stop});assert stop['exit']==0,stop
finally:
 cleanup=cmd(['/bin/sh',str(script),'--stop-watch']);journal=cmd(['journalctl','--user','-u',unit+'.path','-u',unit+'.service','--since','@'+str(started),'--no-pager','-o','short-iso']);errors=[l for l in journal['stdout'].splitlines() if re.search(r'failed|refusing|cannot',l,re.I)]
 result={'scope':__doc__,'templateSha256':hashlib.sha256(source.encode()).hexdigest(),'socketPreflight':preflight,'unit':unit,'iterations':results,'cleanup':cleanup,'journal':journal,'journalErrors':errors,'pass':len(results)==10 and not errors and not cleanup['exit']}
 (out/'rendered-script.sh').write_bytes(script.read_bytes());(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'pass':result['pass'],'iterations':len(results),'journalErrors':errors}),flush=True)
 if cleanup['exit']==0:shutil.rmtree(root)
assert result['pass'],result['journalErrors']
