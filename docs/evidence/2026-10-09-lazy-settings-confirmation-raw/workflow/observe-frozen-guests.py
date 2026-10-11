from pathlib import Path
from types import SimpleNamespace
import sys,json,time,hashlib
sys.path.insert(0,str(Path("/home/patrick/work/put.gd/work/ui-refinement/native-replay")))
import replay
name=sys.argv[1];r=replay.Replay(SimpleNamespace(output=str(replay.HERE/"frozen-exp001-source46"),guest=name,phase="post-native-memory-observation"))
resident,snapshot=r.assert_current();assert snapshot["diagnostics"]["ui"]["pid"]==0;assert r.cli("diagnostics")["activeOperations"]==0
guest="/home/lab/putgd-frozen46-memory"
r.shell("mkdir -p "+guest+"\n")
for file in ["bench_ui_process.py","bench_idle.py"]:r.put(replay.ROOT/"tests/performance"/file,guest+"/"+file)
source=r.source(); expected={f:source["files"]["tests/performance/"+f]["sha256"] for f in ["bench_ui_process.py","bench_idle.py"]}
script="import pathlib,sys,json,hashlib,time\n"+"p=pathlib.Path("+repr(guest)+"); expected="+repr(expected)+"\n"+"assert all(hashlib.sha256((p/f).read_bytes()).hexdigest()==h for f,h in expected.items())\nsys.path.insert(0,str(p))\nfrom bench_ui_process import ProcessFamily\nfrom bench_idle import footprint\n"+"family=ProcessFamily("+str(resident["pid"])+",'/usr/bin/putgd','/usr/bin/putgd-ui',"+str(resident["startTicks"])+")\n"+"rows=[]\nfor i in range(7):\n rows.append(family.sample())\n if i<6:time.sleep(5)\nprint(json.dumps({'scope':'30-second quiescent post-native synthetic-use observation; not cold physical or matched saving','samples':rows,'groupedFootprint':footprint("+str(resident["pid"])+"),'benchmarkHashes':expected}))\n"
result=r.python(script);after,ui=r.assert_current();assert after==resident and ui["diagnostics"]["ui"]["pid"]==0;assert r.cli("diagnostics")["activeOperations"]==0
replay.save(r.dest/"post-native-memory.json",{'utc':replay.now(),'sourceCommit':source['commit'],'resident':resident,'observation':result,'finalHelper':0,'finalActiveOperations':0})
print(json.dumps({'guest':name,'sourceCommit':source['commit'],'memoryObservationSaved':True}))
