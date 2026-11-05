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
run=Path(os.environ['PUTGD_LAB_RUN']).resolve();root='/dev/shm/rcp1';out=Path('/home/patrick/t1r2e/cin-cleanup');out.mkdir(mode=0o700)
build='cc9d9410d6e982dd93d58bcfda82f112c44a82f6';hid='487a04a6-a747-4431-be14-c71e681a0e76'
lab.verify_owned_guest(run)
target=Target(run=run,session_root=root,placement='guest')
target.preflight()
r=target.execute("python3 - <<'END'\nfrom pathlib import Path\nimport json\np=list(Path('/dev/shm/rcp1').glob('fx-*/data'));assert len(p)==1;print(json.dumps(str(p[0])))\nEND\n")
data=json.loads(r['stdout']);assert data.startswith(root+'/fx-')
assert not target.execute('test ! -e '+shlex.quote(target.socket))['returncode']
target.execute('nohup env XDG_DATA_HOME='+shlex.quote(data)+' putgd --background > '+root+'/post-app.log 2>&1 < /dev/null &')
target.started=True
env=dict(desktop='mint-cinnamon-x11',host='d69',placement='guest',guest='cinnamon-x11',package='deb',destination='proton',build=build,placement_details={})
ctx=DriverContext(target,env,load_profile(env['desktop'],Path('/home/patrick/t1profiles')),'/home/patrick/t1cat',out)
def finish(ctx):
 admit(ctx)
 v=ctx.call('ui.snapshot')['build'];ctx.expect(build_matches(build,v['gitCommit']) and v['dirty'] is False,'exact published build resumed')
 ctx.start_notifications()
 detail=ctx.call('history.detail',{'id':hid});ctx.expect(detail['id']==hid and detail['uploadStatus']=='uploaded','retained exact owned uploaded row')
 ctx.note('historyId='+hid)
 with default_browser(ctx):
  previous=ctx.profile.get('windows',{});ctx.profile['windows']={**previous,'outcome-browser':{'title':'.*Firefox.*'}}
  try:
   outcomes._history(ctx,detail);ctx.click('copyHistoryLink',window='history');ctx.expect(ctx.clipboard_text()==detail['receipt']['directUrl'],'copy exact retained share link')
   ctx.click('windowClose',window='history')
   def paste():
    ctx.focus('outcome-browser',raise_window=False);ctx.keys('Ctrl+L','Ctrl+V','Return')
   open_and_verify(ctx,paste,{'kind':'proton','name':detail['name'],'preview':True})
   ctx.expect(detail['receipt']['destination']['credentials']['reference']==os.environ['PUTGD_LAB_PROTON_REFERENCE'],'same original guest credential reference restored through bridge')
   ctx.note('sharing_capable='+str(detail.get('capabilities',{}).get('sharing')))
   _cleanup(ctx,detail)
   ctx.click('windowClose',window='history');ctx.focus('outcome-browser',raise_window=False)
   ctx.expect(live_link_unavailable(ctx,'proton')['unavailable'],'fresh revoked link is unavailable');ctx.note('unavailable_link_count=1')
  finally:ctx.profile['windows']=previous
recipe=Recipe(('history.proton-sharing','confirm.revoke-link'),finish,destinations=('proton',),desktop_drawn=True,timeout=900)
rows=[]
try:rows=run_recipes(ctx,[recipe],env,out,'tier1-phase-b',out/'coverage.json')
finally:
 env['cleanup_errors']=target.close()
 scan=finish_evidence(ctx.evidence,out,env,rows,'tier1-phase-b',out/'coverage.json')
print(json.dumps({'scanPassed':scan['passed'],'statuses':[r.functional for r in rows]}))
