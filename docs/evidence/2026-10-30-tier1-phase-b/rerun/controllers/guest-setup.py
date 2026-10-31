import json,sys
from pathlib import Path
from tools.lab import lab
run=Path(sys.argv[1]);desktop=sys.argv[2]
packages='python3-pyqt6 python3-pyatspi tesseract-ocr xdotool xclip wl-clipboard gstreamer1.0-gtk3 gnome-keyring'
# Refuse compositor, display stack and portal changes before installing tools.
plan=lab.ssh(run,'sudo -n apt-get -s install --no-install-recommends '+packages,timeout=90)
import re
for line in plan.splitlines():
 if line.startswith('Inst ') and re.search(r'\b(?:mutter|gnome-shell|kwin|plasma|xorg|mesa|pipewire|xdg-desktop-portal)',line):raise RuntimeError('desktop-package transaction refused')
print(lab.ssh(run,'sudo -n apt-get install -y --no-install-recommends '+packages,timeout=300))
print(lab.guest_session(run,"mkdir -m 700 -p /run/user/$(id -u)/t1-firefox\n",timeout=20))
if desktop=='kubuntu-kde-wayland':
 from tools.lab.surface_driver.compositor.stage_plasma_accessibility import stage
 # See module CLI for source paths.
 import subprocess
 subprocess.run([sys.executable,'tools/lab/surface_driver/compositor/stage_plasma_accessibility.py',str(run)],check=True)
elif desktop=='ubuntu-gnome-wayland':
 import subprocess
 subprocess.run([sys.executable,'tools/lab/surface_driver/compositor/stage_gnome_witness.py',str(run)],check=True)
 subprocess.run([sys.executable,'tools/lab/guest_reboot.py',str(run)],check=True)
if desktop=='mint-cinnamon-x11':
 print(lab.guest_session(run,"""python3 - <<'MINT'
from pathlib import Path
import os,signal,json,time
root=Path.home()/'.config/autostart';root.mkdir(parents=True,exist_ok=True)
masked=[]
for source in Path('/etc/xdg/autostart').glob('*.desktop'):
 if 'mintupdate' in source.name.lower():
  target=root/source.name
  target.write_text('[Desktop Entry]\\nType=Application\\nName=Disabled update manager for disposable acceptance guest\\nHidden=true\\n')
  masked.append(source.name)
stopped=[]
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:
  if proc.stat().st_uid!=os.getuid():continue
  args=(proc/'cmdline').read_bytes().split(b'\\0')
  exact=any(a in (b'/usr/bin/mintUpdate',b'/usr/bin/mintupdate-launcher',b'/usr/lib/linuxmint/mintUpdate/mintUpdate.py',b'/usr/lib/linuxmint/mintUpdate/mintUpdate',b'mintUpdate') for a in args)
  if exact:
   pid=int(proc.name);fd=os.pidfd_open(pid)
   try:signal.pidfd_send_signal(fd,signal.SIGTERM)
   finally:os.close(fd)
   stopped.append(pid)
 except (FileNotFoundError,ProcessLookupError,PermissionError):continue
time.sleep(2)
assert not any((Path('/proc')/str(pid)).exists() for pid in stopped)
assert masked
print(json.dumps(dict(maskedAutostart=masked,exactPidsStopped=stopped,overlayOnly=True)))
MINT
""",timeout=30))
print(lab.guest_session(run,"mkdir -m 700 -p /dev/shm/t1-firefox\n",timeout=20))
# Use a run-owned native Firefox, also on Ubuntu where the distro entry is snap.
import subprocess
browser=json.loads(subprocess.check_output([sys.executable,'tools/lab/stage_firefox.py','--run',str(run)],text=True))
import shutil
root=Path('/home/patrick/t1profiles');shutil.copytree('tools/lab/surface_driver/profiles',root,dirs_exist_ok=True)
fragment=root/(desktop+'.d/zz-tier1-live.json');fragment.parent.mkdir(parents=True,exist_ok=True)
fragment.write_text(json.dumps({'browser':{'command':browser['command'],'profile_root':'/dev/shm/t1-firefox'}}))
print('guest setup completed')
