#!/usr/bin/env python3
"""Read only: current guest binaries/helper and exact resident identity; no private input."""
from pathlib import Path
import datetime,hashlib,json,subprocess

def command(argv):
 p=subprocess.run(argv,capture_output=True,text=True,timeout=10)
 return {'argv':argv,'exitCode':p.returncode,'stdout':p.stdout,'stderr':p.stderr}

def sha(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for part in iter(lambda:f.read(1024*1024),b''): h.update(part)
 return h.hexdigest()

files=['/usr/bin/putgd','/usr/bin/putgd-cli','/usr/bin/putgd-launcher','/usr/bin/putgd-desktop-fixture','/usr/share/gnome-shell/extensions/putgd@put.gd/extension.js','/usr/share/gnome-shell/extensions/putgd@put.gd/metadata.json','/usr/share/putgd/aws-runtime-manifest.json']
modules=list(Path('/usr/lib').glob('**/putgd/libputgd-s3.so'))
files.extend(str(p) for p in modules)
processes=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit(): continue
 try:
  if (p/'comm').read_text().strip()!='putgd': continue
  fields=(p/'stat').read_text().split(') ',1)[1].split()
  maps=(p/'maps').read_text()
  processes.append({'pid':int(p.name),'startTicks':fields[19],'executableSha256':sha(p/'exe'),'installedExecutableMatch':sha(p/'exe')==sha('/usr/bin/putgd'),'QtNetworkMapped':'libQt6Network.so' in maps,'awsMapped':'libaws-cpp-sdk-' in maps,'s3ModuleMapped':'libputgd-s3.so' in maps})
 except FileNotFoundError: continue
result={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Read-only exact installed and running artifact identity; no credentials/configuration/payload/environment export','files':{p:sha(p) for p in files if Path(p).is_file()},'bootId':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'residentProcesses':processes,'commands':[command(['readelf','-d','/usr/bin/putgd']),command(['gnome-extensions','info','putgd@put.gd']),command(['dpkg-query','-W','-f=${Package} ${Version}\n','putgd','gnome-shell','mutter-common','gjs','libqt6core6t64','qt6-wayland']),command(['loginctl','list-sessions','--no-legend'])]}
print(json.dumps(result,indent=2))
