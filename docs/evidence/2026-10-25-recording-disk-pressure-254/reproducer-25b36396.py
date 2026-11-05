#!/usr/bin/env python3
"""Builder-only bounded tmpfs reproduction; Xvfb evidence is not desktop acceptance."""
import argparse, base64, json, os, pathlib, signal, subprocess, sys, time, traceback
p=argparse.ArgumentParser();p.add_argument('--build',required=True);p.add_argument('--run',required=True);p.add_argument('--baseline',action='store_true');p.add_argument('--exhaust',action='store_true');p.add_argument('--paused',action='store_true');p.add_argument('--scenario',choices=['pressure','minimum','recovery','normal','budget'],default='pressure');a=p.parse_args()
capacity_mib=1216 if a.scenario=='minimum' else 1536
build=pathlib.Path(a.build).resolve();run=pathlib.Path(a.run).resolve();run.mkdir(mode=0o700);(run/'reproducer.py').write_bytes(pathlib.Path(__file__).read_bytes())
repo=pathlib.Path(__file__).resolve().parents[2] if 'tools/lab' in str(__file__) else pathlib.Path('/home/patrick/work/i254')
subprocess.run([sys.executable,str(repo/'tools/diagnostics/check_socket_paths.py'),'--ui-output',str(run)],check=True,stdout=open(run/'socket-preflight.json','w'))
for n in ('config','data','cache','state','runtime','archive'): (run/n).mkdir(mode=0o700)
mount=run/'data/putgd/recording';mount.mkdir(parents=True,mode=0o700)
# Only the private capture staging is capacity-limited. Journal/logs stay writable.
subprocess.run(['sudo','-n','mount','-t','tmpfs','-o',f'size={capacity_mib}M,mode=0700,uid={os.getuid()},gid={os.getgid()},nosuid,nodev','i254-disposable',str(mount)],check=True)
env=dict(os.environ,QT_QPA_PLATFORM='xcb',XDG_SESSION_TYPE='x11',XDG_CURRENT_DESKTOP='X-Generic',NO_AT_BRIDGE='1')
for key,n in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_CACHE_HOME','cache'),('XDG_STATE_HOME','state'),('XDG_RUNTIME_DIR','runtime')]:env[key]=str(run/n)
for key in ('WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS','XAUTHORITY'):env.pop(key,None)
children=[];resident=None;events=[];report={'baseline':a.baseline,'exhaust':a.exhaust,'paused':a.paused,'run':str(run),'build':str(build),'identity':json.loads(subprocess.check_output([str(build/'putgd-cli'),'--version-json'])),'mountLimitBytes':capacity_mib<<20,'scenario':a.scenario,'binarySha256':{name:__import__('hashlib').sha256((build/name).read_bytes()).hexdigest() for name in ('putgd','putgd-cli','putgd-record')}}
def cli(method,params=None,allow=False):
 r=subprocess.run([str(build/'putgd-cli'),method,json.dumps(params or {})],env=env,capture_output=True,text=True,timeout=70)
 result=json.loads(r.stdout);events.append({'at':time.monotonic(),'method':method,'reply':result})
 if not result.get('ok') and not allow:raise RuntimeError(result)
 return result.get('result',{}) if result.get('ok') else result

def wait_state(rid,states,seconds=35):
 deadline=time.monotonic()+seconds
 while time.monotonic()<deadline:
  info=cli('recording.inspect',{'id':rid},allow=True)
  if info.get('state') in states or info.get('ok') is False:return info
  time.sleep(.15)
 raise RuntimeError('state timeout '+str(info))
