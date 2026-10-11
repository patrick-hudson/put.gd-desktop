import pathlib,json,re,os,hashlib,datetime,shutil
p=pathlib.Path('/proc/2958');assert (p/'comm').read_text().strip()=='putgd';assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==81226
records=[];cur=None
for line in (p/'smaps').read_text().splitlines():
 if re.match(r'^[0-9a-f]+-[0-9a-f]+\s',line):
  parts=line.split(None,5);path=parts[5] if len(parts)>5 else '';cur=None
  if 'memfd:' in path:
   name=path.split('memfd:',1)[1].removesuffix(' (deleted)')
   known=next((s for s in ('wayland','qt','Qt','xorg','pipewire','pulse','wlroots') if name.startswith(s)),None)
   cur={'familyPrefix':known or '[other]','exactFixedName':name if name in ('wayland-shm','QtRasterBackingStore','xorg','pipewire-memfd','memfd') else '[redacted-variable-name]','counters':{}};records.append(cur)
 elif cur and ':' in line:
  key,_,value=line.partition(':')
  if key in ('Size','Rss','Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty','Anonymous'):cur['counters'][key+'Bytes']=int(value.split()[0])*1024
paths=['/home/lab/putgd-native/build/sdk','/home/lab/putgd-native/build/dependencies/aws-sdk-cpp','/home/lab/putgd-current-a3d70dc','/usr/bin/cmake','/usr/bin/ninja','/usr/bin/g++']
plugins=sorted({pathlib.Path(line.split(None,5)[5].removesuffix(' (deleted)')).name for line in (p/'maps').read_text().splitlines() if len(line.split(None,5))==6 and '/usr/lib/' in line.split(None,5)[5] and '/qt6/plugins/' in line.split(None,5)[5]})
print(json.dumps({'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'pid':2958,'startTicks':81226,'memfdMappings':records,'nativeQtPluginBasenames':plugins,'knownBuildCachePaths':{s:pathlib.Path(s).exists() for s in paths},'limits':'Known fixed names/prefixes only; unknown memfd names redacted. Counter sum does not identify actual owner, renderer or allocation lifetime. Runtime build paths existence checked only, no credentials/config/environment read.'},indent=2))
