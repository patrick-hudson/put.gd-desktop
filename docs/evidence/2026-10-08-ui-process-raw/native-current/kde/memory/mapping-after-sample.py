#!/usr/bin/env python3
"""Read resident mapping attribution only after a completed, passing timed observation."""
import argparse,datetime,hashlib,importlib.util,json,os
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--results',required=True);p.add_argument('--bench-idle',required=True);p.add_argument('--bench-idle-sha256',required=True);p.add_argument('--output',required=True);a=p.parse_args()
r=json.loads(Path(a.results).read_text());assert r['passed'] and r['status']=='complete';expected=r['residentIdentity'];pid=expected['pid'];assert Path(a.bench_idle).is_file();assert hashlib.sha256(Path(a.bench_idle).read_bytes()).hexdigest()==a.bench_idle_sha256
assert hashlib.sha256(Path(r['binaries']['helper']['path']).read_bytes()).hexdigest()==r['binaries']['helper']['sha256']
def identity():
 root=Path('/proc')/str(pid);f=(root/'stat').read_text().rsplit(') ',1)[1].split();exe=root/'exe';s=exe.stat()
 result={'pid':pid,'startTicks':int(f[19]),'uid':root.stat().st_uid,'executable':os.readlink(exe),'device':s.st_dev,'inode':s.st_ino}
 assert result==expected and result['uid']==os.getuid()
 assert hashlib.sha256(exe.read_bytes()).hexdigest()==r['binaries']['app']['sha256']
 return result
before=identity();spec=importlib.util.spec_from_file_location('putgd_idle_footprint',a.bench_idle);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);mappings=module.footprint(pid);after=identity();assert before==after
out={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Separate post-observation resident mapping attribution; not a timed sample','residentIdentity':before,'sourceRevision':r['sourceRevision'],'appSha256':r['binaries']['app']['sha256'],'helperSha256':r['binaries']['helper']['sha256'],'benchIdleSha256':a.bench_idle_sha256,'scriptSha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'timedObservationSha256':hashlib.sha256(Path(a.results).read_bytes()).hexdigest(),'mappings':mappings}
with open(a.output,'x') as f:json.dump(out,f,indent=2);f.write('\n')
