import sys,os,json,hashlib,subprocess,fcntl
from pathlib import Path
source=Path('/home/user/putgd-gnome-golden')
destination=Path('/home/user/work/put.gd/work/lab-images/gnome-legacy-source-563730cc')
expected='563730cc1d929bd946d20cf30a9aecfe3c4ad40eb12ba7f9abcff1c461e342bb'
sys.path.insert(0,'/home/user/lab-driver/tools/lab')
import overlay_cleanup
with overlay_cleanup.lifecycle_lock():
 assert source.is_dir() and not source.is_symlink() and not destination.exists()
 image=source/'image.qcow2'
 with image.open('rb') as f:before=hashlib.file_digest(f,'sha256').hexdigest()
 assert before==expected
 references=[]
 for candidate in Path('/home/user').rglob('*.qcow2'):
  d=json.loads(subprocess.check_output(['qemu-img','info','--output=json',str(candidate)]))
  backing=d.get('full-backing-filename') or d.get('backing-filename')
  if backing and (candidate.parent/backing).resolve()==image.resolve():references.append(str(candidate))
 assert not references, references
 # Privileged read-only inventory; fail closed on unreadable live descriptors/maps.
 probe=r"""import os,json
from pathlib import Path
image=Path('/home/user/putgd-gnome-golden/image.qcow2');s=image.stat();held=[]
for p in Path('/proc').iterdir():
 if not p.name.isdecimal():continue
 try:
  for fd in (p/'fd').iterdir():
   try:
    t=fd.stat()
    if (t.st_dev,t.st_ino)==(s.st_dev,s.st_ino):held.append(int(p.name))
   except FileNotFoundError:pass
  for line in (p/'maps').read_text().splitlines():
   f=line.split(maxsplit=5)
   if len(f)>=5 and int(f[4])==s.st_ino:
    major,minor=(int(x,16) for x in f[3].split(':'))
    if os.makedev(major,minor)==s.st_dev:held.append(int(p.name))
 except (FileNotFoundError,ProcessLookupError):pass
print(json.dumps(sorted(set(held))))
"""
 held=json.loads(subprocess.check_output(['sudo','-n','python3','-c',probe],text=True));assert not held,held
 os.rename(source,destination)
 with (destination/'image.qcow2').open('rb') as f:after=hashlib.file_digest(f,'sha256').hexdigest()
 assert before==after
 receipt=dict(source=str(source),destination=str(destination),sha256=after,backingReferences=references,heldPids=held,bytesUnchanged=True,originalNamedSnapshotsUnchanged=True)
 (destination/'canonical-relocation.json').write_text(json.dumps(receipt,indent=2)+'\n')
 print(json.dumps(receipt,indent=2))
