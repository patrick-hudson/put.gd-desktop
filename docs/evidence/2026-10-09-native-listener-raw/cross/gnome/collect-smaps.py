#!/usr/bin/env python3
"""Read-only post-memory mapping attribution; emits basenames, never paths."""
from pathlib import Path
from types import SimpleNamespace
import importlib.util
import json
import sys

ROOT = Path('/home/patrick/work/put.gd')
RUNNER = ROOT / 'work/resident-native-boundary/cross-desktop-replay.py'
GATE = '5536705e64b29fae8b586f0c9db9365f1968d77261e10d94c8cac35b43bbabd1'
OUTPUT = ROOT / 'work/ui-refinement/native-replay/nativecore-exp001-c49-cross'
spec = importlib.util.spec_from_file_location('cross_replay', RUNNER)
cross = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cross)
assert list((OUTPUT / 'gnome/attempts').glob('*-memory/memory.json')), 'Finish memory first'
replay = cross.CrossReplay(SimpleNamespace(output=str(OUTPUT), guest='gnome', phase='smaps-attribution', cinnamon_gates_sha256=GATE))
resident, snapshot = replay.assert_current()
assert not snapshot['diagnostics']['ui'].get('pid') and replay.cli('diagnostics')['activeOperations'] == 0
code = r'''from pathlib import Path
import collections,hashlib,json,os,re
expected=EXPECTED
p=Path('/proc')/str(expected['pid'])
def identity():
 assert p.stat().st_uid==os.getuid()
 ticks=int((p/'stat').read_text().rsplit(')',1)[1].split()[19])
 return {'pid':expected['pid'],'startTicks':ticks,'sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest()}
assert identity()==expected
fields=('Rss','Pss','Private_Clean','Private_Dirty','Shared_Clean','Shared_Dirty','Anonymous','Swap')
maps=[];current=None
for line in (p/'smaps').read_text().splitlines():
 if re.match(r'^[0-9a-f]+-[0-9a-f]+\s',line):
  parts=line.split(maxsplit=5);name=parts[5] if len(parts)==6 else '[anonymous]'
  # Full mapping names remain in memory only and are discarded immediately.
  base=name.rsplit('/',1)[-1]
  if base.endswith(' (deleted)'):base=base[:-10]
  current={'basename':base or '[anonymous]',**{f:0 for f in fields}}
  maps.append(current)
 elif current is not None:
  label,_,tail=line.partition(':')
  if label in fields:current[label]=int(tail.split()[0])*1024
def category(base):
 lower=base.lower()
 if base.startswith('[') or lower.startswith('memfd:'):return 'anonymous-or-shared-buffer'
 if any(x in lower for x in ('qt6quick','qt6qml')):return 'QtQuick-Qml'
 if any(x in lower for x in ('qt6gui','qt6opengl')):return 'QtGui-OpenGL'
 if any(x in lower for x in ('libgtk','libgdk','libpango','libcairo','libatk')):return 'GTK-native-theme'
 if any(x in lower for x in ('mesa','_dri.so','libllvm','libdrm','libgbm','libglx')):return 'Mesa-driver'
 if any(x in lower for x in ('libegl','libgles','libgl.so')):return 'EGL-GL'
 if any(x in lower for x in ('fontconfig','freetype')) or lower.endswith(('.ttf','.otf','.ttc')):return 'fontconfig-fonts'
 if 'qt6widgets' in lower:return 'QtWidgets'
 if base.startswith('putgd'):return 'application'
 return 'other-file-mappings'
groups={};basenames={}
for m in maps:
 group=groups.setdefault(category(m['basename']),{'mappingCount':0,**{f:0 for f in fields}})
 name=basenames.setdefault(m['basename'],{'mappingCount':0,**{f:0 for f in fields}})
 for row in (group,name):
  row['mappingCount']+=1
  for f in fields:row[f]+=m[f]
assert identity()==expected
print(json.dumps({'schemaVersion':1,'resident':expected,'totals':{f:sum(m[f] for m in maps) for f in fields},'groups':groups,'basenames':basenames,'fullPathsSaved':False,'environmentRead':False,'payloadMemoryRead':False,'scope':'Read-only smaps accounting after guest native-use memory; anonymous allocations cannot be attributed to an allocator owner from mappings alone'}))
'''
result = replay.python(code.replace('EXPECTED', repr(resident)))
after, _ = replay.assert_current()
assert after == resident
result['sourceCommit'] = snapshot['build']['gitCommit']
result['build'] = snapshot['build']
cross.replay.save(replay.attempt / 'smaps-attribution.json', result)
cross.replay.save(replay.attempt / 'complete.json', {'utc':cross.replay.now(),'phase':'smaps-attribution'})
print(json.dumps({'artifact':str(replay.attempt / 'smaps-attribution.json'),'resident':resident,'totals':result['totals'],'groups':result['groups']}))
