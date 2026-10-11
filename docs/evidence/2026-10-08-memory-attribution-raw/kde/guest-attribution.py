
import datetime,hashlib,json,os,pathlib,re,struct

def build_id(path):
 try:
  with open(path,'rb') as f:
   h=f.read(64)
   if h[:4]!=b'\x7fELF': return None
   endian='<' if h[5]==1 else '>'
   if h[4]==2:
    offset=struct.unpack_from(endian+'Q',h,32)[0]; entsize,num=struct.unpack_from(endian+'HH',h,54)
    form=endian+'IIQQQQQQ'; offidx,sizeidx=2,5
   else:
    offset=struct.unpack_from(endian+'I',h,28)[0]; entsize,num=struct.unpack_from(endian+'HH',h,42)
    form=endian+'IIIIIIII'; offidx,sizeidx=1,4
   for i in range(num):
    f.seek(offset+i*entsize); p=struct.unpack(form,f.read(struct.calcsize(form)))
    if p[0]!=4 or p[sizeidx]>4*1024*1024: continue
    f.seek(p[offidx]); data=f.read(p[sizeidx]); at=0
    while at+12<=len(data):
     ns,ds,nt=struct.unpack_from(endian+'III',data,at); at+=12
     name=data[at:at+ns]; at+=(ns+3)&~3
     desc=data[at:at+ds]; at+=(ds+3)&~3
     if nt==3 and name.rstrip(b'\0')==b'GNU': return desc.hex()
 except (OSError,ValueError,struct.error): return None

def digest(path):
 try:
  h=hashlib.sha256()
  with open(path,'rb') as f:
   for block in iter(lambda:f.read(1024*1024),b''): h.update(block)
  return h.hexdigest()
 except OSError: return None

counters=('Size','Referenced','LazyFree','Rss','Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty','Anonymous','Swap','SwapPss')
def count_values(text):
 out={}
 for line in text.splitlines():
  m=re.match(r'^([A-Za-z_]+):\s+(\d+)\s+kB',line)
  if m and m[1] in counters: out[m[1]+'Bytes']=int(m[2])*1024
 return out

def map_identity(path):
 if not path: return 'anonymous','[anonymous]'
 if path=='[heap]': return 'heap','[heap]'
 if path.startswith('[stack'): return 'stack','[stack]'
 if path.startswith('['): return 'kernel-region',path if path in ('[vvar]','[vdso]','[vsyscall]','[vvar_vclock]') else '[anonymous-region]'
 name=pathlib.PurePosixPath(path.removesuffix(' (deleted)')).name
 if 'memfd:' in path:
  return 'shared-image-or-memfd','memfd:xorg' if 'memfd:xorg' in path else 'memfd:[other]'
 if path.startswith('/SYSV'): return 'shared-image-or-memfd','[SYSV]'
 if name=='putgd': return 'app-executable','putgd'
 library=name.endswith('.so') or '.so.' in name
 system=path.startswith(('/usr/lib/','/lib/','/usr/lib64/','/lib64/','/usr/local/lib/'))
 if library and (system or re.match(r'lib(?:aws|s2n|putgd)',name)):
  if re.match(r'lib(?:aws|s2n)',name): category='aws-sdk-crt-s2n'
  elif name.startswith('libQt') and 'Network' in name: category='qt-network'
  elif name.startswith('libcurl') or name.startswith(('libssh','libnghttp','libkrb5','libgssapi')): category='network-dependencies'
  elif name.startswith(('libcrypto','libssl','libgnutls','libp11-kit','libnss','libnettle','libhogweed')): category='tls-crypto'
  elif name.startswith('libQt'): category='qt-other'
  elif name.startswith(('libqxcb','libqwayland','libqgtk','libqgnome','libgtk','libgdk','libpango','libcairo','libwayland','libX','libxcb','libfontconfig','libfreetype','libharfbuzz','libEGL','libGL','libvulkan','libQt6Xdg')): category='native-gui-font-rendering'
  elif name.startswith(('libsqlite','libqsqlite')): category='sql'
  elif name.startswith('libputgd'): category='private-app-module'
  else: category='other-system-library'
  return category,name
 if path.startswith(('/usr/share/fonts/','/usr/local/share/fonts/')) and name.endswith(('.ttf','.otf','.ttc')): return 'system-font',name
 return 'other-file','[redacted-file]'

