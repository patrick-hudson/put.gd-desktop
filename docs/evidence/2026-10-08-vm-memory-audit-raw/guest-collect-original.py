import datetime,hashlib,json,pathlib,re,struct,subprocess,sys
HOST_ROOT=pathlib.Path('/home/patrick/work/put.gd')
REMOTE=r'''
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

counters=('Rss','Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty','Anonymous','Swap','SwapPss')
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
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdecimal(): continue
 try: comm=(p/'comm').read_text().strip()
 except OSError: continue
 if comm!='putgd': continue
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
  entry['mappingTotals']=maps[:100]
  entry['omittedMappingGroups']=max(0,len(maps)-100)
  names={m['mapping'] for m in maps}
  entry['loadedFamilies']={
   'officialAwsSdkOrCrt':any(re.match(r'lib(?:aws|s2n)',n) for n in names),
   'curl':any(n.startswith('libcurl') for n in names),
   'qtNetwork':any(n.startswith('libQt') and 'Network' in n for n in names),
   'gtk':any(n.startswith(('libgtk','libgdk','libqgtk','libqgnome')) for n in names),
   'wayland':any('wayland' in n.lower() for n in names),
   'xcb':any('xcb' in n.lower() for n in names),
   'tlsCrypto':any(n.startswith(('libcrypto','libssl','libgnutls')) for n in names)}
 except OSError as e: entry['errors'].append({'type':type(e).__name__,'errno':e.errno})
 out['processes'].append(entry)
print(json.dumps(out))
'''

out={'schemaVersion':1,'scope':'Read-only existing-overlay SSH process memory attribution; no guest or app state changes, no graphical acceptance, no credential/config/payload/environment/cmdline reads.','hostCollectionUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'units':'All memory counters converted from proc kB to bytes (1024); summaries use MiB (1048576).','guestAttempts':[],'references':{'canonicalColdRssMiB':44.96484375,'canonicalEmptyThreeViewsHiddenRssMiB':73.82421875,'emptyViewAppSha256':'d00905ec1511eabfd4d3292e021b72cf019fb1eea8837d2c80a3b9a7ecb1d980'}}
for run_name in sys.argv[1:]:
 run=HOST_ROOT/'work/lab-runs'/run_name; state_path=run/'state.json'; d=json.loads(state_path.read_text())
 record={'run':run_name,'profile':d['profile'],'sshHost':'127.0.0.1','sshPort':d['ssh_port'],'vncPort':5900+d['vnc_display'],'stateManifestSha256':hashlib.sha256(state_path.read_bytes()).hexdigest(),'stateRecordedHostPid':d.get('pid')}
 host_proc=pathlib.Path('/proc')/str(d.get('pid'))
 try:
  host_name=(host_proc/'comm').read_text().strip(); host_stat=(host_proc/'stat').read_text(); host_fields=host_stat[host_stat.rfind(')')+2:].split(); host_start=int(host_fields[19]); record['hostQemuIdentity']={'comm':host_name,'startTicks':host_start,'dashboardId':hashlib.sha256((str(d.get('pid'))+':'+str(host_start)).encode()).hexdigest()[:20]}
 except OSError as e: record['hostQemuIdentity']={'error':type(e).__name__,'errno':e.errno}
 cmd=['ssh','-o','BatchMode=yes','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','ConnectTimeout=5','-o','UserKnownHostsFile='+d['known_hosts'],'-i',str(run/'identity'),'-p',str(d['ssh_port']),d['guest_user']+'@127.0.0.1','python3','-']
 try:
  r=subprocess.run(cmd,input=REMOTE,text=True,capture_output=True,timeout=35)
  record['sshExitCode']=r.returncode
  if r.returncode==0:
   record['guest']=json.loads(r.stdout)
   print(json.dumps({'run':run_name,'sshExitCode':0,'processes':[{'pid':p['pid'],'rssMiB':p.get('smapsRollup',{}).get('RssBytes',0)/1048576,'pssMiB':p.get('smapsRollup',{}).get('PssBytes',0)/1048576,'elapsedSeconds':p.get('elapsedSeconds'),'sha256':p.get('executable',{}).get('sha256'),'loaded':p.get('loadedFamilies'),'errors':p['errors']} for p in record['guest']['processes']]}),flush=True)
  else:
   record['sshError']=r.stderr.strip()[:700]
   print(json.dumps({'run':run_name,'sshExitCode':r.returncode,'error':record['sshError']}),flush=True)
 except (subprocess.TimeoutExpired,json.JSONDecodeError) as e:
  record['error']=type(e).__name__; print(json.dumps({'run':run_name,'error':type(e).__name__}),flush=True)
 out['guestAttempts'].append(record)
 pathlib.Path('/tmp/putgd-vm-memory-audit.json').write_text(json.dumps(out,indent=2)+'\n')
