#!/usr/bin/env python3
import json,subprocess
from common import ROOT,OUT,RUN,now,save,source,load_lab
lab=load_lab();manifest=source();output=OUT/'baseline-stop.json';assert not output.exists()
identity=lab.verify_running_app(RUN);operations=lab.cli(RUN,'operation.recent')['operations'];assert all(op['state'] in ['completed','cancelled','failed','partial','interrupted'] for op in operations)
backup='/home/lab/putgd-lab-before-u11-'+manifest['commit'][:12]
receipt={'schemaVersion':1,'startedUtc':now(),'sourceCommit':manifest['commit'],'previousResident':identity,'previousOperationStates':[{'id':op['id'],'state':op['state']} for op in operations],'preservedGuestProfile':backup,'ok':False}
for name in ['package.json','parameters.json','versions.json','diagnostics.json']:
 if (RUN/name).exists():(OUT/('baseline-'+name)).write_bytes((RUN/name).read_bytes())
save(output,receipt)
script='''import pathlib,os,signal,time,hashlib,json
identity=json.loads(%r)
p=pathlib.Path('/proc')/str(identity['pid'])
assert p.stat().st_uid==os.getuid()
assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==identity['startTicks']
assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==identity['sha256']
os.kill(identity['pid'],signal.SIGTERM)
for unused in range(100):
 if not p.exists():break
 time.sleep(.1)
else:raise RuntimeError('Verified baseline resident did not stop')
old=pathlib.Path('/home/lab/putgd-lab');backup=pathlib.Path(%r)
assert old.is_dir() and not old.is_symlink() and old.stat().st_uid==os.getuid()
assert not backup.exists()
old.rename(backup)
print(json.dumps({'stopped':identity,'profilePreserved':True}))
'''%(json.dumps(identity),backup)
command=lab.ssh_args(RUN)+['python3','-']
try:
 result=subprocess.run(command,input=script,text=True,capture_output=True);receipt.update(command=command,script=script,exitCode=result.returncode,result=json.loads(result.stdout) if result.returncode==0 else None,error=result.stderr[:1000] if result.returncode else None);result.check_returncode();receipt['ok']=True
finally:receipt['finishedUtc']=now();save(output,receipt)
print(json.dumps({'stopped':identity,'profilePreserved':backup,'ok':receipt['ok']}))
