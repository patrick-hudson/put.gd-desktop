#!/usr/bin/env python3
"""Replay the committed native GNOME synthetic local-only matrix through the lab runner."""
import datetime,hashlib,json,pathlib,subprocess,sys,time
root=pathlib.Path(__file__).resolve().parents[3]
output=pathlib.Path(__file__).resolve().parent
run=root/'work/lab-runs/current-gnome-a3d70dc'
runner=root/'tools/lab/lab.py'
scenarios=[('qt',name) for name in ['region','window','desktop','clipboard-text','clipboard-image','clipboard-files','region-cancel-escape','region-cancel-right-click','region-cancel-zero-area']]+[('gtk',name) for name in ['clipboard-text','clipboard-image','clipboard-files']]
result={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Visible GNOME Wayland single-head synthetic local-only functional matrix; no cloud or memory comparison','run':str(run),'cases':[],'passed':False}
for client,scenario in scenarios:
 name=client+'-'+scenario
 argv=[sys.executable,str(runner),'run','--run',str(run),'--scenario',scenario,'--clipboard-client',client,'--timeout','90','--keep']
 started=time.monotonic()
 p=subprocess.run(argv,cwd=root,text=True,capture_output=True,timeout=180)
 (output/(name+'.stdout.json')).write_text(p.stdout)
 (output/(name+'.stderr.log')).write_text(p.stderr)
 try: parsed=json.loads(p.stdout)
 except json.JSONDecodeError: parsed={'parseError':True}
 item={'name':name,'argv':argv,'exitCode':p.returncode,'elapsedSeconds':time.monotonic()-started,'runnerSha256':hashlib.sha256(runner.read_bytes()).hexdigest(),'result':parsed}
 result['cases'].append(item)
 (output/'matrix.json').write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps({'name':name,'exitCode':p.returncode,'ok':parsed.get('ok'),'evidenceDirectory':parsed.get('evidenceDirectory')}),flush=True)
 if p.returncode or not parsed.get('ok'): sys.exit(1)
result['passed']=True
result['finishedUtc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
(output/'matrix.json').write_text(json.dumps(result,indent=2)+'\n')
