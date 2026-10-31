"""Fresh-fork lab development proof, using production launch/install/start/cleanup."""
import argparse,hashlib,importlib.util,json,os,subprocess,sys,time,tarfile
from pathlib import Path
P=argparse.ArgumentParser();P.add_argument('--boat-id',required=True);P.add_argument('--session',choices=['wayland'],default='wayland');a=P.parse_args()
controller=Path('/home/user/kde-driver');sys.path.insert(0,str(controller/'tools/lab'));import lab
spec=importlib.util.spec_from_file_location('snapshot_recipe','/home/user/kde_recipe.py');s=importlib.util.module_from_spec(spec);spec.loader.exec_module(s)
m=json.load(open('/home/user/kde-final-preparation-manifest.json'));golden=Path(m['golden']['path'])
evidence=Path('/home/user/kde-evidence/fresh-'+a.session+'-final');evidence.mkdir(mode=0o700,parents=True)
run=controller/'work/lab-runs'/('kw2' if a.session=='wayland' else 'kx1')
entry=controller/'tools/lab/lab.py'
def record(name,data):s.exclusive_json(evidence/name,data)
def cli(*args,timeout=300):return s.run(['python3',entry,*args],timeout=timeout)
record('identity.json',dict(placement='boat',sandbox=a.boat_id,controllerCommit=s.run(['git','rev-parse','HEAD'],cwd=controller).strip(),driverProfileSha256=s.digest(controller/'tools/lab/profiles/kde-wayland.yaml'),recipeSha256=s.digest('/home/user/kde_recipe.py'),sourceGolden=m['sourceGolden'],golden=m['golden'],package=m['packageArtifact']))
assert s.digest(golden)==m['golden']['sha256']
(evidence/'socket-preflight.json').write_text(s.run(['python3',controller/'tools/diagnostics/check_socket_paths.py','--socket',run/'qmp.sock',run/'vnc.sock','/home/lab/putgd-lab/runtime/putgd/session.sock','/tmp/sd-12345678/runtime/putgd/session.sock']))
ok=False;launched=False
try:
 (evidence/'launch.json').write_text(cli('launch','--profile','kde-wayland','--golden',golden,'--run',run,'--bootstrap-key','/home/user/snapshot-bootstrap/identity.pub','--known-hosts','/home/user/snapshot-bootstrap/known_hosts','--acceleration','kvm','--timeout','240'));launched=True
 record('policy.json',json.loads(lab.ssh(run,s.kde_policy_script(False))))
 record('automatic-updates.json',json.loads(lab.ssh(run,s.kde_update_policy_script(False))))
 (evidence/'probes.log').write_text(lab.ssh(run,s.probe_script(False)))
 record('preflight-before.json',lab.guest_preflight(run,apply=False,stage='core-demo-before'))
 (evidence/'desktop-packages.log').write_text(lab.ssh(run,"dpkg-query -W -f='${binary:Package}=${Version}\\n' plasma-workspace kwin-wayland kwin-common sddm apt openssh-server plasma-discover\n"))
 deb=Path(m['packageArtifact']['path']);record('install.json',lab.install(run,deb=deb))
 record('app-socket.json',json.loads(lab.guest_session(run,"python3 - <<'SOCKET'\nimport json,os\np=os.environ['XDG_RUNTIME_DIR']+'/putgd/session.sock';n=len(p.encode());assert n<=107;print(json.dumps(dict(path=p,utf8Bytes=n)))\nSOCKET\n")))
 started=lab.start(run,timeout=60);record('start.json',started)
 observed=json.loads(lab.guest_session(run,'putgd-cli --version-json\n'));assert observed.get('gitCommit')==m['builtTreeCommit'] and observed.get('version')==m['packageArtifact']['buildInfo']['version'] and observed.get('dirty') is False;record('installed-version.json',observed)
 record('show.json',lab.cli(run,'ui.show',{'view':'settings'}));time.sleep(2)
 with lab.QMP(run/'qmp.sock') as q:q.execute('screendump',{'filename':str(evidence/'installed-settings.ppm')})
 lab.cli(run,'ui.quit');deadline=time.monotonic()+15
 while lab.ssh(run,"test ! -d /proc/"+str(started['resident']['pid'])+" && echo stopped || echo running\n").strip()!='stopped':
  assert time.monotonic()<deadline;time.sleep(.25)
 catalog=Path('/home/user/kde-catalog')
 assert (catalog/'manifest.json').is_file(), 'generate the source host catalog first'
 record('catalog-source.json',json.loads(Path('/home/user/kde-evidence/catalog-source.json').read_text()))
 # Preserve the test-helper diagnostic directly on the host, before any reset.
 binary=Path('/home/user/work/put.gd/src/build/dev/putgd-ui-catalog')
 lab.put(run,binary,'/home/lab/catalog-diag')
 diagnostic=lab.command(lab.ssh_args(run)+['sh','-s'],input="chmod 700 /home/lab/catalog-diag\nldd /home/lab/catalog-diag\nQT_QPA_PLATFORM=offscreen timeout 5s /home/lab/catalog-diag /tmp/cdiag\n",check=False,timeout=20)
 record('foreign-helper-diagnostic.json',dict(exitCode=diagnostic.returncode,stdout=diagnostic.stdout,stderr=diagnostic.stderr,binarySha256=s.digest(binary)))
 command=['python3',controller/'tools/lab/surface_driver/run.py','--desktop','kubuntu-kde-'+a.session,'--host','boat','--placement','boat','--boat-id',a.boat_id,'--run',run,'--package','deb','--destination','local','--recipes','core_demo','--catalog',catalog,'--build',m['builtTreeCommit'],'--out',evidence/'recipes','--coverage',evidence/'coverage.json','--campaign','kde-'+a.session]
 record('recipe-command.json',[str(x) for x in command])
 with (evidence/'recipes.log').open('w') as log:r=subprocess.run([str(x) for x in command],cwd=controller,stdout=log,stderr=subprocess.STDOUT,timeout=600)
 record('recipe-exit.json',dict(exitCode=r.returncode));record('preflight-after.json',lab.guest_preflight(run,apply=False,stage='core-demo-after'));record('automatic-updates-after.json',json.loads(lab.ssh(run,s.kde_update_policy_script(False))))
 after=lab.ssh(run,"dpkg-query -W -f='${binary:Package}=${Version}\\n' plasma-workspace kwin-wayland kwin-common sddm apt openssh-server plasma-discover\n");(evidence/'system-packages-after.log').write_text(after);assert after==(evidence/'desktop-packages.log').read_text(), 'system packages changed during the recipe run'
 ok=r.returncode==0
