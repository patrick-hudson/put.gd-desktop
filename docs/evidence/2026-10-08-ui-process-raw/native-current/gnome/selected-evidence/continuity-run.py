from pathlib import Path
from PIL import Image
import sys,json,time,datetime,shlex
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';dest=out/'continuity';params=json.loads((dest/'parameters.json').read_text());assert not (dest/'accepted.json').exists()
ui=lab.cli(run,'ui.snapshot')['diagnostics']['ui'];assert ui['pid']==params['helperPid'] and ui['state']=='ready';assert lab.verify_running_app(run)==params['resident']
observer=(root/'work/u11/native/cinnamon/continuity-run1/observer.py').read_text();(dest/'observer.py').write_text(observer)
lab.guest_session(run,'mkdir "$HOME/putgd-lab/u11-continuity"\n');lab.put(run,dest/'observer.py','/home/lab/putgd-lab/u11-continuity/observer.py');lab.put(run,dest/'parameters.json','/home/lab/putgd-lab/u11-continuity/parameters.json');lab.guest_session(run,'nohup python3 "$HOME/putgd-lab/u11-continuity/observer.py" > "$HOME/putgd-lab/u11-continuity/observer.log" 2>&1 < /dev/null &\n')
# The acceptance reply precedes the normal Close gesture, so this proves already accepted work.
accepted=lab.cli(run,'action.invoke',{'action':'clipboard'});lab.write_json(dest/'accepted.json',{'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'reply':accepted,'helperPidBeforeClose':params['helperPid']})
lab.key(run,['alt','f4']);end=time.monotonic()+12
while True:
 d=lab.cli(run,'ui.snapshot')['diagnostics']
 if d['ui']['pid']==0:break
 if time.monotonic()>end:raise RuntimeError('Normal helper close timeout')
 time.sleep(.1)
assert d['ui']['lastExit']['status']=='normal' and d['ui']['lastExit']['code']==0
w=lab.screenshot(run,'u11-gnome-continuity-after-helper-close');Image.open(w['path']).save(dest/'after-helper-close.png')
end=time.monotonic()+65
while True:
 try:lab.get(run,'/home/lab/putgd-lab/u11-continuity/result.json',dest/'observer-result.json');break
 except lab.LabError:
  if time.monotonic()>end:raise
  time.sleep(.2)
r=json.loads((dest/'observer-result.json').read_text());op=r['operation'];assert r['failure'] is None and op and op['id']==accepted['operationId'];assert op['state']=='completed' and op['copyStatus']=='copied' and len(op['items'])==2 and all(i['local']['status']=='saved' for i in op['items'])
paths=[i['local']['path'] for i in op['items']];assert all(p.startswith('/home/lab/putgd-lab/archive/') for p in paths)
hashes=lab.ssh(run,'sha256sum '+' '.join(shlex.quote(p) for p in paths)+'\n');sizes=lab.ssh(run,"stat -c '%s' "+' '.join(shlex.quote(p) for p in paths)+'\n');assert hashes.splitlines()[0].split()[0]==params['payload']['sha256'] and int(sizes.splitlines()[0])==params['payload']['size'];assert lab.verify_running_app(run)==params['resident']
r.update({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'acceptedBeforeClose':True,'normalExit':d['ui']['lastExit'],'residentUnchanged':params['resident'],'artifactSha256Output':hashes,'artifactSizes':sizes,'verdict':'pass' if r['activeOperationObservedAfterHelperExit'] else 'inconclusive-fast-operation'});lab.write_json(dest/'receipt.json',r);print(json.dumps({'verdict':r['verdict'],'operationId':op['id'],'activeAfterHelperExit':r['activeOperationObservedAfterHelperExit']}))
