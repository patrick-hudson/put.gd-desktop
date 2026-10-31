"""Reconcile only the exact cancelled pre-input S3 operation, with no upload."""
import json,os,shlex,re
from pathlib import Path
from tools.lab import lab,proton_lab,provision_live_credentials as bridge
from tools.lab.surface_driver.api import Recipe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.target import Target
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import run_recipes,finish_evidence,build_matches
from tools.lab.surface_driver.fixtures.live_outcomes import admit
run=Path('work/lab-runs/r3c').resolve();root='/dev/shm/tcsa';out=Path('/home/patrick/t1r3e/cin-s3-cancel');out.mkdir(mode=0o700)
receipt=Path('/home/patrick/t1r3e/cin-s3a/receipt.json');old=json.loads(receipt.read_text());ids=[x['operationId'] for x in old['environment']['operation_ids'] if 'operationId' in x];assert len(ids)==1;oid=ids[0]
assert old['environment']['placement_details']['session_root']==root
lab.verify_owned_guest(run);target=Target(run=run,session_root=root,placement='guest');target.preflight()
r=target.execute("python3 - <<'DATA'\nfrom pathlib import Path\nimport json\np=list(Path('/dev/shm/tcsa').glob('fx-*/data'));assert len(p)==1;print(json.dumps(str(p[0])))\nDATA")
data=json.loads(r['stdout']);assert data.startswith(root+'/fx-')
assert target.execute('test ! -e '+shlex.quote(target.socket))['returncode']==0
assert target.execute('nohup env XDG_DATA_HOME='+shlex.quote(data)+' putgd --background > '+root+'/reconcile-app.log 2>&1 < /dev/null &')['returncode']==0
target.started=True
build='cc9d9410d6e982dd93d58bcfda82f112c44a82f6';env=dict(desktop='mint-cinnamon-x11',host='d69',placement='guest',guest='cinnamon-x11',package='deb',destination='s3',build=build,placement_details={})
ctx=DriverContext(target,env,load_profile(env['desktop'],Path('/home/patrick/t1profiles')),'/home/patrick/t1cat',out)
def finish(ctx):
 admit(ctx)
 ctx.wait(lambda:target.execute('test -S '+shlex.quote(target.socket))['returncode']==0,'retained resident ready')
 b=ctx.call('ui.snapshot')['build'];ctx.expect(build_matches(build,b['gitCommit']) and not b['dirty'],'exact published build resumed')
 op=ctx.call('operation.inspect',{'id':oid})
 if op['state'] in ('accepted','acquiring'):
  ctx.call('operation.cancel',{'id':oid})
  op=ctx.wait(lambda:(o if (o:=ctx.call('operation.inspect',{'id':oid}))['state'] in ('cancelled','failed') else None),'exact pre-input capture cancelled')
 ctx.expect(op['id']==oid and op['state'] in ('cancelled','failed') and not op['items'],'terminal pre-input operation has zero items and no publication')
 ctx.expect(not ctx.call('history.list',{'limit':100})['records'],'no published or failed artifact row exists')
 ctx.note('cancelled_before_upload_count=1; operationId='+oid)
 profile=next(p for p in ctx.call('settings.get')['document']['profiles'] if p['id']=='tier1-live')
 cr=Path('/home/patrick/t1r3e/cin-s3a/s3-bridge.json');_,state=bridge._load_receipt(cr)
 ctx.expect(state['credentialReference']['reference']==profile['credentials']['reference'],'exact retained bridge reference')
 guest=proton_lab.Guest(run);placed=guest.place_brokers(Path.home()/'putgd-lab/proton/guest-bridge')
 try:bridge.cleanup(dict(op='cleanup',provider='s3',run=str(run),guestBroker=placed['qt'],reference=profile['credentials']['reference'],receipt=str(cr)))
 finally:guest.remove_brokers()
 ctx.call('history.clear');ctx.call('cache.clear')
 ctx.note('retained_s3_credential_cleanup_count=1')
 ctx.frame('cancelled-before-upload-confirmed')
recipe=Recipe(('region.to-upload',),finish,destinations=('s3',),desktop_drawn=True,timeout=240)
rows=[]
try:rows=run_recipes(ctx,[recipe],env,out,'tier1-phase-b-followup-2',out/'coverage.json')
finally:
 errors=target.close();env['cleanup_errors']=errors
 if rows and all(r.functional=='passed' for r in rows) and not errors:
  code='''from pathlib import Path
import os,stat,shutil,time,json
root=Path('/dev/shm/tcsa');s=root.lstat()
assert root.resolve()==root and s.st_uid==os.getuid() and stat.S_IMODE(s.st_mode)==0o700 and stat.S_ISDIR(s.st_mode)
for line in Path('/proc/self/mountinfo').read_text().splitlines():
 p=Path(line.split()[4].replace(chr(92)+'040',' '));assert p!=root and not p.is_relative_to(root)
skip=set();pid=os.getpid()
while pid>1:
 skip.add(pid);pid=int((Path('/proc')/str(pid)/'stat').read_text().split(') ',1)[1].split()[1])
for attempt in range(50):
 live=[]
 for p in Path('/proc').glob('[0-9]*'):
  if int(p.name) in skip:continue
  try:
   e=(p/'environ').read_bytes().split(b'\\0')
   if b'XDG_RUNTIME_DIR=/dev/shm/tcsa/runtime' in e:live.append(p.name)
  except OSError:pass
 if not live:break
 time.sleep(.1)
assert not live
shutil.rmtree(root);print(json.dumps({'ownedRetainedRootRemoved':not root.exists()}))
'''
  result=target.execute("python3 - <<'CLEAN'\n"+code+"\nCLEAN",timeout=15)
  if result['returncode']:env['cleanup_errors'].append('retained root cleanup refused')
  else:env['private_cleanup']='reconciled-preinput-root-removed'
 scan=finish_evidence(ctx.evidence,out,env,rows,'tier1-phase-b-followup-2',out/'coverage.json')
print(json.dumps({'scanPassed':scan['passed'],'statuses':[r.functional for r in rows],'cleanupErrors':env['cleanup_errors']}))