except BaseException as error:
 record('failure.json',dict(type=type(error).__name__,detail=str(error)[:2000]))
 raise
finally:
 if launched or (run/'state.json').exists():
  try:
   with lab.QMP(run/'qmp.sock') as q:q.execute('screendump',{'filename':str(evidence/'desktop-after.ppm')})
   lab.ssh(run,'sync\n')
  finally:(evidence/'reset.json').write_text(cli('reset','--run',run))
# Same held-inode refusal and explicit completion evidence as snapshots.py guest.
state=lab.state(run);completion=dict(run=str(run),runId=state['id'],completed=True,outcome='passed' if ok else 'failed')
record('completion.json',completion)
def completion_manifest(name):
 record(name,dict(schemaVersion=1,kind='vm-overlay-cleanup-evidence',**completion,files={p.name:s.digest(p) for p in evidence.iterdir() if p.is_file()}));return evidence/name
manifest=completion_manifest('cleanup-evidence.json')
args=['python3',entry,'cleanup','--run',run,'--images-root',golden.parent.parent,'--evidence-manifest',manifest]
holder=subprocess.Popen([sys.executable,'-c','import json,os,sys,time;f=open(sys.argv[1],"rb");i=os.fstat(f.fileno());print(json.dumps(dict(device=i.st_dev,inode=i.st_ino)),flush=True);time.sleep(60)',str(run/'disk.qcow2')],stdout=subprocess.PIPE,text=True)
try:
 identity=json.loads(holder.stdout.readline());actual=(run/'disk.qcow2').stat();assert identity==dict(device=actual.st_dev,inode=actual.st_ino)
 refused=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=90)
 record('held-overlay-refusal.json',dict(holderPid=holder.pid,identity=identity,exitCode=refused.returncode,stdout=refused.stdout,stderr=refused.stderr,overlayStillExists=(run/'disk.qcow2').exists()))
 assert refused.returncode!=0 and (run/'disk.qcow2').exists()
finally:
 if holder.poll() is None:holder.terminate()
 holder.wait(timeout=15);holder.stdout.close()
args[-1]=completion_manifest('cleanup-evidence-final.json')
(evidence/'cleanup-preview.json').write_text(s.run([*args,'--dry-run']));(evidence/'cleanup.json').write_text(s.run(args))
assert s.digest(golden)==m['golden']['sha256']
record('result.json',dict(ok=ok,placement='boat',sandbox=a.boat_id,session=a.session,goldenUnchanged=True))
raise SystemExit(0 if ok else 1)
