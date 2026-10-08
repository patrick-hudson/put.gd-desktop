
import datetime,json,pathlib,re,subprocess
out={'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'processes':[]}
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdecimal(): continue
 try: name=(p/'comm').read_text().strip()
 except OSError: continue
 if name!='putgd': continue
 data={'pid':int(p.name),'errors':[]}
 stat=(p/'stat').read_text(); data['startTicks']=int(stat[stat.rfind(')')+2:].split()[19])
 try:
  result=subprocess.run(['readelf','-d',str(p/'exe')],capture_output=True,text=True,timeout=8)
  data['elfNeeded']=re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',result.stdout)
  data['elfReadelfExit']=result.returncode
 except (OSError,subprocess.TimeoutExpired) as e: data['errors'].append(type(e).__name__)
 libs=set(); fonts=set(); named=[]
 for line in (p/'smaps').read_text().splitlines():
  if not re.match(r'^[0-9a-f]+-[0-9a-f]+\s',line): continue
  fields=line.split(None,5)
  if len(fields)<6: continue
  path=fields[5].removesuffix(' (deleted)'); base=pathlib.PurePosixPath(path).name
  if path.startswith(('/lib/','/lib64/','/usr/lib/','/usr/lib64/','/usr/local/lib/')) and (base.endswith('.so') or '.so.' in base): libs.add(base)
  elif path.startswith(('/usr/share/fonts/','/usr/local/share/fonts/')) and base.endswith(('.ttf','.otf','.ttc')): fonts.add(base)
 data['systemLibraryBasenames']=sorted(libs)[:256]; data['systemFontBasenames']=sorted(fonts)[:32]
 data['omittedSystemLibraries']=max(0,len(libs)-256)
 out['processes'].append(data)
try:
 result=subprocess.run(['dpkg-query','-W','-f','${Package} ${Version}\n','putgd','libqt6core6','libqt6core6t64'],text=True,capture_output=True,timeout=8)
 out['packageVersions']=[x for x in result.stdout.splitlines() if x and x.split()[0] in ('putgd','libqt6core6','libqt6core6t64')]; out['packageQueryExit']=result.returncode
except (OSError,subprocess.TimeoutExpired) as e: out['packageQueryError']=type(e).__name__
print(json.dumps(out))
