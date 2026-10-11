import pathlib,hashlib,json,subprocess,os,time,datetime
files=['/usr/bin/putgd','/usr/bin/putgd-cli','/usr/bin/putgd-desktop-fixture','/usr/bin/putgd-launcher','/usr/lib/x86_64-linux-gnu/putgd/libputgd-s3.so','/usr/lib/putgd/libputgd-s3.so','/usr/share/applications/putgd.desktop','/usr/share/putgd/aws-runtime-manifest.json','/usr/share/putgd/dependency-lock.json']
def sha(p):
 h=hashlib.sha256()
 with open(p,'rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()
def elf(p):
 try: x=subprocess.run(['readelf','-d',p],capture_output=True,text=True)
 except FileNotFoundError: return {'unavailable':'Runtime guest has no readelf; exact package ELF inspected on host.'}
 return [s.split('[',1)[1].split(']',1)[0] for s in x.stdout.splitlines() if '(NEEDED)' in s]
procs=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if (p/'comm').read_text().strip()!='putgd':continue
  st=(p/'stat').read_text().rsplit(')',1)[1].split()
  procs.append({'pid':int(p.name),'startTicks':int(st[19]),'executableSha256':sha(p/'exe'),'executableMatchesInstalled':sha(p/'exe')==sha('/usr/bin/putgd'),'needed':elf(str(p/'exe'))})
 except (OSError,ProcessLookupError):continue
v=subprocess.run(['dpkg-query','-W','-f=${Package} ${Version}\n','putgd','libqt6core6t64','libqt6gui6','libqt6widgets6','plasma-workspace','kwin-wayland','liblayershellqtinterface6'],capture_output=True,text=True)
print(json.dumps({'schemaVersion':1,'sourceCommit':'a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2','timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'kernel':os.uname().release,'bootId':pathlib.Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'fileSha256':{p:sha(p) for p in files if pathlib.Path(p).is_file()},'installedAppNeeded':elf('/usr/bin/putgd'),'processes':procs,'packageVersions':v.stdout.splitlines(),'limits':'Identity only. No memory comparison, GUI acceptance or user config/environment inspection.'},indent=2))
