#!/usr/bin/env python3
"""Readonly post-idle, source49-bound mapping summaries. Mapping paths never leave guest."""
import argparse, importlib.util
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--cinnamon-gates-sha256',required=True);a=p.parse_args()
s=importlib.util.spec_from_file_location('cross',Path('/home/patrick/work/put.gd/work/resident-native-boundary/cross-desktop-replay.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
a.output=str(m.DEFAULT_OUTPUT);a.guest='kde';a.phase='post-idle-mapping-summary'
r=m.CrossReplay(a);resident,snapshot=r.assert_current()
assert not snapshot['diagnostics']['ui'].get('pid') and r.cli('diagnostics')['activeOperations']==0
code=r'''from pathlib import Path
import hashlib,json,os,re
expected=EXPECTED
proc=Path('/proc')/str(expected['pid'])
def verify():
 assert proc.stat().st_uid==os.getuid()
 fields=(proc/'stat').read_text().rsplit(')',1)[1].split()
 assert int(fields[19])==expected['startTicks']
 assert hashlib.sha256((proc/'exe').read_bytes()).hexdigest()==expected['sha256']
verify();groups={};item=None
for line in (proc/'smaps').read_text().splitlines():
 if re.match(r'^[0-9a-f]+-[0-9a-f]+ ',line):
  fields=line.split(None,5);name=Path(fields[5]).name if len(fields)>5 else '(anonymous)'
  item=groups.setdefault(name,{'mappingBasename':name,'mappingCount':0,'rssBytes':0,'pssBytes':0,'privateBytes':0,'anonymousBytes':0,'swapBytes':0});item['mappingCount']+=1
 elif item is not None:
  key,_,value=line.partition(':')
  target={'Rss':'rssBytes','Pss':'pssBytes','Private_Clean':'privateBytes','Private_Dirty':'privateBytes','Anonymous':'anonymousBytes','Swap':'swapBytes'}.get(key)
  if target:item[target]+=int(value.split()[0])*1024
verify()
categories={}
for row in groups.values():
 name=row['mappingBasename'].lower()
 category='other'
 if row['mappingBasename'] in ('(anonymous)','[heap]'):category='anonymous-and-heap'
 elif any(k in name for k in ('mesa','gallium','egl','libgl','vulkan','swrast','iris','libdrm','llvm')):category='Mesa-EGL-graphics'
 elif any(k in name for k in ('qt6quick','qt6qml')):category='QtQuick-QML'
 elif any(k in name for k in ('libgtk','libgdk')):category='GTK'
 elif any(k in name for k in ('font','freetype','pango','harfbuzz','.ttf','.otf')):category='fonts-text'
 elif 'qt6' in name:category='other-Qt'
 c=categories.setdefault(category,{k:0 for k in ('rssBytes','pssBytes','privateBytes','anonymousBytes','swapBytes','mappingCount')})
 for k in c:c[k]+=row[k]
print(json.dumps({'resident':expected,'topMappingBasenames':sorted(groups.values(),key=lambda v:v['pssBytes'],reverse=True)[:50],'categories':categories,'scope':'Idle post-use current resident mappings; basenames only; categories heuristic, anonymous allocation attribution not established'}))
'''
result=r.python(code.replace('EXPECTED',repr(resident)))
after,_=r.assert_current();assert after==resident
m.replay.save(r.attempt/'mapping-summary.json',{'utc':m.replay.now(),'build':snapshot['build'],'result':result,'collectorSha256':m.replay.sha(__file__)})
print(str(r.attempt/'mapping-summary.json'))
