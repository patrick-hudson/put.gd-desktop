#!/usr/bin/env python3
import json,subprocess,tempfile,time
from pathlib import Path
from common import ROOT,OUT,RUN,now,save,sha,source,load_lab
lab=load_lab();manifest=source();build=json.loads((OUT/'build-receipt.json').read_text());assert build['exitCode']==0 and build['commit']==manifest['commit'];package=Path(build['package']['path']);assert sha(package)==build['package']['sha256']
assert not (OUT/'install.json').exists(),'Keep all install attempts; do not overwrite this receipt'
baseline=json.loads((OUT/'baseline-stop.json').read_text());assert baseline['ok'] and baseline['sourceCommit']==manifest['commit']
identity=baseline['previousResident'];operations=baseline['previousOperationStates'];backup=baseline['preservedGuestProfile']
receipt={'schemaVersion':1,'startedUtc':now(),'sourceCommit':manifest['commit'],'packageSha256':sha(package),'previousResident':identity,'previousOperationStates':operations,'preservedGuestProfile':backup,'baselineStopReceipt':'baseline-stop.json','ok':False}
save(OUT/'install.json',receipt)
try:
 guard_script="""import pathlib,os,json
backup=pathlib.Path(%r)
assert backup.is_dir() and not backup.is_symlink() and backup.stat().st_uid==os.getuid()
assert not pathlib.Path('/home/lab/putgd-lab').exists()
for p in pathlib.Path('/proc').iterdir():
 try:
  if p.name.isdecimal() and p.stat().st_uid==os.getuid():assert (p/'comm').read_text().strip() not in ['putgd','putgd-ui'],'An app process was relaunched before upgrade'
 except (FileNotFoundError,ProcessLookupError):pass
print(json.dumps({'noOldAppProcesses':True,'preservedProfile':str(backup)}))
"""%backup
 command=lab.ssh_args(RUN)+['python3','-'];result=subprocess.run(command,input=guard_script,text=True,capture_output=True);receipt['preInstallGuard']={'command':command,'script':guard_script,'exitCode':result.returncode,'result':json.loads(result.stdout) if result.returncode==0 else None};result.check_returncode()
 command=['python3',str(ROOT/'tools/lab/lab.py'),'install','--run',str(RUN),'--deb',str(package)];receipt['command']=command
 with (OUT/'install.log').open('w') as log:result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
 receipt['exitCode']=result.returncode;result.check_returncode()
 lab.guest_session(RUN,'mkdir -p "$HOME/putgd-lab"\nnohup putgd-launcher --background > "$HOME/putgd-lab/app.log" 2>&1 < /dev/null &\n')
 deadline=time.monotonic()+90
 while True:
  try:
   current=lab.verify_running_app(RUN)
   if lab.cli(RUN,'diagnostics').get('journalReady'):break
  except (lab.LabError,ValueError):pass
  if time.monotonic()>deadline:raise RuntimeError('Current installed resident startup timed out')
  time.sleep(.2)
 assert current!=identity
 assert not lab.cli(RUN,'operation.recent')['operations']
 with tempfile.TemporaryDirectory(prefix='putgd-u11-kde-') as tmp:
  subprocess.run(['dpkg-deb','--extract',str(package),tmp],check=True)
  base=Path(tmp);expected={}
  for prefix in ['usr/bin','usr/lib','usr/share/putgd','usr/share/applications']:
   for f in (base/prefix).rglob('*'):
    if f.is_file():expected['/'+str(f.relative_to(base))]=sha(f)
  assert all('/usr/bin/'+name in expected for name in ['putgd','putgd-cli','putgd-ui'])
  packaged=json.loads((base/'usr/share/putgd/build-info.json').read_text())
 script='''import hashlib,json,pathlib,subprocess
paths=json.loads(%r)
result={'installedFiles':{p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in paths},'manifest':json.loads(pathlib.Path('/usr/share/putgd/build-info.json').read_text()),'versions':{}}
for name in ['putgd','putgd-cli','putgd-ui']:result['versions'][name]=subprocess.check_output(['/usr/bin/'+name,'--version'],text=True).strip()
print(json.dumps(result))
'''%json.dumps(list(expected))
 result=subprocess.run(lab.ssh_args(RUN)+['python3','-'],input=script,text=True,capture_output=True,check=True);actual=json.loads(result.stdout)
 assert actual['installedFiles']==expected
 assert actual['manifest']==packaged==lab.cli(RUN,'diagnostics')['build']
 assert packaged['gitCommit']==manifest['commit'] and packaged['gitTree']==manifest['tree'] and packaged['commitCount']==manifest['commitCount']
 assert all(actual['versions'][name]==name+' '+packaged['version'] for name in ['putgd','putgd-cli','putgd-ui'])
 save(OUT/'package-files.json',actual)
 receipt.update(runningApp=current,version=packaged,operationCount=0,package=json.loads((RUN/'package.json').read_text()),ok=True)
except Exception as error:receipt['error']=str(error);raise
finally:receipt['finishedUtc']=now();save(OUT/'install.json',receipt)
print(json.dumps({'ok':True,'runningApp':current,'version':packaged['version']}))
