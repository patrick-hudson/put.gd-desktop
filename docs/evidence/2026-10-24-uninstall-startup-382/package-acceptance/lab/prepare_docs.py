"""Preserve verified results and add clearly separate annotations to unchanged evidence."""
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import json,hashlib,shutil,re
SRC=Path('work/i382n');DST=Path('work/i382/docs/docs/evidence/2026-10-24-uninstall-startup-382/package-acceptance');DST.mkdir(exist_ok=True)
font=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',18)
small=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',16)
centers={'f44':(730,274),'d69':(710,241),'k72':(710,237),'m74':(460,232),'u382a':(477,220),'a382a':(410,178)}
labels={'f44':'Fedora 44 GNOME 50.6 / Wayland (physical)','d69':'Debian 13 Plasma 6.3.6 / Wayland (physical)','k72':'Kubuntu 26.04 Plasma 6.6.6 / Wayland (physical)','m74':'Mint 22.3 Cinnamon 6.6.9 / X11 (physical)','u382a':'Ubuntu 26.04 GNOME 50.1 / Wayland (VM on Fedora)','a382a':'Aurora 45 Plasma 6.7.5 / Wayland (UEFI VM on Fedora)'}
rows=[];pictures=[]
for summary in sorted((SRC/'evidence').glob('*/*/summary.json')):
 root=summary.parent;d=json.loads(summary.read_text());host=d['target'];mode=d['mode'];out=DST/host/mode;out.mkdir(parents=True,exist_ok=True)
 # Keep full raw receipts privately. Published journals include startup-unit messages only.
 for phase in d['removalPhases'].values():
  j=phase['startupJournal'];raw=j['stdout'];j['unfilteredStdoutSha256']=hashlib.sha256(raw.encode()).hexdigest();j['unfilteredLineCount']=len(raw.splitlines());j['stdout']='\n'.join(x for x in raw.splitlines() if re.search(r'(?:app-[^ ]*putgd|putgd-login-)',x))+'\n';j['scope']='Only app-putgd autostart/scope and managed login-watch messages; original full result retained privately under exportReceipt'
 (out/'summary.json').write_text(json.dumps(d,indent=2)+'\n');shutil.copy2(root/'visual-review.json',out/'visual-review.json')
 for name in ['receipt-selection.json','harness-correction.json','harness-correction2.json','harness-correction3.json','fedora-qt-dependencies.txt','release-lock.txt','release-lock-retry.txt','non-owned-signer-process.json']:
  if (root/name).is_file():shutil.copy2(root/name,out/name)
 for label in ['login-settings','active-cleaned-login']:
  visual=json.loads((root/(label+'-visual.json')).read_text());raw=root/visual['file'];digest=hashlib.sha256(raw.read_bytes()).hexdigest();assert digest==d['visualReview']['files'][raw.name]
  im=Image.open(raw).convert('RGB');w,h=im.size;canvas=Image.new('RGB',(w,h+118),(15,23,42));canvas.paste(im,(0,118));draw=ImageDraw.Draw(canvas);title=f'{host} {mode}: {labels[host]}';draw.text((16,9),title,font=font,fill='white');draw.text((16,36),f'26.1024.1186-dev+gd5b561ddb94f | session {visual["sessionId"]} | {visual["at"]}',font=small,fill=(210,220,240))
  if label=='login-settings':
   x,y=centers[host];box=(x-18,y-18+118,x+145,y+18+118);draw.rectangle(box,outline=(240,90,32),width=4);note='Box: Launch at login enabled after real login; exact running build verified in summary.json.'
  else:
   draw.rectangle((3,121,w-4,h+114),outline=(240,90,32),width=4);note='Box: desktop after removal and real login. JSON verifies startup files/process absent, settings retained.'
  draw.text((16,64),note,font=small,fill=(255,205,150));draw.text((16,89),'Caption/boxes added; raw frame SHA-256: '+digest[:32]+'...',font=small,fill=(190,205,225))
  target=out/(label+'-annotated.png');canvas.save(target,optimize=True)
  pictures.append({'target':host,'mode':mode,'scenario':label,'rawFile':raw.name,'rawSha256':digest,'annotatedFile':str(target.relative_to(DST)),'annotatedSha256':hashlib.sha256(target.read_bytes()).hexdigest(),'visual':visual})
 rows.append({'target':host,'mode':mode,'platform':labels[host],'summary':str((out/'summary.json').relative_to(DST)),'sourceCommit':d['build']['gitCommit'],'artifact':d['artifact'],'peakRssMiB':d['memory']['peakRssMiB'],'endPssMiB':d['memory']['pssMiB'],'memoryAssessment':d['memoryAssessment'],'functionalPassed':d['completion']['functionalPassed'],'activeSecondsAfterRemovalReturned':d['activeRemovalObservation']['secondsAfterRemovalReturned'],'sessions':{'enabled':d['enabledLogin']['sessionId'],**{k:v['sessionId'] for k,v in d['removalPhases'].items()}}})
(DST/'matrix.json').write_text(json.dumps(rows,indent=2)+'\n');(DST/'pictures.json').write_text(json.dumps(pictures,indent=2)+'\n')
lab=DST/'lab';lab.mkdir(exist_ok=True)
for name in ['i382.py','i382-aurora.py','i382-aurora2.py','i382-aurora3.py','resume_aurora.py','resume_aurora2.py','resume_aurora3.py','campaign.py','control.py','guest_control.py','guest_login.py','full_rfb.py','collect.py','summarize.py','contact_sheet.py','export_guest.py','prepare_docs.py','fetch_artifact.py','fetch_metadata.py','stage_artifact.py']:
 shutil.copy2(SRC/name,lab/name)
shutil.copy2('docs/evidence/2026-10-23-resident-session-490/lab/r490.py',lab/'r490.py');shutil.copy2('tools/diagnostics/check_socket_paths.py',lab/'check_socket_paths.py');shutil.copy2('config/development.example.json',lab/'template.json')
for name in ['u382a-masked-login.png','u382a-greeter-2.png']:
 # These inspected references contain a blank field; never copy any post-input login frame.
 if (SRC/name).exists():shutil.copy2(SRC/name,lab/name)
art=DST/'artifacts';art.mkdir(exist_ok=True)
for name in ['fedora44','debian13','ubuntu24','ubuntu26','appimage','appimage-metadata','pr-appimage','pr-appimage-metadata']:
 out=art/name;out.mkdir(exist_ok=True)
 for p in (SRC/'artifacts'/name).glob('*.json'):shutil.copy2(p,out/p.name)
for name in ['branch-ci-state.json','pr-ci-state.json','pr-integration-sbom-failure.log']:
 shutil.copy2(SRC/name,DST/name)
print('Recorded',len(rows),'rows and',len(pictures),'annotated pictures')
