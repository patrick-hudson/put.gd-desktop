#!/usr/bin/env python3
from pathlib import Path
import sys,json,time,datetime
root=Path(__file__).resolve().parents[3];sys.path.insert(0,str(root/'tools/lab'));import lab
run=root/'work/lab-runs/current-gnome-a3d70dc';state=lab.state(run);p=Path('/proc')/str(state['pid'])
assert p.exists() and str(run/'disk.qcow2').encode() in (p/'cmdline').read_bytes()
record={'schemaVersion':1,'timestampUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'scope':'Restart existing writable fresh GNOME test overlay only; discover installed system helper; no golden modification','oldHostPid':state['pid'],'bootIdBefore':lab.ssh(run,'cat /proc/sys/kernel/random/boot_id\n').strip(),'vncPort':5900+state['vnc_display'],'initialAttempt':'GNOME session reboot command did not restart the locked session'}
lab.ssh(run,'sync\n')
with lab.QMP(run/'qmp.sock') as qmp:record['acpiPowerdownReply']=qmp.execute('system_powerdown')
for _ in range(20):
 if not p.exists():break
 time.sleep(1)
if p.exists():
 with lab.QMP(run/'qmp.sock') as qmp:record['syncedOwnedQmpQuitReply']=qmp.execute('quit')
 for _ in range(20):
  if not p.exists():break
  time.sleep(.5)
assert not p.exists(),'Old overlay process still present; will not start duplicate'
profile=lab.load_profile(state['profile'])
argv=lab.qemu_command(profile,run,Path(state['overlay']),state['ssh_port'],state['vnc_display'],state['acceleration'])
reply=lab.command(argv,timeout=15,check=False)
assert reply.returncode==0,reply.stderr
state['pid']=int((run/'qemu.pid').read_text().strip());lab.write_json(run/'state.json',state)
record.update(newHostPid=state['pid'],qemuCommand=argv,oldHostProcessGone=True)
path=root/'work/cross-desktop-current/gnome/overlay-restart.json';lab.write_json(path,record)
for _ in range(60):
 time.sleep(2)
 try:
  boot=lab.ssh(run,'cat /proc/sys/kernel/random/boot_id\n',timeout=7).strip()
  if boot!=record['bootIdBefore']:
   record.update(bootIdAfter=boot,bootChanged=True);break
 except Exception:pass
lab.write_json(path,record);print(json.dumps(record))
