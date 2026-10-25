"""Real user-manager AppImage-directory watch with synthetic ELF fixtures; no AppImage/desktop acceptance."""
from pathlib import Path
import os,sys,json,subprocess,tempfile,time,shutil,signal,importlib.util
REPO=Path('/home/patrick/work/pg382');OUT=REPO/'work/i382-appimage-live1';OUT.mkdir(mode=0o700)
run=Path(tempfile.mkdtemp(prefix='i382a-',dir='/tmp'));appdir=run/'app';binpath=appdir/'usr/bin';binpath.mkdir(parents=True)
for n in ['putgd','putgd-cli','putgd-ui']:shutil.copy2(REPO/'build/dev'/n,binpath/n)
shutil.copy2(REPO/'build/dev/packaging/linux/putgd-launcher',binpath/'putgd-launcher');(binpath/'putgd-launcher').chmod(0o700)
images=run/"Applications ' % $";images.mkdir(mode=0o700)
spec=importlib.util.spec_from_file_location('fixtures',REPO/'tests/unit/test_appimage_login_start.py');f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)
source=run/'fixture.c';source.write_text(f.FIXTURE.replace('const char* path = getenv(variable);','const char* path = getenv(variable); if (!path && strcmp(variable,"PG_RECEIPT")==0) path='+json.dumps(str(run/'unexpected-start'))+';'))
fixture=run/'fixture';subprocess.run(['cc','-O1','-o',str(fixture),str(source)],check=True)
infofile=run/'upd-info';infofile.write_text(f.NIGHTLY+'\0');subprocess.run(['objcopy','--add-section','.upd_info='+str(infofile),str(fixture)],check=True)
b=bytearray(fixture.read_bytes());b[8:11]=b'AI\x02';fixture.write_bytes(b)
def image(name):
 p=images/name;shutil.copy2(fixture,p);p.chmod(0o700);Path(str(p)+'.info').write_text(f.NIGHTLY+'\n');Path(str(p)+'.version').write_text('26.1024.1183-dev+gb15f47d589e8\n');return p
configured=image('putgd-26.1024.1183-dev+gb15f47d589e8-x86_64.AppImage')
env={'HOME':str(Path.home()),'USER':os.environ['USER'],'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','QT_QPA_PLATFORM':'offscreen','DBUS_SESSION_BUS_ADDRESS':'unix:path=/run/user/'+str(os.getuid())+'/bus','APPIMAGE':str(configured),'APPDIR':str(appdir)}
for k,n in [('XDG_CONFIG_HOME','c'),('XDG_DATA_HOME','d'),('XDG_STATE_HOME','s'),('XDG_CACHE_HOME','a'),('XDG_RUNTIME_DIR','r')]:
 p=run/n;p.mkdir(mode=0o700);env[k]=str(p)
sock=run/'r/putgd/session.sock';subprocess.run(['python3',str(REPO/'tools/diagnostics/check_socket_paths.py'),'--socket',str(sock),'/run/user/'+str(os.getuid())+'/bus'],stdout=(OUT/'socket-preflight.log').open('w'),check=True)
c=json.loads((REPO/'config/development.example.json').read_text());c['profiles']=[]
for a in c['actions'].values():a.update(destination='local',profileId=None,shortcut='')
c['preferences'].update(launchAtLogin=True,notifications=False,unlockCredentialsAtStartup=False,updates={'check':False,'channel':'build'});c['archive'].update(keepUploads=True,root=str(run/'archive'))
cf=run/'c/putgd/config.json';cf.parent.mkdir(mode=0o700);cf.write_text(json.dumps(c)+'\n');cf.chmod(0o600)
entry=run/'c/autostart/putgd.desktop';script=run/'d/putgd/login/start.sh';ps=[]
def wait(fn,seconds=25):
 end=time.monotonic()+seconds;last=None
 while time.monotonic()<end:
  try:
   v=fn()
   if v:return v
  except Exception as e:last=str(e)
  time.sleep(.1)
 raise TimeoutError(last)
def cli(m,p=None):
 r=subprocess.run([str(binpath/'putgd-cli'),'--socket',str(sock),m,json.dumps(p or {})],env=env,capture_output=True,text=True,timeout=20);d=json.loads(r.stdout);assert d['ok'],d;return d['result']
def start(label):
 with (OUT/(label+'.log')).open('w') as log:p=subprocess.Popen([str(binpath/'putgd'),'--background'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 ps.append(p)
 def ready():
  d=cli('ui.snapshot')['diagnostics']['loginStartup'];return d if d.get('enabled') and d.get('applied') else None
 wait(ready)
def quit():cli('ui.quit');wait(lambda:all(p.poll() is not None for p in ps))
result={'scope':__doc__,'host':os.uname().nodename,'run':str(run)}
try:
 start('initial');quit();before=cf.read_bytes()
 assert "kind='appimage'" in script.read_text() and f.NIGHTLY in script.read_text()
 unit='putgd-login-startup-'+next(l.split('=',1)[1] for l in entry.read_text().splitlines() if l.startswith('X-putgd-StartupId='))
 props=subprocess.check_output(['systemctl','--user','show',unit+'.path','-p','Paths','-p','ActiveState'],text=True);assert str(images) in props and 'ActiveState=active' in props;result['unitPaths']=props
 configured.unlink();wait(lambda:not entry.exists() and not script.exists(),12);assert cf.read_bytes()==before;result['lastImageRemovalCleanedManagedFiles']=True
 image(configured.name);start('replacement');quit();replacement=image('putgd-26.1024.1184-dev+gfixture-x86_64.AppImage');configured.unlink();time.sleep(3)
 assert entry.exists() and script.exists();assert not (run/'unexpected-start').exists();assert cf.read_bytes()==before;result['eligibleReplacementPreservedStartupWithoutLaunchingAnything']=True
 replacement.unlink();wait(lambda:not entry.exists() and not script.exists(),12);assert cf.read_bytes()==before;result['removingFinalReplacementCleanedManagedFiles']=True;result['passed']=True
finally:
 if script.exists():subprocess.run(['/bin/sh',str(script),'--stop-watch'],env=env,capture_output=True,timeout=15)
 for p in ps:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait(timeout=10)
 result['ownedProcessExitCodes']=[p.poll() for p in ps]
 (OUT/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
