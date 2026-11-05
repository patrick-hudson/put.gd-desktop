from pathlib import Path
import json,subprocess,sys
run=Path(sys.argv[1]).resolve();script,prefix=sys.argv[2:4];base=Path('/home/patrick/t1r2e');session='codex-tier1b-r2-6801919c'
entries=[(p.stat().st_mtime,json.loads(p.read_text())) for p in (Path.home()/'putgd-lab/proton/receipts').glob('*/lend.json')]
if len(sys.argv)>4:
 from tools.lab import lab
 root=sys.argv[4];assert root.startswith('/dev/shm/')
 code="import json,os,stat\nfrom pathlib import Path\np=Path("+repr(root+"/config/putgd/config.json")+")\nassert not p.is_symlink() and p.stat().st_uid==os.getuid() and stat.S_ISREG(p.stat().st_mode)\nv=json.loads(p.read_text());print(json.dumps(next(p['credentials']['reference'] for p in v['profiles'] if p['id']=='tier1-live')))"
 import shlex,re
 ref=json.loads(lab.guest_session(run,"python3 - <<'REF'\n"+code+"\nREF",timeout=20))
 assert re.fullmatch('putgd-lab-[0-9a-f-]{36}',ref)
else:
 ref=min((t,v['reference']) for t,v in entries if v.get('run')==str(run) and v.get('session')==session and v.get('step')=='cleaned')[1]
with (base/(prefix+'-check-before.json')).open('x') as f:subprocess.run(['python3','tools/lab/proton_lab.py','check','--session',session,'--owner','tier1-phase-b'],stdout=f,check=True)
with (base/(prefix+'.log')).open('x') as f:r=subprocess.run(['python3','tools/lab/proton_lab.py','lend',str(run),'--guest-reference',ref,'--session',session,'--owner','tier1-phase-b','--purpose','History UI cleanup of the exact retained upload; no new upload','--','python3',script],stdout=f,stderr=f)
print(prefix,'lend exit',r.returncode,flush=True)
with (base/(prefix+'-final-check.json')).open('x') as f:subprocess.run(['python3','tools/lab/proton_lab.py','check','--session',session,'--owner','tier1-phase-b'],stdout=f,check=True)
