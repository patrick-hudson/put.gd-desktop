import json,shutil,hashlib,sys
from pathlib import Path
from tools.lab.surface_driver.redact import scan_run
row=sys.argv[1];run=Path(sys.argv[2]).resolve();out=Path('/home/patrick/t1r3export')/row
out.mkdir(mode=0o700,parents=True,exist_ok=False)
frames={}
e=Path('/home/patrick/t1r3e');prefix={'cinnamon':'cin','kde':'kde','gnome':'gno'}[row]
for attempt in ('pattern','s3-cancel','s3-delete','s3-delete2','s3a','s3b','s3c','s3d','s3-inspect','s3-detail','s3-reconcile','protona','protonb','protonc','post','cleanup','finish','scroll','last','qt64'):
 source=e/(prefix+'-'+attempt)
 if not source.exists():continue
 dest=out/attempt;dest.mkdir(mode=0o700)
 for name in ('receipt.json','coverage.json','commands.jsonl','frame-masks.json','pattern-occlusion.json'):
  if (source/name).exists():shutil.copy2(source/name,dest/name)
 if (source/'frames').exists():shutil.copytree(source/'frames',dest/'frames')
 if (source/'frame-masks.json').exists():
  for key,value in json.loads((source/'frame-masks.json').read_text())['frames'].items():frames[attempt+'/'+key]=value
 functional=source.with_name(source.name+'-functional')
 if functional.exists():shutil.copytree(functional,dest/'functional')
setup=out/'setup';setup.mkdir(mode=0o700)
for source in e.glob(prefix+'-*'):
 if source.is_file() and source.suffix in ('.json','.png','.txt') and 'coverage' not in source.name and 'export' not in source.name:
  shutil.copy2(source,setup/source.name)
info=json.loads((run/'state.json').read_text())
(setup/'run-identity.json').write_text(json.dumps({k:info.get(k) for k in ('id','profile','golden','golden_sha256','overlay','ssh_port','vnc_display','acceleration','guest_user','pid')},indent=2))
if (e/(prefix+'-ready.png')).exists():shutil.copy2(e/(prefix+'-ready.png'),setup/'unlocked-before-live.png')
# Persist only lifecycle states from the bridge, never credential/session bytes.
batons=[]
for folder in sorted((Path.home()/'putgd-lab/proton/receipts').iterdir()):
 p=folder/'lend.json'
 if not p.exists():continue
 v=json.loads(p.read_text())
 if v.get('run')!=str(run) or v.get('session')!='codex-tier1b-r3-6801919c':continue
 carried=[json.loads(p.read_text()).get('state') for p in folder.glob('carry-back-*.json')]
 provision=json.loads((folder/'provision.json').read_text())
 batons.append({'receiptDirectory':folder.name,'finalStep':v.get('step'),'testExit':v.get('testExit'),
                'carryBackStates':carried,'guestEntryState':provision.get('state')})
(out/'batons.json').write_text(json.dumps({'lends':batons},indent=2))
from PIL import Image
for p in out.rglob('*.png'):
 name=str(p.relative_to(out))
 if name not in frames:
  with Image.open(p) as im:frames[name]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'size':list(im.size),'regions':[],'guard':'precredential-desktop-or-synthetic-functional-payload'}
(out/'frame-masks.json').write_text(json.dumps({'frames':frames},indent=2))
result=scan_run(out,frames=frames,require_ocr=True,quarantine=False)
if not result['passed']:raise RuntimeError('curated evidence scan failed: '+json.dumps(result))
(out/'scan.json').write_text(json.dumps(result,indent=2))
files={str(p.relative_to(out)):hashlib.sha256(p.read_bytes()).hexdigest() for p in out.rglob('*') if p.is_file()}
(out/'sha256.json').write_text(json.dumps(files,indent=2))
print(json.dumps({'row':row,'files':len(files),'scanPassed':result['passed'],'batonsReturned':all(b['finalStep']=='cleaned' and 'replace_confirmed' in b['carryBackStates'] and b['guestEntryState']=='cleanup_confirmed' for b in batons)}))
