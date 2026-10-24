"""Bounded offscreen production CLI + real user-manager/desktop-parser checks; no desktop acceptance claim."""
import os,json,subprocess,time,tempfile,shutil,hashlib,signal
from pathlib import Path
import gi
gi.require_version('Gio','2.0')
from gi.repository import Gio
REPO=Path('/home/patrick/work/pg382');OUT=REPO/'work/i382-race2';OUT.mkdir(mode=0o700)
run=Path(tempfile.mkdtemp(prefix='i382-',dir='/tmp'));bindir=run/'bin';bindir.mkdir(mode=0o700)
for name in ['putgd','putgd-cli','putgd-ui']:shutil.copy2(REPO/'build/dev'/name,bindir/name)
master_launcher=REPO/'build/dev/packaging/linux/putgd-launcher';shutil.copy2(master_launcher,bindir/'putgd-launcher');(bindir/'putgd-launcher').chmod(0o700)
env={'HOME':str(Path.home()),'USER':os.environ['USER'],'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','QT_QPA_PLATFORM':'offscreen','DBUS_SESSION_BUS_ADDRESS':'unix:path=/run/user/'+str(os.getuid())+'/bus'}
for k,n in [('XDG_CONFIG_HOME','c'),('XDG_DATA_HOME','d \' % $ ` " \\ \t __PUTGD_KIND__'),('XDG_STATE_HOME','s'),('XDG_CACHE_HOME','a'),('XDG_RUNTIME_DIR','r')]:
 p=run/n;p.mkdir(mode=0o700);env[k]=str(p)
socket=run/'r/putgd/session.sock'
subprocess.run(['python3',str(REPO/'tools/diagnostics/check_socket_paths.py'),'--socket',str(socket),'/run/user/'+str(os.getuid())+'/bus'],check=True,stdout=(OUT/'socket-preflight.log').open('w'))
config=json.loads((REPO/'config/development.example.json').read_text());config['profiles']=[]
for a in config['actions'].values():a.update(destination='local',profileId=None,shortcut='')
config['preferences'].update(launchAtLogin=True,notifications=False,unlockCredentialsAtStartup=False,updates={'check':False,'channel':'build'});config['archive'].update(keepUploads=True,root=str(run/'archive'))
cf=run/'c/putgd/config.json';cf.parent.mkdir(mode=0o700);cf.write_text(json.dumps(config)+'\n');cf.chmod(0o600)
entry=run/'c/autostart/putgd.desktop';script=Path(env['XDG_DATA_HOME'])/'putgd/login/start.sh';launcher=bindir/'putgd-launcher';processes=[]
def wait(fn,seconds=30):
 end=time.monotonic()+seconds;last=None
 while time.monotonic()<end:
  try:
   x=fn()
   if x:return x
  except Exception as e:last=str(e)
  time.sleep(.1)
 raise TimeoutError(last)
def cli(method,params=None):
 r=subprocess.run([str(bindir/'putgd-cli'),'--socket',str(socket),method,json.dumps(params or {})],env=env,text=True,capture_output=True,timeout=20)
 d=json.loads(r.stdout)
 if not d.get('ok'):
  (OUT/'last-cli-error.json').write_text(json.dumps({'argv':r.args,'reply':d,'socketExists':socket.exists(),'socketPath':str(socket),'returncode':r.returncode},indent=2))
 assert d.get('ok'),d;return d['result']
def start(label):
 with (OUT/(label+'.log')).open('w') as log:p=subprocess.Popen([str(bindir/'putgd'),'--background'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 processes.append(p)
 def applied():
  d=cli('ui.snapshot')['diagnostics']['loginStartup'];return d if d.get('enabled') and d.get('applied') else None
 time.sleep(.5)
 import socket as unix_socket
 probe=unix_socket.socket(unix_socket.AF_UNIX,unix_socket.SOCK_STREAM);probe.settimeout(2)
 try:probe.connect(str(socket));(OUT/'raw-probe.txt').write_text('Connected to '+str(socket))
 except OSError as e:(OUT/'raw-probe.txt').write_text(repr(e)+' '+str(socket))
 finally:probe.close()
 return wait(applied)
def quit():
 cli('ui.quit');wait(lambda:all(p.poll() is not None for p in processes))
def call(a):
 return subprocess.run(a,env=env,text=True,capture_output=True,timeout=20)
def launch_entry():
 old=dict(os.environ)
 try:
  os.environ.clear();os.environ.update(env)
  app=Gio.DesktopAppInfo.new_from_filename(str(entry));assert app
  assert app.launch([],None)
 finally:os.environ.clear();os.environ.update(old)
result={'scope':'Bounded offscreen settings-change/uninstall race probe; no graphical acceptance','run':str(run)}
try:
 start('race');meta=cli('settings.get')['metadata'];before_pref=json.loads(cf.read_text())['preferences']['launchAtLogin']
 launcher.unlink();(bindir/'putgd').unlink()
 patch=cli('settings.patch',{'patch':{'preferences':{'notifications':True}},'expectedRevision':meta['observedRevision']})
 time.sleep(4)
 snapshot=cli('ui.snapshot')['diagnostics']['loginStartup']
 result.update({'entryRemains':entry.exists(),'scriptRemains':script.exists(),'savedLaunchPreference':json.loads(cf.read_text())['preferences']['launchAtLogin'],'loginStartup':snapshot,'cleanupPassed':not entry.exists() and not script.exists()});assert result['cleanupPassed'],result
finally:
 if not (bindir/'putgd').exists():shutil.copy2(REPO/'build/dev/putgd',bindir/'putgd')
 if not launcher.exists():shutil.copy2(master_launcher,launcher);launcher.chmod(0o700)
 try:
  metadata=cli('settings.get')['metadata'];cli('settings.patch',{'patch':{'preferences':{'launchAtLogin':False}},'expectedRevision':metadata['observedRevision']});wait(lambda:not entry.exists() and not script.exists());quit()
 except Exception as e:result['cleanupErrorType']=type(e).__name__
 if script.exists():call(['/bin/sh',str(script),'--stop-watch'])
 for p in processes:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=10)
 result['ownedProcessExitCodes']=[p.poll() for p in processes]
 (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
