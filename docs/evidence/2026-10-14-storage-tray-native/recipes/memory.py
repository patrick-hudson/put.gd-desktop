import sys,json,subprocess,shlex,hashlib
from pathlib import Path
SRC=Path('/home/patrick/work/put.gd/work/i33');ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(SRC/'tools/lab'));import lab
r=Path(sys.argv[1]).resolve();phase=sys.argv[2];out=ROOT/r.name;out.mkdir(mode=0o700,exist_ok=True)
if lab.state(r)['profile']=='gnome-wayland':
 old=lab.guest_session
 def session(run,body):return old(run,'export WAYLAND_DISPLAY=wayland-0\n'+body)
 lab.guest_session=session
if phase=='start':
 resident=lab.verify_running_app(r);d=lab.cli(r,'ui.snapshot')['diagnostics'];assert d['ui']['pid']==0 and d['activeOperations']==0
 paths=['/tmp/i33m1','/tmp/i33m2'];pre=subprocess.check_output(['python3',str(SRC/'tools/diagnostics/check_socket_paths.py'),'--ui-output',*paths],text=True);(out/'memory-preflight.json').write_text(pre)
 h=SRC/'tests/performance/bench_ui_process.py';lab.put(r,str(h),'/tmp/i33-bench.py');commands=[]
 for p in paths:commands.append(['python3','/tmp/i33-bench.py','--mode','split','--app','/usr/bin/putgd','--cli','/usr/bin/putgd-cli','--helper','/usr/bin/putgd-ui','--source-revision',d['build']['gitCommit'],'--output',p,'--pid',str(resident['pid']),'--start-ticks',str(resident['startTicks']),'--socket','/run/user/1000/putgd/session.sock','--label','issue33-post-native-tray','--seconds','300','--interval','5'])
 script='import subprocess,json,os\nfrom pathlib import Path\ncommands='+repr(commands)+'\nresult={"clockTicksPerSecond":os.sysconf("SC_CLK_TCK"),"commands":commands,"exits":[]}\nfor argv in commands:\n p=subprocess.run(argv,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);result["exits"].append(p.returncode)\n if p.returncode:break\nPath("/tmp/i33-pair-result.json").write_text(json.dumps(result))\n';p=out/'memory-supervisor.py';p.write_text(script);lab.put(r,str(p),'/tmp/i33-pair.py');(out/'memory-command.json').write_text(json.dumps({'commands':commands,'resident':resident,'harnessSha256':hashlib.sha256(h.read_bytes()).hexdigest(),'preDiagnostics':d},indent=2));print(lab.guest_session(r,'nohup python3 /tmp/i33-pair.py > /tmp/i33-pair.log 2>&1 < /dev/null &\necho $!\n'))
elif phase=='collect':
 exists=json.loads(lab.ssh(r,"python3 - <<'IN'\nfrom pathlib import Path\nimport json\nprint(json.dumps(Path('/tmp/i33-pair-result.json').exists()))\nIN\n"))
 if not exists:print(json.dumps({'complete':False}));raise SystemExit(2)
 lab.get(r,'/tmp/i33-pair-result.json',out/'memory-pair-result.json');supervisor=json.loads((out/'memory-pair-result.json').read_text());assert supervisor['exits']==[0,0];gates=[]
 for index in [1,2]:
  for f in ['results.json','samples.jsonl']:lab.get(r,f'/tmp/i33m{index}/'+f,out/f'memory-{index}-{f}')
  result=json.loads((out/f'memory-{index}-results.json').read_text());rows=[json.loads(x) for x in (out/f'memory-{index}-samples.jsonl').read_text().splitlines()];dt=rows[-1]['elapsedSeconds']-rows[0]['elapsedSeconds'];peak=max(x['totals']['rssBytes'] for x in rows);zero=all(p['swapBytes']==0 for x in rows for p in x['processes']);quiet=all(x['processCount']==1 and x['uiProcessCount']==0 and x['escapedProcessCount']==0 for x in rows);ticks=rows[-1]['processes'][0]['cpuTicks']-rows[0]['processes'][0]['cpuTicks'];cpu=ticks/supervisor['clockTicksPerSecond']/dt*100
  gate={'passed':result['passed'] and dt>=300 and len(rows)>=60 and peak<=40*1024**2 and zero and quiet and cpu<.5,'peakMiB':peak/1024**2,'peakPssMiB':max(x['totals']['pssBytes'] for x in rows)/1024**2,'durationSeconds':dt,'samples':len(rows),'zeroSwap':zero,'noHelpers':quiet,'cpuPercentOneCore':cpu};gates.append(gate)
 d=lab.cli(r,'ui.snapshot')['diagnostics'];assert d['activeOperations']==0 and d['ui']['pid']==0;v={'passed':all(x['passed'] for x in gates),'scope':'two serial 300-second post-use guest resident observations; not physical acceptance or empty-view workload','windows':gates,'postDiagnostics':d};(out/'memory-gate.json').write_text(json.dumps(v,indent=2));print(json.dumps({'passed':v['passed'],'windows':gates}));raise SystemExit(0 if v['passed'] else 1)
