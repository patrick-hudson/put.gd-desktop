#!/usr/bin/env python3
from pathlib import Path
from collections import Counter
import re,json,datetime,hashlib
p=Path('/proc/2775');expected='15252c95892e81a4ec07afe816d090b3701374da868d0bc65f54ea8a9e92abd3'
assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==4592
assert hashlib.sha256((p/'exe').read_bytes()).hexdigest()==expected
rows=[];current=None;backings={}
for line in (p/'smaps').read_text().splitlines():
 m=re.match(r'^([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+\S+\s+(\S+)\s+(\d+)(?:\s+(.*))?$',line)
 if m:
  current=None;name=m[6] or ''
  if name.startswith('/memfd:'):
   name=name[len('/memfd:'):].removesuffix(' (deleted)')
   safe=name if name in ('wayland-shm','xorg') else 'other'
   backing=(m[4],m[5]);backings.setdefault(backing,'backing-'+str(len(backings)+1))
   current={'name':'memfd:'+safe,'backingGroup':backings[backing],'mappedBytes':int(m[2],16)-int(m[1],16),'permissions':m[3]};rows.append(current)
  continue
 if current is not None:
  v=re.match(r'^(Rss|Pss|Private_Clean|Private_Dirty|Shared_Clean|Shared_Dirty):\s+(\d+) kB$',line)
  if v:current[v[1]+'Bytes']=int(v[2])*1024
names=Counter()
for f in (p/'fd').iterdir():
 try:
  name=f.readlink().as_posix()
  if name.startswith('/memfd:'):
   name=name[len('/memfd:'):].removesuffix(' (deleted)');names['memfd:'+name if name in ('wayland-shm','xorg') else 'memfd:other']+=1
 except FileNotFoundError:pass
assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==4592
print(json.dumps({'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'pid':2775,'startTicks':4592,'executableSha256':expected,'scope':'Read-only shared mapping shape; only approved standard memfd labels retained, no mapped addresses/content/raw inode IDs','mappings':rows,'openMemfdCounts':dict(names),'backingGroupCount':len(backings)},indent=2))
