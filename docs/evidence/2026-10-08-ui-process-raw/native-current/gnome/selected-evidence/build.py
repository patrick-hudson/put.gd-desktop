#!/usr/bin/env python3
"""Build an exact committed source archive inside the existing GNOME overlay."""
from pathlib import Path
import json,hashlib,subprocess,sys,time,shlex,datetime
root=Path('/home/patrick/work/put.gd');out=Path(__file__).resolve().parent;common=out.parent
assert not (out/'build.log').exists(), 'Preserve prior build attempts'
started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat()
sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';source=json.loads((common/'source.json').read_text());archive=Path(source['archive']);assert lab.hash_file(archive)==source['archiveSha256']
guest='/home/lab/putgd-u11-'+source['commit'][:12]
lab.ssh(run,'mkdir '+shlex.quote(guest)+'\n')
lab.put(run,archive,guest+'/source.tar');lab.put(run,common/'source.json',guest+'/source.json')
verify='''from pathlib import Path
import hashlib,json,subprocess
p=Path.cwd();m=json.loads((p/'source.json').read_text());assert hashlib.sha256((p/'source.tar').read_bytes()).hexdigest()==m['archiveSha256'];(p/'source').mkdir();subprocess.run(['tar','-xf','source.tar','-C','source'],check=True)
for name,entry in m['files'].items():
 f=p/'source'/name
 if entry['type']=='file':assert f.is_file() and not f.is_symlink() and hashlib.sha256(f.read_bytes()).hexdigest()==entry['sha256'] and f.stat().st_mode&0o777==entry['mode'],name
 else:assert f.is_symlink() and str(f.readlink())==entry['target'],name
print('Verified committed archive and',len(m['files']),'files')
'''
script=(common/'guest-build-template.sh').read_text().replace('@SOURCE@',guest+'/source').replace('@OUTPUT@',guest).replace('@COMMIT@',source['commit']).replace('@TREE@',source['tree']).replace('@COUNT@',str(source['commitCount']))
local=out/'build.sh';local.write_text(script);lab.put(run,local,guest+'/build.sh')
with (out/'verify.log').open('w') as log:
 r=subprocess.run(lab.ssh_args(run)+['cd '+shlex.quote(guest)+' && python3 -'],input=verify,text=True,stdout=log,stderr=subprocess.STDOUT);r.check_returncode()
started=time.monotonic()
with (out/'build.log').open('w') as log:r=subprocess.run(lab.ssh_args(run)+['sh '+shlex.quote(guest+'/build.sh')],stdout=log,stderr=subprocess.STDOUT)
receipt={'schemaVersion':1,'startedUtc':started_utc,'finishedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'command':lab.ssh_args(run)+['sh '+shlex.quote(guest+'/build.sh')],'commit':source['commit'],'guestDirectory':guest,'archiveSha256':source['archiveSha256'],'exitCode':r.returncode,'elapsedSeconds':time.monotonic()-started,'phase':'build only; native acceptance pending'}
(out/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt),flush=True)
if lab.ssh(run,'test ! -f '+shlex.quote(guest+'/dev.xml')+' || echo present\n').strip()=='present':lab.get(run,guest+'/dev.xml',out/'dev.xml')
r.check_returncode()
remote=lab.ssh(run,'find '+shlex.quote(guest+'/packages')+" -maxdepth 1 -name '*.deb' -print\n").splitlines();assert len(remote)==1
package=out/Path(remote[0]).name;lab.get(run,remote[0],package);receipt['package']={'path':str(package),'sha256':lab.hash_file(package)}
lab.get(run,guest+'/source/build/release/build-identity/build-info.json',out/'build-info.json');assert json.loads((out/'build-info.json').read_text())['gitCommit']==source['commit']
(out/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
