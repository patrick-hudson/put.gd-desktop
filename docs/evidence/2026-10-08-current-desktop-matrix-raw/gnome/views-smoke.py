#!/usr/bin/env python3
"""Visible native view lifecycle smoke; QMP witnesses supplement CLI/PID checks."""
from pathlib import Path
import sys,json,time,datetime,hashlib
from PIL import Image
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';out=Path(__file__).resolve().parent/'views';out.mkdir(exist_ok=False)
result={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Two visible GNOME cycles opening/closing Settings, History and Activity; no settings edits or memory metrics','runnerSha256':lab.hash_file(root/'tools/lab/lab.py'),'scriptSha256':lab.hash_file(__file__),'cases':[],'proceduralPassed':False,'visualReview':'pending'}
initial=lab.verify_running_app(run);result['initialRunningApp']=initial
lab.guest_session(run,"pkill -x putgd-desktop-f || true\npkill -f '^python3 /home/[^/]+/putgd-lab/gtk_clipboard_fixture.py --output ' || true\n")
time.sleep(.5)
for cycle in range(1,3):
 for view in ('settings','history','activity'):
  started=time.monotonic();reply=lab.cli(run,'ui.show',{'view':view});time.sleep(1.5)
  witness=lab.screenshot(run,f'view-{cycle}-{view}-open');png=out/f'{cycle}-{view}-open.png';Image.open(witness['path']).save(png)
  lab.key(run,['alt','f4']);time.sleep(.8)
  closed=lab.screenshot(run,f'view-{cycle}-{view}-closed');closedPng=out/f'{cycle}-{view}-closed.png';Image.open(closed['path']).save(closedPng)
  current=lab.verify_running_app(run);assert current['pid']==initial['pid'] and current['startTicks']==initial['startTicks']
  result['cases'].append({'cycle':cycle,'view':view,'showReply':reply,'elapsedSeconds':time.monotonic()-started,'openedWitness':{'path':str(png),'sha256':lab.hash_file(png)},'closedWitness':{'path':str(closedPng),'sha256':lab.hash_file(closedPng)},'residentAfterClose':current})
  lab.write_json(out/'results.json',result)
result['proceduralPassed']=True;lab.write_json(out/'results.json',result);print(json.dumps({'proceduralPassed':True,'visualReview':'pending','cases':len(result['cases'])}))
