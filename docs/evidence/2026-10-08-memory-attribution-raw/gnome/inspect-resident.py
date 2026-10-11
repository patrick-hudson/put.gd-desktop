#!/usr/bin/env python3
"""Read-only sanitized resident map attribution; no env/cmdline/config/payload reads."""
from pathlib import Path
from collections import defaultdict
import argparse,datetime,hashlib,json,os,re,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--pid',type=int,required=True);p.add_argument('--start-ticks',type=int,required=True);p.add_argument('--sha256',required=True);a=p.parse_args()
process=Path('/proc')/str(a.pid)
def digest(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()
def identity():
 stat=(process/'stat').read_text().rsplit(')',1)[1].split()
 return {'pid':a.pid,'startTicks':int(stat[19]),'sha256':digest(process/'exe'),'comm':(process/'comm').read_text().strip()}
initial=identity();assert initial=={'pid':a.pid,'startTicks':a.start_ticks,'sha256':a.sha256,'comm':'putgd'}
started=time.monotonic();raw=(process/'smaps').read_text();status=(process/'status').read_text();rollup=(process/'smaps_rollup').read_text()
fields=('Size','Rss','Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty','Anonymous','Swap','SwapPss','Locked')
def zero():return dict.fromkeys(fields,0)
def name_for(path):
 if path=='[heap]':return 'heap','[heap]'
 if path=='[stack]' or path.startswith('[stack:'):return 'stack','[stack]'
 if path.startswith('/memfd:') or path.startswith('memfd:'):
  return 'memfd','memfd:xorg' if 'xorg' in path else 'memfd:other'
 if not path:return 'anonymous','[anonymous]'
 if path.startswith('['):return 'kernel','[kernel-special]'
 safe=path.removesuffix(' (deleted)')
 if safe.startswith(('/usr/lib/','/lib/','/lib64/','/usr/lib64/')):
  return 'library',Path(safe).name
 if safe.startswith(('/usr/share/fonts/','/usr/local/share/fonts/')):return 'font',Path(safe).name
 if safe in ('/usr/bin/putgd',):return 'executable','putgd'
 return 'other-file','[redacted-file]'
groups=defaultdict(zero);counts=defaultdict(int);categories=defaultdict(zero);current=None
for line in raw.splitlines():
 m=re.match(r'^[0-9a-f]+-[0-9a-f]+\s+\S+\s+\S+\s+\S+\s+\d+(?:\s+(.*))?$',line)
 if m:
  current=name_for(m.group(1) or '');counts[current]+=1;continue
 if current:
  match=re.match(r'^(\w+):\s+(\d+) kB$',line)
  if match and match[1] in fields:groups[current][match[1]]+=int(match[2])*1024
rows=[]
for (category,name),values in groups.items():
 for field,value in values.items():categories[category][field]+=value
 rows.append({'category':category,'name':name,'mappingCount':counts[(category,name)],**{key+'Bytes':v for key,v in values.items()},'privateBytes':values['Private_Clean']+values['Private_Dirty'],'sharedBytes':values['Shared_Clean']+values['Shared_Dirty']})
rows.sort(key=lambda row:row['RssBytes'],reverse=True)
def proc_counters(text):
 return {m[1]:int(m[2])*1024 for m in re.finditer(r'^(\w+):\s+(\d+) kB$',text,re.M)}
final=identity();assert final==initial
needed=subprocess.run(['readelf','-d',str(process/'exe')],capture_output=True,text=True,timeout=10)
needed_names=re.findall(r'\(NEEDED\).*\[(.*?)\]',needed.stdout)
font_symbols=[row['name'] for row in rows if row['category']=='font']
result={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Single read-only sanitized native GNOME resident map attribution; no memory ownership mutation or renderer inference','scriptSha256':digest(__file__) if Path(__file__).is_file() else None,'identityBefore':initial,'identityAfter':final,'bootId':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'snapshotReadElapsedSeconds':time.monotonic()-started,'statusCounters':proc_counters(status),'rollupCounters':proc_counters(rollup),'categoryTotals':{k:{key+'Bytes':v for key,v in values.items()} for k,values in categories.items()},'mapGroups':rows,'sumRssBytes':sum(v['RssBytes'] for v in rows),'sumPssBytes':sum(v['PssBytes'] for v in rows),'sumPrivateBytes':sum(v['privateBytes'] for v in rows),'sumSharedBytes':sum(v['sharedBytes'] for v in rows),'directNeededLibraries':needed_names,'mappedLibrariesOfInterest':[row for row in rows if row['category']=='library' and any(part in row['name'].lower() for part in ['qt6','gtk','gdk','wayland','egl','glx','gallium','llvm','vulkan','aws','s2n','fontconfig','freetype','harfbuzz'])],'mappedFontBasenames':font_symbols,'limits':['smaps and status/rollup are sequential kernel reads, not atomic','RSS charges shared mappings in full; PSS apportions shared pages','Heap anonymous page residency does not identify live allocations versus allocator slack','An old process snapshot is contextual only; source/toolkit/workload/uptime differ','No renderer inferred from library presence alone']}
print(json.dumps(result,indent=2))
