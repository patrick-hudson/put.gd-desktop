"""Bounded offscreen production CLI + real user-manager/desktop-parser checks; no desktop acceptance claim."""
import os,json,subprocess,time,tempfile,shutil,hashlib,signal
from pathlib import Path
import gi
gi.require_version('Gio','2.0')
from gi.repository import Gio
REPO=Path('/home/patrick/work/pg382');OUT=REPO/'work/i382-monitor-cli1';OUT.mkdir(mode=0o700)
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

result={'scope':'Bounded offscreen production CLI plus real user-manager regression; not desktop or package acceptance','run':str(run)}
def props(unit):
 r=call(['systemctl','--user','show',unit+'.path','-p','ActiveState','-p','ActiveEnterTimestampMonotonic','-p','LoadState']);assert r.returncode==0 or 'LoadState=not-found' in r.stdout,r.stderr;return r.stdout
def unit_name():return 'putgd-login-startup-'+next(l.split('=',1)[1] for l in entry.read_text().splitlines() if l.startswith('X-putgd-StartupId='))
def patch_preferences(patch):
 metadata=cli('settings.get')['metadata'];reply=cli('settings.patch',{'patch':{'preferences':patch},'expectedRevision':metadata['observedRevision']})
 def done():
  snap=cli('ui.snapshot');d=snap['diagnostics'];status=d['loginStartup'];return snap if d['activeOperations']==0 and status.get('enabled')==patch.get('launchAtLogin',True) and status.get('applied') is True else None
 return reply,wait(done)
started=int(time.time())-1;units=[]
try:
 start('unchanged');unit=unit_name();units.append(unit);before=props(unit);assert 'ActiveState=active' in before
 for n in range(10):
  patch_preferences({'notifications':bool(n%2)});after=props(unit);assert after==before,(before,after)
 result['unchangedWatchPreservedAcrossTenSettingsPatches']=True;result['watchBefore']=before
 for n in range(3):
  patch_preferences({'launchAtLogin':False});assert not entry.exists() and not script.exists()
  patch_preferences({'launchAtLogin':True});assert entry.exists() and script.exists()
 result['threeDisableEnableCyclesPassed']=True
 # A different launcher location changes the installation ID: retire the old
 # watch before overwriting its script, then arm only the replacement watch.
 old_unit=unit;quit();old_bindir=bindir;bindir=run/'bin2';bindir.mkdir(mode=0o700)
 for name in ['putgd','putgd-cli','putgd-ui']:shutil.copy2(REPO/'build/dev'/name,bindir/name)
 launcher=bindir/'putgd-launcher';shutil.copy2(master_launcher,launcher);launcher.chmod(0o700)
 start('new-installation');unit=unit_name();units.append(unit);assert unit!=old_unit
 wait(lambda:'LoadState=not-found' in props(old_unit));assert 'ActiveState=active' in props(unit)
 result['installationChangeStoppedOnlyTheOldWatch']=True
 launcher.unlink();(bindir/'putgd').unlink();metadata=cli('settings.get')['metadata'];cli('settings.patch',{'patch':{'preferences':{'notifications':False}},'expectedRevision':metadata['observedRevision']})
 wait(lambda:not entry.exists() and not script.exists(),12);snapshot=cli('ui.snapshot')['diagnostics']['loginStartup'];assert json.loads(cf.read_text())['preferences']['launchAtLogin'] is True
 result['uninstallSettingsRaceCleanedFilesAndPreservedPreference']=True;result['raceDiagnostic']=snapshot
finally:
 if not (bindir/'putgd').exists():shutil.copy2(REPO/'build/dev/putgd',bindir/'putgd')
 if not launcher.exists():shutil.copy2(master_launcher,launcher);launcher.chmod(0o700)
 try:
  metadata=cli('settings.get')['metadata'];cli('settings.patch',{'patch':{'preferences':{'launchAtLogin':False}},'expectedRevision':metadata['observedRevision']});wait(lambda:not entry.exists() and not script.exists());quit()
 except Exception as e:result['cleanupErrorType']=type(e).__name__
 if script.exists():call(['/bin/sh',str(script),'--stop-watch'])
 for p in processes:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=10)
 time.sleep(.3)
 journal=call(['journalctl','--user','--since','@'+str(started),'--no-pager','-o','short-iso',*[v for unit in units for v in ['-u',unit+'.path','-u',unit+'.service']]])
 errors=[l for l in journal.stdout.splitlines() if __import__('re').search('failed|refusing|cannot',l,__import__('re').I)]
 result['journal']=journal.stdout;result['journalErrors']=errors;result['ownedProcessExitCodes']=[p.poll() for p in processes];result['pass']=all(result.get(k) for k in ['unchangedWatchPreservedAcrossTenSettingsPatches','threeDisableEnableCyclesPassed','installationChangeStoppedOnlyTheOldWatch','uninstallSettingsRaceCleanedFilesAndPreservedPreference']) and not errors and not result.get('cleanupErrorType')
 (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='journal'},indent=2),flush=True)
assert result['pass'],result['journalErrors']
