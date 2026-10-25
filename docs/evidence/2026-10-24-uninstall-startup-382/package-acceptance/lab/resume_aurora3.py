"""Resume Aurora after correcting the lab's Plasma Login Manager logout route."""
import json,time,hashlib,sys,subprocess,shlex
from pathlib import Path
from datetime import datetime
from campaign import Row
from full_rfb import RFB
import guest_control
root=Path('work/i382n/evidence/a382a/appimage');base='/home/lab/work/p382n';source=Path('work/i382n/i382-aurora3.py')
r=guest_control.guest('a382a',['sh','-c','cat > '+base+'/i382-aurora3.py'],source.read_text());assert not r['exit'],r
# Keep the first driver's identity and failure exactly as recorded.
correction={'reason':'Stopping plasmalogin alone left ksmserver/plasmashell from session 1 active in the user manager while session 69 was blank. Stop the owned Plasma workspace targets after stopping the DM, then restart for a genuine clean graphical login.','sourceCommit':'d5b561ddb94f8255b8bc754c1382552d983a6fa9','initialDriverSha256':hashlib.sha256(Path('work/i382n/i382.py').read_bytes()).hexdigest(),'correctedDriverSha256':hashlib.sha256(source.read_bytes()).hexdigest(),'correctedRemotePath':base+'/i382-aurora3.py','changedGate':False,'resumePoint':'before enabled logout/login; repeat that transition, then normal idle/remaining lifecycle'}
(root/'harness-correction3.json').write_text(json.dumps(correction,indent=2)+'\n')
r=RFB(5993);r.screenshot(str(root/'stale-workspace-after-stop.png'));r.close()
row=Row.__new__(Row);row.target='a382a';row.mode='appimage';row.artifact=base+'/putgd-26.1024.1186-dev+gd5b561ddb94f-x86_64.AppImage';row.out=root;row.physical=False;row.remote_home='/home/lab';row.base=base;row.heartbeat=time.monotonic();row.memory_assessment={}
prepare=json.loads(json.loads((root/'prepare-'+row.artifact.replace('/','_')+'.jsonl').read_text())['stdout']) if False else json.loads(json.loads(next(root.glob('prepare-*.jsonl')).read_text())['stdout'])
row.started=time.monotonic()-(time.time()-datetime.fromisoformat(prepare['at'].replace('Z','+00:00')).timestamp())
def remote(args,timeout=1200):return guest_control.guest('a382a',[base+'/i382-aurora3.py' if x==base+'/i382.py' else x for x in args],timeout=timeout)
row.remote=remote
row.login('enabled',True,already_out=True);row.frame('enabled-login-recovered3')
# Keep the original invalid frames; select the newly recovered complete frame.
visual=json.loads((root/'enabled-login-recovered3-visual.json').read_text());visual['selectionReason']='Recovered desktop after retiring stale Plasma user workspace; prior blank frames invalid';(root/'enabled-login-visual.json').write_text(json.dumps(visual,indent=2)+'\n')
row.call('identity');row.call('memory','180');row.run(after_memory=True)
# Map the accepted retry explicitly, retaining each original receipt.
selection={}
for stem in ['check-present-enabled','identity','logout-enabled','relogin-enabled']:
 candidates=list(root.glob(stem+'.*.jsonl'))+[root/(stem+'.jsonl')];candidates=[p for p in candidates if p.exists()]
 selection[stem]=max(candidates,key=lambda p:p.stat().st_mtime_ns).name
(root/'receipt-selection.json').write_text(json.dumps(selection,indent=2)+'\n')
