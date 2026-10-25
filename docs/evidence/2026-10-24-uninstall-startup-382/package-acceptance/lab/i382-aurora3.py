#!/usr/bin/env python3
"""Bounded production-package startup/uninstall acceptance driver. No cloud/credentials."""
import os,sys,json,time,hashlib,subprocess,shutil,re,signal,importlib.util,contextlib,io
from pathlib import Path
BASE=Path.home()/'work/p382n'; MODE=sys.argv[1]; ACTION=sys.argv[2]; ARGS=sys.argv[3:]
R=BASE/MODE; UID=os.getuid(); HOME=Path.home(); ENTRY=HOME/'.config/autostart/putgd.desktop'; SCRIPT=HOME/'.local/share/putgd/login/start.sh'; CONFIG=HOME/'.config/putgd/config.json'
spec=importlib.util.spec_from_file_location('session_helpers',BASE/'r490.py'); h=importlib.util.module_from_spec(spec); spec.loader.exec_module(h)
def iso():return time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())
def sha(p):return hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def cmd(a,env=None,timeout=90):
 r=subprocess.run(a,env=env,text=True,capture_output=True,timeout=timeout,stdin=subprocess.DEVNULL)
 return {'command':a,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
def record(label,data):
 data={'at':iso(),'mode':MODE,'host':os.uname().nodename,**data};path=R/(label+'.json');attempt=1
 while path.exists():
  attempt+=1;path=R/(label+'.attempt'+str(attempt)+'.json')
 data['evidenceFile']=path.name;path.write_text(json.dumps(data,indent=2)+'\n');print(json.dumps(data),flush=True);return data
def env():
 e=h.session_env()[0]
 for key in ('XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME','XDG_STATE_HOME','APPIMAGE','APPDIR','QT_QPA_PLATFORM'):e.pop(key,None)
 return e
def socket_preflight():
 e=env();runtime=Path(e['XDG_RUNTIME_DIR']);paths=[runtime/'putgd/session.sock',runtime/'bus']
 if e.get('WAYLAND_DISPLAY'):paths.append(Path(e['WAYLAND_DISPLAY']) if e['WAYLAND_DISPLAY'].startswith('/') else runtime/e['WAYLAND_DISPLAY'])
 if re.fullmatch(r':[0-9]+(?:\.[0-9]+)?',e.get('DISPLAY','')):paths.append(Path('/tmp/.X11-unix/X'+e['DISPLAY'][1:].split('.')[0]))
 paths=sorted({str(p.resolve()) for p in paths});r=cmd([sys.executable,str(BASE/'check_socket_paths.py'),'--socket',*paths]);assert not r['exit'],r;return r

def exe():return '/usr/bin/putgd-launcher' if MODE=='native' else str(R/'Applications'/json.loads((R/'meta.json').read_text())['imageName'])
def cli(method,params=None,timeout=30):
 e=env();c='/usr/bin/putgd-cli'
 if MODE=='appimage':
  ps=[p for p in h.residents() if Path(p['exe']).name=='putgd'];assert len(ps)==1,ps
  c=str(Path(ps[0]['exe']).parent/'putgd-cli');procenv=h.environ(ps[0]['pid'])
  for k in ('LD_LIBRARY_PATH','APPDIR','APPIMAGE'): 
   if k in procenv:e[k]=procenv[k]
 r=cmd([c,method,json.dumps(params or {})],e,timeout)
 try:d=json.loads(r['stdout'])
 except ValueError:raise RuntimeError(str(r))
 assert d.get('ok'),d
 return d['result']
def wait(f,seconds=60):
 end=time.monotonic()+seconds;last=None
 while time.monotonic()<end:
  try:
   v=f()
   if v:return v
  except Exception as x:last=str(x)
  time.sleep(.5)
 raise TimeoutError(str(last))
def personal():
 found=[]
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   if p.stat().st_uid!=UID:continue
   name=(p/'comm').read_text().strip().lower()
   if any(x in name for x in ('firefox','chrome','chromium','chatgpt','claude','codex','t3','terminal','konsole','btop')):
    found.append({'pid':int(p.name),'comm':name,'cgroup':(p/'cgroup').read_text().strip()})
  except OSError:pass
 return found
def pref():return json.loads(CONFIG.read_text())['preferences']['launchAtLogin'] if CONFIG.exists() else None
def state(snapshot=True):
 sid,props=h.graphical_session();ps=h.residents();d={'sessionId':sid,'session':props,'residents':ps,'configSha256':sha(CONFIG) if CONFIG.exists() else None,'preference':pref(),'entry':ENTRY.read_text() if ENTRY.exists() else None,'entrySha256':sha(ENTRY) if ENTRY.exists() else None,'scriptSha256':sha(SCRIPT) if SCRIPT.exists() else None,'personalApps':personal(),'load':Path('/proc/loadavg').read_text().strip()}
 for p in ps:
  maps=Path('/proc/'+str(p['pid'])+'/maps').read_text();p['exeSha256']=sha('/proc/'+str(p['pid'])+'/exe');p['appImageEnvironment']={k:v for k,v in h.environ(p['pid']).items() if k in ('APPIMAGE','APPDIR')};p['qtPlatformPlugins']=sorted(set(re.findall(r'/\S*libq(?:wayland[^/\s]*|xcb|offscreen)\.so',maps)))
 if ps and snapshot:d['snapshot']=cli('ui.snapshot')
 return d
if ACTION=='prepare':
 assert MODE in ('native','appimage')
 assert not R.exists();R.mkdir(mode=0o700,parents=True)
 assert not h.residents(),h.residents();assert not personal(),personal()
 paths=[HOME/'.config/putgd',HOME/'.local/share/putgd',HOME/'.cache/putgd',ENTRY]
 assert not any(p.exists() for p in paths),[str(p) for p in paths if p.exists()]
 sock=f'/run/user/{UID}/putgd/session.sock';assert len(sock.encode())<=107
 baseline={'pathsAbsent':[str(p) for p in paths],'socket':{'path':sock,'bytes':len(sock.encode()),'limit':107},'session':state(False),'os':Path('/etc/os-release').read_text(),'sourceCommit':'d5b561ddb94f8255b8bc754c1382552d983a6fa9'}
 oldstate=HOME/'.local/state/putgd'
 if oldstate.exists():shutil.move(str(oldstate),R/'preexisting-state');baseline['preservedState']=str(R/'preexisting-state')
 template=json.loads((BASE/'template.json').read_text());template['profiles']=[]
 for a in template['actions'].values():a.update(destination='local',profileId=None,shortcut='')
 template['archive'].update(keepUploads=True,root=str(R/'archive'));template['preferences'].update(launchAtLogin=False,notifications=False,unlockCredentialsAtStartup=False,updates={'check':False,'channel':'build'})
 CONFIG.parent.mkdir(mode=0o700,parents=True);CONFIG.write_text(json.dumps(template)+'\n');CONFIG.chmod(0o600)
 if MODE=='appimage':
  image=Path(ARGS[0]);(R/'Applications').mkdir(mode=0o700);target=R/'Applications'/image.name;shutil.copyfile(image,target);target.chmod(0o700)
  baseline['imageSocketPreflight']=socket_preflight();information=cmd([str(target),'--appimage-updateinformation'],env())
  baseline['appImageUpdateInformation']=information
  (R/'meta.json').write_text(json.dumps({'imageName':image.name,'imageSha256':sha(image),'masterImage':str(image),'updateInformation':information}))
 record('prepare',baseline)
elif ACTION=='pkg-sim':
 install=ARGS[0]=='install';pkg=ARGS[1] if install else 'putgd'
 if Path('/etc/apt').exists():a=['apt-get','-s','install' if install else 'remove',pkg]
 else:a=['sudo','-n','dnf','--assumeno','--setopt=localpkg_gpgcheck=0','--setopt=clean_requirements_on_remove=False','--setopt=install_weak_deps=False','install' if install else 'remove',pkg]
 r=cmd(a,timeout=600);record('pkg-sim-'+ARGS[2],r)
elif ACTION=='pkg-apply':
 install=ARGS[0]=='install';pkg=ARGS[1] if install else 'putgd';assert pkg=='putgd' or Path(pkg).name.startswith('putgd')
 if Path('/etc/apt').exists():a=['sudo','-n','apt-get','-y','install' if install else 'remove',pkg]
 else:a=['sudo','-n','dnf','-y','--setopt=localpkg_gpgcheck=0','--setopt=clean_requirements_on_remove=False','--setopt=install_weak_deps=False','install' if install else 'remove',pkg]
 r=cmd(a,timeout=900);record('pkg-'+ARGS[2],r);assert not r['exit'],r
elif ACTION=='launch':
 assert not h.residents(),h.residents();e=env();preflight=socket_preflight()
 with (R/('launch-'+ARGS[0]+'.log')).open('w') as log:p=subprocess.Popen([exe(),'--background'],env=e,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 def ready():
  snap=cli('ui.snapshot');d=snap.get('diagnostics',{}).get('loginStartup',{});return snap if d.get('enabled') is pref() and d.get('applied') is True else None
 snap=wait(ready)
 verify=cmd(['dpkg','--verify','putgd'] if Path('/etc/apt').exists() else ['rpm','-V','putgd']) if MODE=='native' else {'imageSha256':sha(exe()),'expectedImageSha256':json.loads((R/'meta.json').read_text())['imageSha256'],'exit':0}
 assert not verify['exit'],verify
 if MODE=='appimage':assert verify['imageSha256']==verify['expectedImageSha256'],verify
 record('launch-'+ARGS[0],{'launchedPid':p.pid,'socketPreflight':preflight,'installedPackageVerification':verify,'state':state()})
elif ACTION=='enable' or ACTION=='disable':
 enabled=ACTION=='enable';metadata=cli('settings.get')['metadata'];patch=cli('settings.patch',{'patch':{'preferences':{'launchAtLogin':enabled}},'expectedRevision':metadata['observedRevision']})
 def applied():
  s=cli('ui.snapshot');v=s.get('diagnostics',{}).get('loginStartup',{});return s if v.get('enabled') is enabled and v.get('applied') is True and ENTRY.exists()==enabled and (enabled or not SCRIPT.exists()) else None
 snapshot=wait(applied);record(ACTION,{'patch':patch,'state':state(),'snapshot':snapshot})
elif ACTION=='show':
 preflight=socket_preflight();reply=cli('ui.show',{'view':'settings'});wait(lambda:any(Path(p['exe']).name=='putgd-ui' for p in h.residents()),30);time.sleep(1);record('show-'+ARGS[0],{'reply':reply,'socketPreflight':preflight,'state':state()})
elif ACTION=='quit':
 ps=h.residents()
 if ps:reply=cli('ui.quit');wait(lambda:not h.residents(),30)
 else:reply=None
 if MODE=='appimage':time.sleep(1)
 record('quit-'+ARGS[0],{'reply':reply,'before':ps,'residents':h.residents()})
elif ACTION=='logout':
 assert not personal(),personal();sid,props=h.graphical_session();assert sid
 e,shell,_=h.session_env()
 if shell.startswith('cinnamon'):a=['cinnamon-session-quit','--logout','--no-prompt']
 elif shell=='gnome-shell':a=['gnome-session-quit','--logout','--no-prompt']
 else:a=['qdbus6' if shutil.which('qdbus6') else 'qdbus','org.kde.Shutdown','/Shutdown','org.kde.Shutdown.logout']
 dm_link=Path('/etc/systemd/system/display-manager.service')
 if dm_link.exists() and dm_link.resolve().stem=='plasmalogin':a=['sudo','-n','systemctl','stop','plasmalogin']
 (R/'last-session').write_text(sid);record('logout-'+ARGS[0],{'sessionId':sid,'command':a,'before':state(False)})
 if dm_link.exists() and dm_link.resolve().stem=='plasmalogin':
  stopped=cmd(a,e,60);assert not stopped['exit'],stopped
  # Plasma's user-manager workspace can otherwise outlive its display manager.
  # Retire the owned disposable guest desktop before starting the next session.
  targets=cmd(['systemctl','--user','stop','plasma-workspace-wayland.target','plasma-workspace.target','graphical-session.target'],e,60)
  with contextlib.redirect_stdout(io.StringIO()):record('logout-workspace-'+ARGS[0],{'displayManager':stopped,'workspaceTargets':targets})
  assert not targets['exit'],targets
 else:subprocess.Popen(a,env=e,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
 wait(lambda:h.graphical_session()[0]!=sid,60)
elif ACTION=='relogin':
 assert not personal(),personal()
 time.sleep(3) # Let the old graphical target finish stopping before the DM restart.
 link=Path('/etc/systemd/system/display-manager.service');dm=link.resolve().stem if link.exists() else next(d for d in ('lightdm','gdm','sddm','plasmalogin') if cmd(['systemctl','is-active',d])['stdout'].strip()=='active')
 r=cmd(['sudo','-n','systemctl','restart',dm]);record('relogin-'+ARGS[0],{'dm':dm,**r});assert not r['exit'],r
 wait(lambda:h.graphical_session()[0] and h.graphical_session()[0]!=(R/'last-session').read_text(),120)
elif ACTION=='check':
 expected=ARGS[0]=='present';label=ARGS[1];old=(R/'last-session').read_text() if (R/'last-session').exists() else None
 wait(lambda:h.graphical_session()[0] and h.graphical_session()[0]!=old,120)
 if expected:wait(lambda:len([p for p in h.residents() if Path(p['exe']).name=='putgd'])==1 and cli('ui.snapshot'),90)
 else:time.sleep(15)
 s=state(expected);sid=s['sessionId'];props=cmd(['loginctl','show-session',sid,'-p','Timestamp'])['stdout'];since=props.strip().partition('=')[2]
 s['journal']=cmd(['journalctl','--user','--since',since or '-3min','--no-pager','-o','short-iso'],timeout=45)
 s['journal']['stdout']='\n'.join(l for l in s['journal']['stdout'].splitlines() if re.search('putgd|put.gd',l,re.I))
 s['failedUnits']=cmd(['systemctl','--user','--failed','--no-legend','--no-pager'])
 startup_lines=[line for line in s['journal']['stdout'].splitlines() if re.search(r'app-(?:gnome-)?putgd|putgd-login-startup-|put\.gd login start:',line)]
 s['startupJournalErrors']=[line for line in startup_lines if re.search(r'failed|refusing|cannot|not executable',line,re.I)]
 s['expectedResident']=expected;s['newSession']=sid!=old;s['pass']=bool(s['residents'])==expected and sid!=old and not s['startupJournalErrors'] and not re.search('app-putgd|putgd-login-startup',s['failedUnits']['stdout'],re.I)
 if expected:
  assert s['snapshot']['build']['gitCommit']=='d5b561ddb94f8255b8bc754c1382552d983a6fa9',s['snapshot']['build']
  assert pref() is True and ENTRY.exists()
 elif label in ('active','inactive'):
  assert pref() is True and not ENTRY.exists() and not SCRIPT.exists()
  if MODE=='native':assert not Path('/usr/bin/putgd-launcher').exists()
  else:assert not Path(exe()).exists()
 elif label=='disabled':assert pref() is False and not ENTRY.exists() and not SCRIPT.exists()
 if expected:s['runningVersion']=cmd([exe(),'--version-json'],env())
 record('check-'+label,s);assert s['pass'],s
elif ACTION=='memory':
 idle=int(ARGS[0]);start=state();ps=[p for p in start['residents'] if Path(p['exe']).name=='putgd'];assert len(ps)==1
 pid=ps[0]['pid'];samples=[];began=time.monotonic()
 while True:
  status=Path(f'/proc/{pid}/status').read_text();rss=int(re.search(r'VmRSS:\s+(\d+)',status)[1])/1024;samples.append({'seconds':round(time.monotonic()-began,2),'rssMiB':rss})
  remaining=idle-(time.monotonic()-began)
  if remaining<=0:break
  time.sleep(min(10,remaining))
 roll=Path(f'/proc/{pid}/smaps_rollup').read_text();maps=Path(f'/proc/{pid}/maps').read_text();pss=int(re.search(r'^Pss:\s+(\d+)',roll,re.M)[1])/1024
 bad=sorted(set(re.findall(r'/\S*(?:libgtk|libgdk-3|libgdk_pixbuf|libqgtk3|libKF|KDEPlasmaPlatformTheme|plasma-integration|libssl|libcrypto|libgnutls|libqopensslbackend|libQt6Network)\S*',maps)))
 peak=max(s['rssMiB'] for s in samples)
 record('memory',{'idleSeconds':idle,'pid':pid,'rssMiB':rss,'peakRssMiB':peak,'pssMiB':pss,'samples':samples,'forbiddenMaps':bad,'pass':peak<40 and pss<40 and not bad,'loadBefore':start['load'],'loadAfter':Path('/proc/loadavg').read_text().strip()})
elif ACTION=='remove-image':
 target=Path(exe());assert not h.residents();target.rename(R/('removed-image-'+ARGS[0]));record('image-removed-'+ARGS[0],{'entrySha256':sha(ENTRY) if ENTRY.exists() else None,'scriptSha256':sha(SCRIPT) if SCRIPT.exists() else None,'preference':pref(),'removedSha256':sha(R/('removed-image-'+ARGS[0]))})
elif ACTION=='restore-image':
 (R/('removed-image-'+ARGS[0])).rename(exe());record('image-restored-'+ARGS[0],{'imageSha256':sha(exe()),'preference':pref()})
elif ACTION=='stop-watch':
 assert not h.residents();assert SCRIPT.exists() and ENTRY.exists()
 entry=ENTRY.read_text();unit='putgd-login-startup-'+next(l.split('=',1)[1] for l in entry.splitlines() if l.startswith('X-putgd-StartupId='))
 r=cmd(['/bin/sh',str(SCRIPT),'--stop-watch'],env());assert not r['exit'],r
 status=cmd(['systemctl','--user','show',unit+'.path','-p','LoadState','-p','ActiveState'])
 record('stop-watch',{'command':r,'unit':unit,'after':status,'entryRetained':ENTRY.exists(),'scriptRetained':SCRIPT.exists()})
elif ACTION=='wait-removed':
 assert not h.residents();start=time.monotonic()
 wait(lambda:not ENTRY.exists() and not SCRIPT.exists(),12)
 assert pref() is True
 record('active-removal',{'secondsAfterRemovalReturned':round(time.monotonic()-start,3),'state':state(False)})
elif ACTION=='assert-inactive':
 sid,props=h.graphical_session();assert not sid,(sid,props)
 assert ENTRY.exists() and SCRIPT.exists();record('inactive-before-removal',{'sessionId':sid,'entryRetained':True,'scriptRetained':True,'preference':pref()})
elif ACTION=='identity':
 e=env();tools=['gnome-shell','cinnamon','plasmashell','kwin_wayland','Xorg']
 versions={t:cmd([t,'--version'],e,20) for t in tools if shutil.which(t)}
 record('identity',{'versions':versions,'session':state(False),'os':Path('/etc/os-release').read_text(),'kernel':cmd(['uname','-r']), 'displayEnvironment':{k:e.get(k) for k in h.VARIABLES if k in e}})
elif ACTION=='retention':
 label=ARGS[0];phase=ARGS[1];assert not h.residents()
 if label=='before':
  marker=HOME/'.local/share/putgd/i382-user-data.txt';marker.parent.mkdir(mode=0o700,parents=True,exist_ok=True);marker.write_text('synthetic uninstall preservation witness\n')
 roots=[CONFIG.parent,HOME/'.local/share/putgd',HOME/'.local/state/putgd',R/'archive']
 files={str(p):{'sha256':sha(p),'mode':p.stat().st_mode & 0o777} for root in roots for p in root.rglob('*') if p.is_file() and not p.is_symlink() and p!=SCRIPT}
 d={'files':files,'entrySha256':sha(ENTRY) if ENTRY.exists() else None,'scriptSha256':sha(SCRIPT) if SCRIPT.exists() else None,'preference':pref()}
 if label=='after':
  before=json.loads((R/('retention-before-'+phase+'.json')).read_text());d['unchanged']=all(d[k]==before[k] for k in ['files','preference']) and not ENTRY.exists() and not SCRIPT.exists();assert d['unchanged'],d
 record('retention-'+label+'-'+phase,d)
elif ACTION=='status':record('status-'+ARGS[0],state())
elif ACTION=='cleanup':
 assert not h.residents(),h.residents();assert not ENTRY.exists();assert not SCRIPT.exists()
 for p in [HOME/'.config/putgd',HOME/'.local/share/putgd',HOME/'.cache/putgd',HOME/'.local/state/putgd']:
  if p.exists():shutil.move(str(p),R/('final-'+p.parent.name+'-'+p.name))
 if (R/'preexisting-state').exists():shutil.move(str(R/'preexisting-state'),HOME/'.local/state/putgd')
 record('cleanup',{'residents':h.residents(),'entryAbsent':not ENTRY.exists(),'scriptAbsent':not SCRIPT.exists(),'profileAbsent':not CONFIG.exists(),'personalApps':personal()})
else:raise ValueError(ACTION)
