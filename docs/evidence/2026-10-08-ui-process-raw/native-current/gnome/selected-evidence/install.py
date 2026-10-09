#!/usr/bin/env python3
"""Replace only the existing owned GNOME lab resident, preserving its synthetic state."""
from pathlib import Path
import datetime,hashlib,json,shlex,subprocess,sys,time
from PIL import Image
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';source=json.loads((out.parent/'source.json').read_text());guest='/home/lab/putgd-u11-'+source['commit'][:12]
assert not (out/'install.json').exists(),'Preserve prior install evidence'
build=json.loads((out/'build-receipt.json').read_text());assert build['exitCode']==0 and build['commit']==source['commit'];package=Path(build['package']['path']);assert lab.hash_file(package)==build['package']['sha256']
expected=json.loads((out/'baseline-resident.json').read_text())['identity']
if not (out/'preserve-state.json').exists():
 assert lab.verify_running_app(run)==expected
 ops=lab.cli(run,'operation.recent')['operations'];assert all(x['state'] in ['completed','failed','cancelled','partial','interrupted'] for x in ops)
bootBefore=lab.ssh(run,'cat /proc/sys/kernel/random/boot_id\n').strip()
stop=r'''from pathlib import Path
import hashlib,json,os,signal,time
expected=EXPECTED
proc=Path('/proc')/str(expected['pid']);p=Path('/home/lab/putgd-lab');target=Path(TARGET)
assert p.is_dir() and not p.is_symlink() and not target.exists()
fd=os.pidfd_open(expected['pid'])
try:
 actual={'pid':expected['pid'],'startTicks':int((proc/'stat').read_text().rsplit(')',1)[1].split()[19]),'sha256':hashlib.sha256((proc/'exe').read_bytes()).hexdigest()}
 assert actual==expected and proc.stat().st_uid==os.getuid() and (proc/'comm').read_text().strip()=='putgd'
 signal.pidfd_send_signal(fd,signal.SIGTERM)
finally:os.close(fd)
end=time.monotonic()+10
while proc.exists():
 if time.monotonic()>end:raise RuntimeError('Owned resident failed to stop normally')
 time.sleep(.1)
p.rename(target)
print(json.dumps({'stopped':expected,'statePreservedAt':str(target),'method':'identity-guarded pidfd SIGTERM'}))
'''.replace('EXPECTED',repr(expected)).replace('TARGET',repr(guest+'/previous-lab-data'))
if not (out/'preserve-state.json').exists():
 p=subprocess.run(lab.ssh_args(run)+['python3','-'],input=stop,text=True,capture_output=True,timeout=30)
 (out/'preserve-state.json').write_text(json.dumps({'exitCode':p.returncode,'stdout':p.stdout,'stderr':p.stderr},indent=2)+'\n');p.check_returncode()
else:
 prior=json.loads((out/'preserve-state.json').read_text());assert prior['exitCode']==0 and prior.get('userRequestedImmediateOldResidentStop')
 saved=json.loads(prior['stdout']);assert saved['stopped']==expected and saved['statePreservedAt']==guest+'/previous-lab-data'
 terminals=json.loads((out/'stop-old-terminal-operations.json').read_text())['operations'];assert len(terminals)==15 and all(x['state'] in ['completed','failed','cancelled','partial','interrupted'] for x in terminals)
 check=lab.ssh(run,'set -eu\ntest ! -e /proc/'+str(expected['pid'])+'\ntest -d '+shlex.quote(guest+'/previous-lab-data')+'\ntest ! -e /home/lab/putgd-lab\nif pgrep -u lab -x putgd >/dev/null; then exit 1; fi\necho verified-stopped\n')
 assert check.strip()=='verified-stopped'
argv=[sys.executable,str(root/'tools/lab/lab.py'),'install','--run',str(run),'--deb',str(package)]
p=subprocess.run(argv,text=True,capture_output=True,timeout=200)
(out/'install.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'argv':argv,'exitCode':p.returncode,'stdout':p.stdout,'stderr':p.stderr},indent=2)+'\n');p.check_returncode()
(out/'package.json').write_bytes((run/'package.json').read_bytes())
inspect='''from pathlib import Path
import hashlib,json,subprocess
files=['/usr/bin/putgd','/usr/bin/putgd-cli','/usr/bin/putgd-ui']
print(json.dumps({'manifest':json.loads(Path('/usr/share/putgd/build-info.json').read_text()),'versions':{p:subprocess.check_output([p,'--version'],text=True).strip() for p in files},'binarySha256':{p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in files},'packageVersion':subprocess.check_output(['dpkg-query','-W','-f=${Version}','putgd'],text=True),'extensionSha256':hashlib.sha256(Path('/usr/share/gnome-shell/extensions/putgd@put.gd/extension.js').read_bytes()).hexdigest()},indent=2))
'''
p=subprocess.run(lab.ssh_args(run)+['python3','-'],input=inspect,capture_output=True,text=True,timeout=30);p.check_returncode();installed=json.loads(p.stdout);assert installed['manifest']['gitCommit']==source['commit'];version='0.1.1-dev.41+g97ca3a521714';assert all(version in x for x in installed['versions'].values());assert installed['extensionSha256']==source['files']['integrations/gnome/extension.js']['sha256'];(out/'installed.json').write_text(json.dumps(installed,indent=2)+'\n')
# Reboot this existing disposable overlay; its automatic login starts fresh Shell JS.
result=subprocess.run(lab.ssh_args(run)+['sudo -n systemctl reboot'],capture_output=True,text=True,timeout=20)
(out/'reboot-request.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'exitCode':result.returncode,'bootIdBefore':bootBefore,'purpose':'Load installed GNOME extension in a fresh Shell process; existing VM only'},indent=2)+'\n')
end=time.monotonic()+150
while True:
 try:
  bootAfter=lab.ssh(run,'cat /proc/sys/kernel/random/boot_id\n',timeout=8).strip()
  if bootAfter!=bootBefore:
   info=lab.guest_session(run,'gnome-extensions info putgd@put.gd\n')
   if 'State: ACTIVE' in info:break
 except (lab.LabError,subprocess.TimeoutExpired):pass
 if time.monotonic()>end:raise RuntimeError('Fresh GNOME session/extension not ready after bounded reboot')
 time.sleep(2)
frame=lab.screenshot(run,'u11-gnome-fresh-shell');Image.open(frame['path']).save(out/'fresh-shell.png')
(out/'session-reload.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'bootIdBefore':bootBefore,'bootIdAfter':bootAfter,'extensionInfo':info,'visibleWitness':'fresh-shell.png','VncPort':5936,'newVmLaunched':False},indent=2)+'\n')
print(json.dumps({'status':'installed; fresh GNOME Shell extension active','version':version,'VncPort':5936}))
