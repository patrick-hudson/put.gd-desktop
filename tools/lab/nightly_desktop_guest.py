"""Existing Tier 1 campaign preparation, applied only to fresh owned overlays."""
import json
from pathlib import Path
import re
import shlex
import shutil
import sys

from tools.lab import lab

# These are probe/media/secret-service tools, never desktop or portal upgrades.
DEB_TOOLS = ('python3-pyqt6 python3-pyatspi python3-dbus python3-pil tesseract-ocr '
             'xdotool xclip wl-clipboard gstreamer1.0-gtk3 gnome-keyring')
RPM_TOOLS = ('python3-qt6 python3-pyatspi python3-dbus python3-pillow tesseract '
             'xdotool xclip wl-clipboard gtk3 gnome-keyring gstreamer1-plugins-good-gtk')

QUIET = r'''python3 - <<'NIGHTLY_QUIET'
from pathlib import Path
import configparser,json,os,shlex,signal,subprocess,time
# guest_session deliberately isolates the app. These writes instead belong to
# the disposable desktop account, so the actual Shell sees them.
os.environ['XDG_CONFIG_HOME']=str(Path.home()/'.config')
root=Path.home()/'.config/autostart';root.mkdir(parents=True,exist_ok=True)
def matches(args):
 if not args:return False
 executable=Path(args[0]).name
 return (executable=='update-notifier' or
         (executable=='gnome-software' and '--gapplication-service' in args[1:]) or
         any(a in ('/usr/lib/linuxmint/mintUpdate/mintUpdate.py','/usr/lib/linuxmint/mintUpdate/mintUpdate') for a in args))
names={'update-notifier.desktop','org.gnome.Software.desktop'}
for source in list(Path('/etc/xdg/autostart').glob('*.desktop'))+list(root.glob('*.desktop')):
 p=configparser.ConfigParser(interpolation=None,strict=False)
 try:
  p.read(source)
  if matches(shlex.split(p.get('Desktop Entry','Exec',fallback=''))):names.add(source.name)
 except (ValueError,configparser.Error):pass
for name in sorted(names):
 (root/name).write_text('[Desktop Entry]\nType=Application\nName=Disabled update nag in disposable nightly guest\nHidden=true\n')
stopped=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  if p.stat().st_uid!=os.getuid():continue
  args=[v.decode() for v in (p/'cmdline').read_bytes().split(b'\0') if v]
  if matches(args):
   fd=os.pidfd_open(int(p.name))
   try:signal.pidfd_send_signal(fd,signal.SIGTERM)
   finally:os.close(fd)
   stopped.append(int(p.name))
 except (OSError,UnicodeDecodeError):pass
settings={}
schemas=subprocess.check_output(['gsettings','list-schemas'],text=True).splitlines()
if 'org.gnome.software' in schemas:
 for key in ('download-updates','allow-updates'):
  subprocess.run(['gsettings','set','org.gnome.software',key,'false'],check=True,capture_output=True)
  assert subprocess.check_output(['gsettings','get','org.gnome.software',key],text=True).strip()=='false'
  settings[key]=False
if os.environ.get('XDG_CURRENT_DESKTOP','').find('GNOME')>=0:
 subprocess.run(['gsettings','set','org.gnome.desktop.notifications','show-banners','false'],check=True,capture_output=True)
 time.sleep(12)
print(json.dumps(dict(overlayOnly=True,maskedUserAutostart=sorted(names),exactPidsStopped=stopped,gnomeSoftware=settings)))
NIGHTLY_QUIET
'''

RAM = r'''python3 - <<'NIGHTLY_RAM'
from pathlib import Path
import json,os,subprocess
for name in ('nd-firefox','nd-downloads','nd-tmp'):
 root=Path('/dev/shm')/name;root.mkdir(mode=0o700)
 assert not root.is_symlink() and root.stat().st_uid==os.getuid()
 assert subprocess.check_output(['stat','-f','-c','%T',str(root)],text=True).strip()=='tmpfs'
downloads=Path.home()/'Downloads'
assert downloads.is_dir() and not downloads.is_symlink() and not any(downloads.iterdir())
downloads.rename(Path.home()/'Downloads.nightly-before')
downloads.symlink_to('/dev/shm/nd-downloads',target_is_directory=True)
print(json.dumps(dict(browserAndDownloadsInRam=True,emptyDownloadsPreserved=True)))
NIGHTLY_RAM
'''


def prepare(run, row, out):
    lab.verify_owned_guest(run)
    package = row['package']
    if package == 'deb':
        plan = lab.ssh(run, 'sudo -n apt-get -s install --no-install-recommends '+DEB_TOOLS, timeout=120)
        for line in plan.splitlines():
            if line.startswith('Inst ') and re.search(r'\b(?:mutter|gnome-shell|kwin|plasma|xorg|mesa|pipewire|xdg-desktop-portal)', line):
                raise RuntimeError('desktop-package transaction refused')
        (out/'probe-install.txt').write_text(lab.ssh(run, 'sudo -n apt-get install -y --no-install-recommends '+DEB_TOOLS, timeout=300))
    elif package == 'rpm':
        # Native Fedora golden already carries the system probes; refuse missing
        # dependencies instead of allowing dnf to upgrade a desktop stack.
        lab.ssh(run, "python3 -c 'import pyatspi,dbus;from PyQt6.QtGui import QGuiApplication'\ncommand -v tesseract\n")
    else:
        # Aurora's prepared system overlay has the probe and keyring packages.
        lab.ssh(run, "python3 -c 'import pyatspi,dbus;from PyQt6.QtGui import QGuiApplication'\ncommand -v tesseract\n")
    if 'gnome' in row['desktop']:
        from tools.lab.surface_driver.compositor.stage_gnome_witness import stage
        from tools.lab.guest_reboot import reboot
        (out/'witness.txt').write_text(stage(run, Path(__file__).parent/'surface_driver/compositor/gnome-witness'))
        (out/'reboot.json').write_text(json.dumps(reboot(run), indent=2))
    elif 'kde' in row['desktop']:
        from tools.lab.surface_driver.compositor.stage_plasma_accessibility import stage
        (out/'accessibility.txt').write_text(stage(run))
    (out/'nag-suppression.json').write_text(lab.guest_session(run, QUIET, timeout=60))
    (out/'ram-placement.json').write_text(lab.guest_session(run, RAM))
    from tools.lab.stage_firefox import stage
    browser = json.loads(stage(run))
    (out/'firefox.json').write_text(json.dumps(browser, indent=2))
    profiles = out/'profiles'
    shutil.copytree(Path(__file__).parent/'surface_driver/profiles', profiles)
    fragment = profiles/(row['desktop']+'.d/zz-nightly.json')
    fragment.parent.mkdir(parents=True, exist_ok=True)
    value = {'browser':dict(command=browser['command'], profile_root='/dev/shm/nd-firefox')}
    if row['desktop'] == 'aurora-kde-wayland':
        from tools.lab.surface_driver.profiles import load_profile
        kde = load_profile('kubuntu-kde-wayland')
        value.update(focus=kde['focus'], compositor=kde['compositor'], shell_health=kde['shell_health'])
    if row['desktop'] == 'fedora-gnome-wayland':
        from tools.lab.surface_driver.profiles import load_profile
        gnome = load_profile('ubuntu-gnome-wayland')
        value.update(focus=gnome['focus'], compositor=gnome['compositor'])
    fragment.write_text(json.dumps(value, indent=2))
    lab.guest_preflight(run, stage='nightly-ready')
    return profiles
