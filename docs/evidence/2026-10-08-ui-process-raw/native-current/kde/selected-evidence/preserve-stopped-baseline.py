#!/usr/bin/env python3
import json,subprocess
from common import ROOT,OUT,RUN,now,save,source,load_lab
lab=load_lab();manifest=source();output=OUT/'baseline-stop.json';assert not output.exists()
previous=json.loads((OUT/'preflight.json').read_text())['baselineIdentity'];backup='/home/lab/putgd-lab-before-u11-'+manifest['commit'][:12]
script='''import pathlib,os,json,sqlite3
previous=json.loads(%r)
for p in pathlib.Path('/proc').iterdir():
 try:
  if p.name.isdecimal() and p.stat().st_uid==os.getuid():assert (p/'comm').read_text().strip() not in ['putgd','putgd-ui'],'An app process remains'
 except (FileNotFoundError,ProcessLookupError):pass
p=pathlib.Path('/proc')/str(previous['pid'])
assert not p.exists(),'Old PID still exists; inspect before proceeding'
old=pathlib.Path('/home/lab/putgd-lab');backup=pathlib.Path(%r)
assert old.is_dir() and not old.is_symlink() and old.stat().st_uid==os.getuid()
assert not backup.exists()
db=sqlite3.connect('file:'+str(old/'data/putgd/putgd/history.sqlite')+'?mode=ro',uri=True)
rows=[{'id':row[0],'state':row[1]} for row in db.execute("SELECT id,json_extract(data,'$.state') FROM operations")];db.close()
assert all(row['state'] in ['completed','cancelled','failed','partial','interrupted'] for row in rows)
old.rename(backup)
print(json.dumps({'alreadyStopped':True,'previousResident':previous,'profilePreserved':True,'operations':rows}))
'''%(json.dumps(previous),backup)
receipt={'schemaVersion':1,'startedUtc':now(),'sourceCommit':manifest['commit'],'previousResident':previous,'preservedGuestProfile':backup,'ok':False,'stopDisposition':'Already absent before guarded stop; no signal sent; cause not established'}
for name in ['package.json','parameters.json','versions.json','diagnostics.json']:
 if (RUN/name).exists():(OUT/('baseline-'+name)).write_bytes((RUN/name).read_bytes())
try:
 command=lab.ssh_args(RUN)+['python3','-'];result=subprocess.run(command,input=script,text=True,capture_output=True);receipt.update(command=command,script=script,exitCode=result.returncode,result=json.loads(result.stdout) if result.returncode==0 else None,error=result.stderr[:1000] if result.returncode else None);result.check_returncode();receipt['previousOperationStates']=receipt['result']['operations'];receipt['ok']=True
finally:receipt['finishedUtc']=now();save(output,receipt)
print(json.dumps({'ok':True,'previousResident':previous,'alreadyStopped':True,'operationCount':len(receipt['previousOperationStates']),'profile':backup}))
