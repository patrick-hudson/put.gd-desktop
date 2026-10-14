import sys,json,time
from pathlib import Path
from PIL import Image
SRC=Path('/home/patrick/work/put.gd/work/i33');ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(SRC/'tools/lab'));import lab
r=Path(sys.argv[1]).resolve();out=ROOT/r.name/('views'+(sys.argv[2] if len(sys.argv)>2 else ''));out.mkdir(mode=0o700,exist_ok=False)
if lab.state(r)['profile']=='gnome-wayland':
 old=lab.guest_session
 def session(run,body):return old(run,'export WAYLAND_DISPLAY=wayland-0\n'+body)
 lab.guest_session=session
receipt={'resident':lab.verify_running_app(r),'steps':[],'scope':'native Settings/History/Activity reopen and last-window helper exit'}
for cycle in range(2):
 for view in ('settings','history','activity'):
  ack=lab.cli(r,'ui.show',{'view':view});time.sleep(1.5);snap=lab.cli(r,'ui.snapshot');assert snap['diagnostics']['ui']['pid']>0
  p=Path(lab.screenshot(r,f'{view}-{cycle}')['path']);im=Image.open(p);im.save(out/f'{view}-{cycle}.png');p.unlink();receipt['steps'].append({'cycle':cycle,'view':view,'acknowledgement':ack,'snapshot':snap})
  lab.key(r,['alt','f4'])
  for _ in range(100):
   d=lab.cli(r,'ui.snapshot')['diagnostics']
   if d['ui']['pid']==0:break
   time.sleep(.1)
  assert d['ui']['pid']==0,'last_window_helper_did_not_exit'
receipt['passed']=True;(out/'results.json').write_text(json.dumps(receipt,indent=2));print(json.dumps({'passed':True,'reopens':6,'helperExitedAfterEveryClose':True}))