def mappings(text):
 records=[]; cur=None
 for line in text.splitlines():
  if re.match(r'^[0-9a-f]+-[0-9a-f]+\s',line):
   parts=line.split(None,5); category,name=map_identity(parts[5] if len(parts)>5 else '')
   cur={'category':category,'mapping':name,'permissions':parts[1],'count':1,'counters':{}}
   records.append(cur)
  elif cur:
   vals=count_values(line); cur['counters'].update(vals)
 totals={}; categories={}
 for rec in records:
  for out,key in ((totals,(rec['category'],rec['mapping'])),(categories,rec['category'])):
   entry=out.setdefault(key,{'category':rec['category'],'mapping':rec['mapping'] if out is totals else None,'mappingCount':0,'counters':{},'permissions':set()})
   entry['mappingCount']+=1; entry['permissions'].add(rec['permissions'])
   for k,v in rec['counters'].items(): entry['counters'][k]=entry['counters'].get(k,0)+v
 def serial(out):
  arr=[]
  for x in out.values(): x['permissions']=sorted(x['permissions']); arr.append(x)
  return sorted(arr,key=lambda x:x['counters'].get('RssBytes',0),reverse=True)
 return serial(categories),serial(totals)

uptime=float(pathlib.Path('/proc/uptime').read_text().split()[0]); ticks=os.sysconf('SC_CLK_TCK')
out={'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'kernel':os.uname().release,'processes':[],'scanErrors':[]}
expected_pid=2958
expected_start=81226
expected_sha='8336dcdee8429dd0126b8b33609cf3ab6b7f5fc0fc607554a9eb69a5302603a1'
for p in [pathlib.Path('/proc')/str(expected_pid)]:
 if not p.name.isdecimal(): continue
 try: comm=(p/'comm').read_text().strip()
 except OSError: continue
 assert p.stat().st_uid==os.getuid()
 assert comm=='putgd'
 assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==expected_start
 assert digest(p/'exe')==expected_sha
 entry={'pid':int(p.name),'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'comm':comm,'errors':[]}
 try:
  link=os.readlink(p/'exe'); name=pathlib.PurePosixPath(link.removesuffix(' (deleted)')).name
  entry['executable']={'basename':name if name=='putgd' else '[redacted]','deleted':link.endswith(' (deleted)'),'sha256':digest(p/'exe'),'elfBuildId':build_id(p/'exe')}
  stat=(p/'stat').read_text(); fields=stat[stat.rfind(')')+2:].split()
  entry['startTicks']=int(fields[19]); entry['elapsedSeconds']=round(uptime-entry['startTicks']/ticks,3)
  status={}
  for line in (p/'status').read_text().splitlines():
   key,_,value=line.partition(':')
   if key in ('State','Threads'): status[key]=value.strip()
   elif key in ('VmRSS','VmHWM','RssAnon','RssFile','RssShmem','VmSwap'):
    status[key+'Bytes']=int(value.split()[0])*1024
  entry['status']=status
  entry['fdCount']=len(list((p/'fd').iterdir()))
  entry['taskCount']=len(list((p/'task').iterdir()))
  entry['smapsRollup']=count_values((p/'smaps_rollup').read_text())
  entry['categories'],maps=mappings((p/'smaps').read_text())
  entry['mappingCount']=sum(x['mappingCount'] for x in maps)
  entry['mappingTotals']=maps
  entry['omittedMappingGroups']=0
  names={m['mapping'] for m in maps}
  entry['loadedFamilies']={
   'officialAwsSdkOrCrt':any(re.match(r'lib(?:aws|s2n)',n) for n in names),
   'llvm':any('LLVM' in n for n in names),
   'gallium':any('gallium' in n.lower() for n in names),
   'privateS3Module':any(n.startswith('libputgd-s3') for n in names),
   'curl':any(n.startswith('libcurl') for n in names),
   'qtNetwork':any(n.startswith('libQt') and 'Network' in n for n in names),
   'gtk':any(n.startswith(('libgtk','libgdk','libqgtk','libqgnome')) for n in names),
   'wayland':any('wayland' in n.lower() for n in names),
   'xcb':any('xcb' in n.lower() for n in names),
   'tlsCrypto':any(n.startswith(('libcrypto','libssl','libgnutls')) for n in names)}
 except OSError as e: entry['errors'].append({'type':type(e).__name__,'errno':e.errno})
 assert int((p/'stat').read_text().rsplit(')',1)[1].split()[19])==expected_start
 assert digest(p/'exe')==expected_sha
 entry['identityUnchanged']=True
 out['processes'].append(entry)
print(json.dumps(out))
