"""Export only this campaign's owned guest test directories, verify every tar member."""
import sys,subprocess,shlex,json,hashlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent));import control
n=sys.argv[1];assert n in ('u382a','a382a')
remote=Path('/home/patrick/work/a382/work/exports/i382-final-'+n)
script=r'''
import os,pathlib,subprocess,shlex,json,hashlib,tarfile,sys
n=sys.argv[1];root=pathlib.Path('/home/patrick/work/a382/work/exports/i382-final-'+n);root.mkdir(mode=0o700,parents=True,exist_ok=False)
port={'u382a':22282,'a382a':22482}[n];base='/home/patrick/work/a382/work/lab-runs/'+n
ssh=['ssh','-i',base+'/identity','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+base+'/known_hosts','-o','BatchMode=yes','-p',str(port),'lab@127.0.0.1']
code="import pathlib,json,hashlib; root=pathlib.Path('/home/lab/work'); names=['i382a','p382r','p382f','p382h','p382n']; files={}\nfor n in names:\n for p in (root/n).rglob('*'):\n  assert not p.is_symlink(),str(p)\n  if p.is_file():\n   assert not any(v in p.name.lower() for v in ['password','credential','secret','id_rsa','id_ed25519']),str(p)\n   files[str(p.relative_to(root))]={'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'bytes':p.stat().st_size}\nprint(json.dumps(files))"
r=subprocess.run(ssh+[shlex.join(['python3','-c',code])],capture_output=True,text=True,check=True);index=json.loads(r.stdout);(root/'guest-files.json').write_text(json.dumps(index,indent=2)+'\n')
archive=root/'guest-work.tar'
with archive.open('xb') as f:subprocess.run(ssh+[shlex.join(['tar','-C','/home/lab/work','-cf','-','i382a','p382r','p382f','p382h','p382n'])],stdout=f,stderr=subprocess.PIPE,check=True,timeout=300)
os.chmod(archive,0o600)
seen=set()
with tarfile.open(archive,'r:') as t:
 for m in t:
  assert m.isfile() or m.isdir(),m.name
  if not m.isfile():continue
  assert m.name in index and m.size==index[m.name]['bytes'],m.name
  assert hashlib.file_digest(t.extractfile(m),'sha256').hexdigest()==index[m.name]['sha256'],m.name
  seen.add(m.name)
assert seen==set(index)
receipt={'guest':n,'archive':str(archive),'bytes':archive.stat().st_size,'sha256':hashlib.file_digest(archive.open('rb'),'sha256').hexdigest(),'files':len(index),'allMembersVerified':True,'scope':'Only owned synthetic test directories; no guest identity key/bootstrap/login material'}
(root/'archive-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
'''
r=control.run('f44',['python3','-c',script,n],timeout=600);Path('work/i382n/'+n+'-archive-result.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r));assert r['exit']==0
