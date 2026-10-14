import argparse,json,sys,shutil,time,subprocess
from pathlib import Path
SRC=Path('/home/patrick/work/put.gd/work/i33');ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(SRC/'tools/lab'));import lab
p=argparse.ArgumentParser();p.add_argument('run');p.add_argument('scenario');p.add_argument('--client',default='qt');p.add_argument('--attempt',default='');a=p.parse_args();run=Path(a.run).resolve();out=ROOT/run.name/(a.scenario+'-'+a.client+a.attempt);out.mkdir(parents=True,mode=0o700,exist_ok=False)
if lab.state(run)['profile']=='gnome-wayland':
 old=lab.guest_session
 def session(r,body):return old(r,'test -S /run/user/1000/wayland-0\nexport WAYLAND_DISPLAY=wayland-0\n'+body)
 lab.guest_session=session
pre=json.loads(subprocess.check_output(['python3',str(SRC/'tools/diagnostics/check_socket_paths.py'),'--socket',str(run/'qmp.sock'),'/run/user/1000/putgd/session.sock','/run/user/1000/bus','/run/user/1000/wayland-0','/tmp/.X11-unix/X0','/run/dbus/system_bus_socket'],text=True));(out/'preflight.json').write_text(json.dumps(pre));assert pre['passed']
args=argparse.Namespace(run=str(run),scenario=a.scenario,clipboard_client=a.client,timeout=90,keep=True,settings=None,history=False)
result=lab.fixture_run(args)
for q in run.iterdir():
 if q.is_file() and (q.suffix in ('.ppm','.xml') or q.name in ('results.json','parameters.json','ready.json','gtk-ready.json','paste.json','diagnostics.json','versions.json','package.json')):shutil.copyfile(q,out/q.name)
print(json.dumps({'scenario':a.scenario,'client':a.client,'ok':result['ok'],'path':str(out)}));raise SystemExit(0 if result['ok'] else 1)
