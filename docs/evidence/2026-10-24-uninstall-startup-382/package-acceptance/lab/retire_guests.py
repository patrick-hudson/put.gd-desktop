"""Guarded retirement after complete verified guest exports, preserving every base."""
import sys,json,subprocess,shlex,tarfile,io,hashlib
from pathlib import Path
import control
n=sys.argv[1];assert n in ('u382a','a382a');source=Path('work/i382n/evidence')/n;target='/home/patrick/work/a382/work/exports/i382-final-'+n
# Keep full good and failed visual/JSON evidence outside the disposable disk too.
buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w') as t:
 for p in sorted(source.rglob('*')):
  assert not p.is_symlink(),p
  if p.is_file():t.add(p,arcname=str(p.relative_to(source)),recursive=False)
data=buf.getvalue();digest=hashlib.sha256(data).hexdigest()
r=subprocess.run(control.ssh('f44')+['umask 077; cat > '+shlex.quote(target+'/workstation-evidence.tar')],input=data,capture_output=True);assert not r.returncode,r.stderr
code=r'''
import pathlib,json,hashlib,subprocess,sys,tarfile,os
n,expected=sys.argv[1:];root=pathlib.Path('/home/patrick/work/a382/work/exports/i382-final-'+n);run=pathlib.Path('/home/patrick/work/a382/work/lab-runs')/n;state=json.loads((run/'state.json').read_text());a=root/'workstation-evidence.tar';assert hashlib.file_digest(a.open('rb'),'sha256').hexdigest()==expected
receipt=json.loads((root/'archive-receipt.json').read_text());assert receipt['allMembersVerified'];assert hashlib.file_digest((root/'guest-work.tar').open('rb'),'sha256').hexdigest()==receipt['sha256']
with tarfile.open(a,'r:') as t:
 seen=set()
 for m in t:
  assert m.isfile() and not pathlib.PurePosixPath(m.name).is_absolute() and '..' not in pathlib.PurePosixPath(m.name).parts,m.name
  if m.name.endswith('/summary.json'):
   d=json.load(t.extractfile(m));assert d['completion']['functionalPassed'] and d['build']['gitCommit']=='d5b561ddb94f8255b8bc754c1382552d983a6fa9' and d['visualReview']['accepted'];seen.add(d['mode'])
assert seen==({'native','appimage'} if n=='u382a' else {'appimage'}),seen
(root/'completion.json').write_text(json.dumps({'run':str(run),'runId':state['id'],'completed':True,'outcome':'passed','scope':'Final d5b561dd targeted uninstall lifecycle; earlier failed attempts retained as failures in verified guest/workstation evidence archives','sourceCommit':'d5b561ddb94f8255b8bc754c1382552d983a6fa9','historicalFailuresRecorded':True},indent=2)+'\n')
files={p.name:hashlib.file_digest(p.open('rb'),'sha256').hexdigest() for p in root.iterdir() if p.is_file() and p.name!='cleanup-evidence.json'}
manifest={'schemaVersion':1,'kind':'vm-overlay-cleanup-evidence','run':str(run),'runId':state['id'],'completed':True,'outcome':'passed','files':files};(root/'cleanup-evidence.json').write_text(json.dumps(manifest,indent=2)+'\n')
base=['python3','tools/lab/lab.py'];cwd='/home/patrick/work/a382'
def call(args,label):
 r=subprocess.run(base+args,cwd=cwd,capture_output=True,text=True,timeout=180);(root/(label+'.log')).write_text(r.stdout+r.stderr);print(json.dumps({'phase':label,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}),flush=True);assert not r.returncode
call(['reset','--run',str(run)],'reset')
args=['cleanup','--run',str(run),'--images-root','/home/patrick/work/put.gd/work/lab-images','--evidence-manifest',str(root/'cleanup-evidence.json')]
call(args+['--dry-run'],'cleanup-preview');call(args,'cleanup-apply')
assert not (run/'disk.qcow2').exists();assert pathlib.Path(state['golden']).is_file()
retired=json.loads((run/'overlay-cleanup.json').read_text());(root/'overlay-cleanup.json').write_text(json.dumps(retired,indent=2)+'\n')
print(json.dumps({'guest':n,'runId':state['id'],'diskAbsent':True,'goldenPresent':True,'exportRoot':str(root),'retirementReceipt':retired}),flush=True)
'''
r=control.run('f44',['python3','-c',code,n,digest],timeout=600);Path('work/i382n/'+n+'-retirement-result.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r));assert not r['exit'],r
