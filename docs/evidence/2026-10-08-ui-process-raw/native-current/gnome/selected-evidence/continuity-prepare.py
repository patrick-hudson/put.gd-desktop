from pathlib import Path
from PIL import Image
import sys,json,time,datetime
out=Path(__file__).resolve().parent;root=out.parents[3];sys.path.insert(0,str(out/'frozen/tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';dest=out/'continuity';dest.mkdir(exist_ok=False)
s=lab.cli(run,'settings.get')['document'];assert s['actions']['clipboard']['destination']=='local' and s['archive']['root']=='/home/lab/putgd-lab/archive';assert min(s['limits'][k] for k in ['maxFileBytes','maxBatchBytes','pendingArtifactBytes'])>512*1024*1024+100
assert lab.cli(run,'ui.snapshot')['diagnostics']['ui']['pid']==0
ready=json.loads(lab.guest_session(run,'cat ~/putgd-lab/fixture/gtk-ready.json\n'));assert ready['client']=='GTK' and 'Wayland' in ready['displayBackend']
prepare=(root/'work/u11/native/cinnamon/continuity-run1/prepare.sh').read_text().replace("assert (root/'ready.json').is_file()", "assert (root/'gtk-ready.json').is_file()")
(dest/'prepare.sh').write_text(prepare);payload=json.loads(lab.guest_session(run,prepare));lab.pointer(run,450,300,1280,800,True);lab.pointer(run,450,300,1280,800,False);lab.key(run,['ctrl','alt','shift','f']);time.sleep(.3)
lab.pointer(run,1158,16,1280,800,True);lab.pointer(run,1158,16,1280,800,False);time.sleep(.2);lab.pointer(run,1130,297,1280,800,True);lab.pointer(run,1130,297,1280,800,False);time.sleep(.8)
u=lab.cli(run,'ui.snapshot')['diagnostics']['ui'];assert u['pid']>0 and u['state']=='ready'
params={'schemaVersion':1,'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'payload':payload,'limits':s['limits'],'archive':s['archive'],'resident':lab.verify_running_app(run),'helperPid':u['pid'],'operationIdsBefore':[x['id'] for x in lab.cli(run,'operation.recent')['operations']],'scope':'One real 512MiB synthetic file plus tiny second file, GTK native clipboard, local archive. No S3, delay injection or memory claim.'};lab.write_json(dest/'parameters.json',params)
w=lab.screenshot(run,'u11-gnome-continuity-before-trigger');Image.open(w['path']).save(dest/'before-trigger.png');print(json.dumps({'helperPid':u['pid'],'payloadBytes':payload['size'],'witness':str(dest/'before-trigger.png')}))
