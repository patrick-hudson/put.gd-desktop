"""Finish only the retained upload; never capture or upload a second payload."""
import json,os,shlex,time
from pathlib import Path
from tools.lab import lab
from tools.lab.surface_driver.api import Recipe
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.target import Target
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import run_recipes,finish_evidence,build_matches
from tools.lab.surface_driver.browser import default_browser,open_and_verify,live_download_digest,live_link_unavailable
from tools.lab.surface_driver.recipes import outcomes
from tools.lab.surface_driver.recipes.outcomes_live import _cleanup
from tools.lab.surface_driver.fixtures.live_outcomes import admit
run=Path(os.environ['PUTGD_LAB_RUN']).resolve();root='/dev/shm/rks2';out=Path('/home/patrick/t1r2e/kde-s3-detail');out.mkdir(mode=0o700)
build='cc9d9410d6e982dd93d58bcfda82f112c44a82f6';hid='22ddf47e-304a-41e5-8ef5-e154c0329434'
lab.verify_owned_guest(run)
target=Target(run=run,session_root=root,placement='guest')
target.preflight()
r=target.execute("python3 - <<'END'\nfrom pathlib import Path\nimport json\np=list(Path('/dev/shm/rks2').glob('fx-*/data'));assert len(p)==1;print(json.dumps(str(p[0])))\nEND\n")
data=json.loads(r['stdout']);assert data.startswith(root+'/fx-')
assert not target.execute('test ! -e '+shlex.quote(target.socket))['returncode']
target.execute('nohup env XDG_DATA_HOME='+shlex.quote(data)+' putgd --background > '+root+'/post-app.log 2>&1 < /dev/null &')
target.started=True
env=dict(desktop='kubuntu-kde-wayland',host='f44',placement='guest',guest='kde-wayland',package='deb',destination='s3',build=build,placement_details={})
ctx=DriverContext(target,env,load_profile(env['desktop'],Path('/home/patrick/t1profiles')),'/home/patrick/t1cat',out)
def finish(ctx):
 admit(ctx)
 op=ctx.call('operation.inspect',{'id':'0920a9cf-7d17-4c39-8950-ea868e81d37c'})
 d=ctx.call('history.detail',{'id':hid})
 def code(value):
  import re
  s=value.get('error',{}).get('code','')
  return s if re.fullmatch('[a-zA-Z0-9_]{0,80}',s) else '[unclassified]'
 ctx.note('retained-failure='+json.dumps({'operationState':op.get('state'),'operationError':code(op),'uploadStatus':d.get('uploadStatus'),'shareStatus':d.get('shareStatus'),'linkAvailable':d.get('linkAvailable'),'detailError':code(d),'hasReceipt':bool(d.get('receipt')),'itemErrors':[code(i) for i in op.get('items',[])],'itemFieldNames':[list(i) for i in op.get('items',[])]}))
 settings=ctx.call('settings.get')['document']
 profile=next(p for p in settings['profiles'] if p['id']=='tier1-live')
 ctx.note('destinationAclMode='+profile['settings']['aclMode'])
 for item in [d]+op.get('items',[]):
  e=item.get('error',{}).get('details',{})
  ctx.note('provider-error-fields='+json.dumps({k:e.get(k) for k in ('requestStage','serviceCode','httpStatus')}))
 outcomes._history(ctx,d);ctx.frame('failed-upload-history',window='history')
recipe=Recipe(('region.to-upload',),finish,destinations=('s3',),desktop_drawn=True,timeout=420)
rows=[]
try:rows=run_recipes(ctx,[recipe],env,out,'tier1-phase-b',out/'coverage.json')
finally:
 env['cleanup_errors']=target.close()
 scan=finish_evidence(ctx.evidence,out,env,rows,'tier1-phase-b',out/'coverage.json')
print(json.dumps({'scanPassed':scan['passed'],'statuses':[r.functional for r in rows]}))