try:
 # Dynamic X display allocation, never the user's session.
 with open(run/'display','w+') as f:
  xvfb=subprocess.Popen(['Xvfb','-displayfd',str(f.fileno()),'-screen','0','1024x768x24','-nolisten','tcp','-ac'],pass_fds=(f.fileno(),),stdout=open(run/'xvfb.log','w'),stderr=subprocess.STDOUT);children.append(xvfb)
  for _ in range(100):
   f.seek(0);display=f.read().strip()
   if display:break
   time.sleep(.05)
  else:raise RuntimeError('Xvfb did not start')
 env['DISPLAY']=':'+display
 # A synthetic moving source; no host windows can be recorded.
 pattern=subprocess.Popen(['gst-launch-1.0','-q','videotestsrc','is-live=true','pattern=snow' if a.scenario=='budget' else 'pattern=ball','!','video/x-raw,width=800,height=600,framerate=15/1','!','videoconvert','!','ximagesink'],env=env,stdout=open(run/'pattern.log','w'),stderr=subprocess.STDOUT);children.append(pattern)
 resident=subprocess.Popen([str(build/'putgd'),'--background'],env=env,stdout=open(run/'resident.log','w'),stderr=subprocess.STDOUT);children.append(resident)
 deadline=time.monotonic()+30
 while not (run/'runtime/putgd/session.sock').exists() and time.monotonic()<deadline:time.sleep(.1)
 settings=cli('settings.get')
 cli('settings.patch',{'patch':{'archive':{'keepUploads':True,'root':str(run/'archive')},'preferences':{'launchAtLogin':False}},'expectedRevision':settings['metadata']['observedRevision']})
 if a.scenario=='budget':
  settings=cli('settings.get');cli('settings.patch',{'patch':{'limits':{'maxFileBytes':2<<20}},'expectedRevision':settings['metadata']['observedRevision']})
 report['capabilities']=cli('recording.capabilities')
 rid=cli('recording.prepare',{'target':{'kind':'region','rect':[0,0,640,480]},'format':'mp4','fps':15})['recordingId'];report['id']=rid
 assert wait_state(rid,{'ready'})['state']=='ready'
 cli('recording.start',{'id':rid});time.sleep(3)
 if a.paused:cli('recording.pause',{'id':rid})
 report['before']=cli('recording.inspect',{'id':rid})
 subprocess.run(['ffmpeg','-v','error','-f','x11grab','-video_size','1024x768','-i',env['DISPLAY'],'-frames:v','1',str(run/'capture.png')],check=True,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 # A separate writer consumes only this bounded mount. In abrupt exhaustion
 # mode freeze just our helper to deterministically exercise the zero-space race.
 helpers=[]
 if a.exhaust:
  for entry in pathlib.Path('/proc').iterdir():
   if entry.name.isdigit():
    try:
     if (entry/'exe').resolve()==build/'putgd-record' and int((entry/'stat').read_text().split()[3])==resident.pid:helpers.append(int(entry.name))
    except (OSError,ValueError):pass
  assert len(helpers)==1,helpers
  os.kill(helpers[0],signal.SIGSTOP)
 writer_code='''import os,sys,time,json
path=sys.argv[1];floor=int(sys.argv[2]);written=0
with open(path,'wb',buffering=0) as f:
 assert os.fstat(f.fileno()).st_dev==int(sys.argv[3])
 v=os.fstatvfs(f.fileno());assert v.f_blocks*v.f_frsize==int(sys.argv[5])
 while True:
  s=os.statvfs(path);free=s.f_bavail*s.f_frsize
  if free<=floor:break
  try:written+=f.write(b'x'*min(8<<20,free-floor))
  except OSError as e:
   print(json.dumps({'written':written,'errno':e.errno}),flush=True);break
  if sys.argv[4]!='fast':time.sleep(.05)
print(json.dumps({'written':written,'freeBytes':os.statvfs(path).f_bavail*os.statvfs(path).f_frsize}),flush=True)
'''
 identity=json.loads(subprocess.check_output(['findmnt','--json','--target',str(mount),'--output','TARGET,SOURCE,FSTYPE']))['filesystems'][0]
 assert identity['target']==str(mount) and identity['source']=='i254-disposable' and identity['fstype']=='tmpfs',identity
 volume=os.statvfs(mount);assert volume.f_blocks*volume.f_frsize==capacity_mib<<20
 report['mountIdentity']=dict(identity,device=os.stat(mount).st_dev,capacityBytes=volume.f_blocks*volume.f_frsize)
 filler=mount/'competing-writer'
 floor=(1200<<20) if a.scenario=='recovery' else ((capacity_mib<<20) if a.scenario in ('minimum','normal','budget') else 0)
 writer=subprocess.Popen([sys.executable,'-c',writer_code,str(filler),str(floor),str(os.stat(mount).st_dev),'fast' if a.exhaust else 'paced',str(capacity_mib<<20)],stdout=open(run/'writer.jsonl','w'),stderr=subprocess.STDOUT);children.append(writer);assert writer.wait(30)==0
 for pid in helpers:os.kill(pid,signal.SIGCONT)
 if a.scenario in ('minimum','normal','recovery'):
  time.sleep(2);assert cli('recording.inspect',{'id':rid})['state'] in ('recording','paused')
  filler.unlink()
  if a.paused:cli('recording.resume',{'id':rid});time.sleep(1)
  cli('recording.stop',{'id':rid})
 report['after']=wait_state(rid,{'reviewing'},25)
 report['operationBeforeUpload']=cli('operation.inspect',{'id':rid})
 subprocess.run(['ffmpeg','-v','error','-f','x11grab','-video_size','1024x768','-i',env['DISPLAY'],'-frames:v','1',str(run/'result.png')],check=True,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 if a.baseline or a.exhaust:
  assert report['after'].get('ok') is False,report['after']
  report['expectedFailure']=True
 else:
  assert report['after']['state']=='reviewing'
  if a.scenario=='pressure':
   assert report['after']['stopDetails']['reason']=='disk_space'
   assert report['after']['progress'].get('bytes',0)<report['after']['config']['budgetBytes']
  elif a.scenario=='budget':assert report['after']['stopDetails']['reason']=='max_file_bytes'
  else:assert report['after']['stopReason']=='user'
  assert not list((run/'archive').rglob('*')),'automatic publication'
  mcp=repo/'tools/mcp/.venv/bin'
  report['mcpInspect']=json.loads(subprocess.check_output([str(mcp/'python'),'/home/patrick/work/i254-mcp-inspect.py',str(mcp/'putgd-mcp'),str(build/'putgd-cli'),str(run/'runtime/putgd/session.sock'),rid],env=env,text=True,stderr=open(run/'mcp.log','w')))
  assert report['mcpInspect']['result']['stopReason']==report['after']['stopReason']
  assert report['mcpInspect']['result'].get('stopDetails')==report['after'].get('stopDetails')
  preview=cli('recording.preview',{'id':rid,'time':0,'maxEdge':320});(run/'preview.png').write_bytes(base64.b64decode(preview['png']))
  # Restore only our filler; nothing touches host storage or user objects.
  if filler.exists():filler.unlink()
  cli('recording.saveCopy',{'id':rid,'path':str(run/'copy.mp4')})
  report['export']=wait_state(rid,{'reviewing'})
  assert report['export']['lastExport']['status']=='saved',report['export']
  subprocess.run(['ffmpeg','-v','error','-i',str(run/'copy.mp4'),'-f','null','-'],check=True,stdout=open(run/'decode.log','w'),stderr=subprocess.STDOUT)
  assert not list((run/'archive').rglob('*')),'local copy published'
  report['upload']=cli('recording.upload',{'id':rid,'revision':report['export']['edit']['revision']})
  deadline=time.monotonic()+30
  while time.monotonic()<deadline:
   op=cli('operation.inspect',{'id':rid});report['operationAfterUpload']=op
   if op.get('state') in ('completed','failed'):break
   time.sleep(.2)
  assert op.get('state')=='completed',op
 report['passed']=True
except Exception:
 report['passed']=False;report['failure']=traceback.format_exc()
finally:
 if resident and resident.poll() is None:
  try:cli('ui.quit')
  except Exception:pass
 for process in reversed(children):
  if process.poll() is None:
   process.terminate()
   try:process.wait(12)
   except subprocess.TimeoutExpired:process.kill();process.wait()
 if (mount/'competing-writer').exists():(mount/'competing-writer').unlink()
 report['cleanupUnmount']=subprocess.run(['sudo','-n','umount',str(mount)],capture_output=True,text=True).returncode
 (run/'events.json').write_text(json.dumps(events,indent=2));(run/'result.json').write_text(json.dumps(report,indent=2))
 print(json.dumps(report,indent=2))
 sys.exit(0 if report.get('passed') and report['cleanupUnmount']==0 else 1)
