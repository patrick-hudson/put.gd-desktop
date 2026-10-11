#!/usr/bin/env python3
import hashlib, importlib.util, json, os, pathlib, re, select, subprocess, tempfile, time
root=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('idle',pathlib.Path('tests/performance/bench_idle.py').resolve());idle=importlib.util.module_from_spec(spec);spec.loader.exec_module(idle)
assert os.environ.get('XDG_SESSION_TYPE')=='x11' and 'Cinnamon' in os.environ.get('XDG_CURRENT_DESKTOP','')
assert subprocess.run(['systemd-detect-virt'],capture_output=True,text=True).stdout.strip()=='none'
app=root/'build/native-qt-cold'
def detail(pid):
 groups={};current=None
 for line in pathlib.Path('/proc',str(pid),'smaps').read_text().splitlines():
  if re.match(r'^[0-9a-f]+-[0-9a-f]+ ',line):
   parts=line.split(None,5);name=pathlib.Path(parts[5]).name if len(parts)==6 else '[anonymous]'
   key=(name,parts[1]);current=groups.setdefault(key,{'mapping':name,'permissions':parts[1]})
  elif ':' in line and current is not None:
   key,value=line.split(':',1)
   if key in ('Size','Rss','Pss','Shared_Clean','Shared_Dirty','Private_Clean','Private_Dirty','Anonymous','Swap'):
    current[key+'Bytes']=current.get(key+'Bytes',0)+int(value.split()[0])*1024
 return sorted(groups.values(),key=lambda row:-row.get('RssBytes',0))
results={'scope' :'Locating native QApplication overhead only; no product, no windows/tray/actions/network, not a lower-bound proof or performance gate', 'binarySha256':hashlib.sha256(app.read_bytes()).hexdigest(),'sourceSha256':hashlib.sha256((root/'probe.cpp').read_bytes()).hexdigest(),'cmakeSha256':hashlib.sha256((root/'CMakeLists.txt').read_bytes()).hexdigest(),'desktop':os.environ['XDG_CURRENT_DESKTOP'],'session':'x11','runs':[]}
for attempt in range(2):
 with tempfile.TemporaryDirectory(prefix='putgd-native-qt-') as temp:
  env=os.environ.copy();env['QT_QPA_PLATFORM']='xcb'
  for key,name in [('XDG_CONFIG_HOME','config'),('XDG_DATA_HOME','data'),('XDG_CACHE_HOME','cache'),('XDG_RUNTIME_DIR','runtime')]:
   path=pathlib.Path(temp)/name;path.mkdir(mode=0o700);env[key]=str(path)
  with (root/('stderr-'+str(attempt)+'.log')).open('wb') as log:
   process=subprocess.Popen([str(app)],env=env,stdout=subprocess.PIPE,stderr=log)
   try:
    assert select.select([process.stdout],[],[],15)[0], 'Native QApplication readiness timeout'
    report=json.loads(process.stdout.readline());assert report['gtkPlatformThemeMapped'] and report['pid']==process.pid
    report['rssBytes']=int(pathlib.Path('/proc',str(process.pid),'statm').read_text().split()[1])*os.sysconf('SC_PAGE_SIZE')
    report['footprint']=idle.footprint(process.pid)
    report['detailedFootprint']=detail(process.pid)
    assert process.wait(timeout=10)==0
    results['runs'].append(report)
    (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
   finally:
    if process.poll() is None:process.terminate();process.wait(timeout=10)
print(json.dumps({'scope':results['scope'],'rssBytes':[v['rssBytes'] for v in results['runs']]}))
