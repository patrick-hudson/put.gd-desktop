import sys,json,pathlib,datetime,time,hashlib,argparse
sys.path.insert(0,'/home/patrick/work/put.gd/tools/lab')
import lab
root=pathlib.Path('/home/patrick/work/put.gd')
run=root/'work/lab-runs/current-kde-a3d70dc'
out=root/'work/cross-desktop-current/kde'
started=datetime.datetime.now(datetime.timezone.utc).isoformat()
scenarios=['clipboard-text','clipboard-image','clipboard-files']
results=[]
for scenario in scenarios:
 print('RUN',scenario,flush=True)
 args=argparse.Namespace(run=str(run),scenario=scenario,settings=None,history=False,clipboard_client='gtk',timeout=90,keep=True)
 result=lab.fixture_run(args)
 results.append(result)
 (out/'gtk-matrix-results.json').write_text(json.dumps({'schemaVersion':1,'sourceCommit':'a3d70dc2a6c6866fc6e38dd5e001553fb5c157a2','startedUtc':started,'updatedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'run':str(run),'scenarios':results,'isolation':'One fresh overlay; scenarios reuse its native session and resident. Per-case artifacts preserved. Local-only synthetic data.'},indent=2)+'\n')
 print('RESULT',scenario,result['ok'],result.get('error'),flush=True)
print('matrix finished',len(results),sum(r['ok'] for r in results),flush=True)
