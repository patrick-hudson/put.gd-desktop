"""Existing Tier 1 campaign preparation, applied only to fresh owned overlays."""
import json
import fnmatch
from pathlib import Path
import re
import shlex
import shutil
import sys

from tools.lab import lab

# These are probe/media/secret-service tools, never desktop or portal upgrades.
DEB_TOOLS = ('python3-pyqt6 python3-pyatspi python3-dbus python3-pil tesseract-ocr '
             'xdotool x11-utils x11-xserver-utils xclip wl-clipboard gstreamer1.0-gtk3 gnome-keyring '
             'libqt6keychain1')  # The native stdin credential bridge also runs beside AppImages.

FEDORA_DESKTOP = ('gnome-shell*', 'mutter*', 'xdg-desktop-portal*', 'gdm*', 'pipewire*', 'kernel*')
FEDORA_TOOLS = ('python3-pyqt6 python3-pyatspi python3-dbus python3-pillow tesseract '
                'xdotool xprop xwininfo xrandr xclip wl-clipboard gstreamer1-plugins-good-gtk gnome-keyring '
                'openh264 gstreamer1-plugin-openh264')


def install_rpm(run, package, out):
    """Install the published RPM through the golden's authorized package manager.

    Runtime dependencies may advance in this disposable overlay; the desktop
    under test must retain its exact installed packages. Record the full RPM
    inventory delta as well as verifying the installed application bytes.
    """
    lab.verify_owned_guest(run)
    run, package, out = Path(run), Path(package), Path(out)
    (run/'package.json').unlink(missing_ok=True)

    def inventory():
        value = lab.ssh(run, "rpm -qa --qf '%{NAME}.%{ARCH} %{EPOCHNUM}:%{VERSION}-%{RELEASE}\\n'\n")
        packages = {}
        for line in value.splitlines():
            name, version = line.split(maxsplit=1)
            packages.setdefault(name, []).append(version)
        return {name: sorted(versions) for name, versions in packages.items()}

    def protected(packages):
        return {name: version for name, version in packages.items()
                if any(fnmatch.fnmatchcase(name, pattern) for pattern in FEDORA_DESKTOP)}

    before = inventory()
    lab.write_json(out/'rpm-before.json', before)
    lab.put(run, str(package), 'putgd-nightly.rpm')
    exclude = shlex.quote('--exclude='+','.join(FEDORA_DESKTOP))
    result = lab.command(lab.ssh_args(run)+['sh', '-s'], check=False, timeout=900,
        input=f'sudo -n dnf --color=never {exclude} install -y "$HOME/putgd-nightly.rpm" {FEDORA_TOOLS}\n')
    (out/'rpm-install.txt').write_text(result.stdout+'\n'+result.stderr)
    after = inventory()
    lab.write_json(out/'rpm-after.json', after)
    changes = [dict(package=name, before=before.get(name), after=after.get(name))
               for name in sorted(before.keys() | after.keys()) if before.get(name) != after.get(name)]
    report = dict(exitCode=result.returncode, excluded=list(FEDORA_DESKTOP),
                  desktopUnchanged=protected(before) == protected(after), packagesChanged=changes)
    lab.write_json(out/'rpm-transaction.json', report)
    if not report['desktopUnchanged']:
        raise RuntimeError('Fedora installation changed the protected desktop stack; private transaction retained')
    if result.returncode:
        raise RuntimeError('Fedora dnf installation failed; private transaction retained')
    installed = lab.verify_installed_binaries(run, package)
    receipt = dict(sha256=lab.hash_file(package), filename=package.name, format='rpm',
                   installedBinarySha256=installed, rpmTransaction=report)
    lab.write_json(run/'package.json', receipt)
    lab.write_json(out/'rpm-install.json', receipt)
    return receipt


GNOME_DESKTOP = r'''python3 - <<'NIGHTLY_GNOME'
import dbus,json,subprocess,time
from pathlib import Path
from gi.repository import Gio
settings=Gio.Settings.new('org.gnome.shell')
uuid='putgd-lab-witness@put.gd'
assert uuid in settings.get_strv('enabled-extensions'), 'lab witness is not enabled'
assert not settings.get_boolean('disable-user-extensions'), 'user extensions are disabled'
assert (Path.home()/'.local/share/gnome-shell/extensions'/uuid/'extension.js').is_file()
witness=dbus.Interface(dbus.SessionBus().get_object('org.gnome.Shell','/org/putgd/LabWitness'),
                       'org.putgd.LabWitness')
before=json.loads(str(witness.Desktop(timeout=3)))
assert before['witnessVersion']>=7, 'lab witness is stale'
assert before['locked'] is False, 'GNOME session is locked'
subprocess.run(['gnome-extensions','enable','putgd@put.gd'],check=True,capture_output=True)
assert witness.HideOverview(timeout=3), 'overview dismissal refused'
deadline=time.monotonic()+5
while True:
 after=json.loads(str(witness.Desktop(timeout=3)))
 if (after['overviewVisible'] is False and after['overviewAnimating'] is False and
     after['locked'] is False and after['nativeHelperEnabled'] is True):break
 assert time.monotonic()<deadline, 'GNOME desktop/native helper did not become ready'
 time.sleep(.1)
print(json.dumps(dict(witnessEnabled=True,before=before,after=after)))
NIGHTLY_GNOME
'''


def prepare_gnome_desktop(run, out):
    lab.verify_owned_guest(run)
    report = json.loads(lab.guest_session(run, 'export XDG_CONFIG_HOME="$HOME/.config"\n'+GNOME_DESKTOP))
    lab.write_json(Path(out)/'gnome-desktop.json', report)
    return report


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


def prepare_live_keyring(run, out):
    """Use the local fixture's unique GNOME connection alongside Atomic KWallet.

    The daemon and its blank collection live on the disposable guest's tmpfs.
    Its unique bus name is shared by the bridge and every private app resident.
    KWallet keeps its original owner, collections and default alias.
    """
    from tools.lab.surface_driver.fixtures.keyring import OWNER, UNIQUE_SERVICE
    lab.verify_owned_guest(run)
    prior = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+OWNER+'\nPY\n'))
    if prior['provider'] != 'ksecretd':
        return
    root = '/dev/shm/nd-keys'
    from tools.lab.surface_driver.target import checked_socket
    checked_socket(root+'/control/control')
    script = '''import json,os,subprocess
from pathlib import Path
root=Path(ROOT);root.mkdir(mode=0o700)
for name in ('config','data','cache','control'):(root/name).mkdir(mode=0o700)
with (root/'daemon.log').open('xb') as log:
 p=subprocess.Popen(['gnome-keyring-daemon','--foreground','--components=secrets',
                     '--control-directory='+str(root/'control')],
  env=dict(os.environ,XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache')),stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
print(json.dumps(dict(pid=p.pid,start=Path('/proc/%d/stat'%p.pid).read_text().rsplit(')',1)[1].split()[19])))
'''.replace('ROOT', repr(root))
    identity = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+script+'\nPY\n'))
    probe = UNIQUE_SERVICE.replace('IDENTITY', repr(identity))
    import time
    deadline = time.monotonic()+10
    while True:
        names = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+probe+'\nPY\n'))
        if len(names) == 1:
            break
        if len(names) > 1 or time.monotonic() >= deadline:
            raise RuntimeError('isolated live Secret Service connection unavailable or ambiguous')
        time.sleep(.2)
    # Establish the collection before importing either real credential.
    lab.put(run, str(Path(__file__).with_name('lab_keyring.py')), root+'/lab_keyring.py')
    info = lab.state(run)
    info['secret_service'] = names[0]
    prefix = lab.guest_secret_prefix(info)
    created = json.loads(lab.guest_session(run, prefix+'python3 '+root+'/lab_keyring.py'))
    status = json.loads(lab.guest_session(run, prefix+'python3 '+root+'/lab_keyring.py --status'))
    after = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+OWNER+'\nPY\n'))
    if after != prior or status.get('locked') is not False or status.get('default') != created['collection']:
        raise RuntimeError('isolated live keyring readiness or preserved KWallet owner failed')
    lab.write_json(run/'state.json', info)
    (out/'live-keyring.json').write_text(json.dumps(dict(identity, busName=names[0],
        root=root, collection=status, desktopServicePreserved=after == prior), indent=2))


def prepare(run, row, out):
    lab.verify_owned_guest(run)
    # AppImages also run on Debian-derived guests. Their probe dependencies
    # still come from apt; selecting the probes never changes the app artifact.
    package = row.get('probePackageManager', row['package'])
    if package == 'deb':
        plan = lab.ssh(run, 'sudo -n apt-get -s install --no-install-recommends '+DEB_TOOLS, timeout=120)
        for line in plan.splitlines():
            if line.startswith('Inst ') and re.search(r'\b(?:mutter|gnome-shell|kwin|plasma|xorg|mesa|pipewire|xdg-desktop-portal)', line):
                raise RuntimeError('desktop-package transaction refused')
        (out/'probe-install.txt').write_text(lab.ssh(run, 'sudo -n apt-get install -y --no-install-recommends '+DEB_TOOLS, timeout=300))
    elif package == 'rpm':
        # install_rpm provisions the distro probes in its protected transaction.
        pass  # Individual probe diagnostics are recorded below.
    else:
        # Reuse Phase A's Atomic overlay layering; the immutable source stays
        # read-only. rpm-ostree install layers tools onto the pinned base.
        components = 'kwin plasma-workspace qt6-qtbase qt6-qtwayland xdg-desktop-portal xdg-desktop-portal-kde'
        before = lab.ssh(run, 'rpm -q '+components).strip()
        script = '''set -eu
sudo -n systemctl stop uupd.service
sudo -n systemctl disable --now uupd.timer
sudo -n systemctl mask uupd.timer
missing=''
for package in python3-pyqt6 python3-pyatspi python3-dbus python3-pillow python3-gobject tesseract xdotool xprop xwininfo xrandr xclip wl-clipboard gstreamer1-plugins-good-gtk gnome-keyring; do
 if ! rpm -q "$package" >/dev/null; then missing="$missing $package"; fi
done
if [ -n "$missing" ]; then sudo -n rpm-ostree install --apply-live $missing; echo NIGHTLY_TOOLS_APPLIED; fi
'''
        preparation = lab.guest_session(run, script, timeout=1200)
        (out/'atomic-overlay.txt').write_text(preparation)
        if lab.ssh(run, 'rpm -q '+components).strip() != before:
            raise RuntimeError('Atomic overlay changed a desktop component; no acceptance run allowed')
    # Record individual missing probe names, never Python tracebacks or inputs.
    probes = lab.guest_session(run, '''python3 - <<'PROBES'
import importlib,json,shutil
modules={}
for name in ('pyatspi','dbus','PyQt6.QtGui'):
 try:importlib.import_module(name);modules[name]=True
 except Exception:modules[name]=False
print(json.dumps(dict(modules=modules,tools={name:bool(shutil.which(name)) for name in ('tesseract','xdotool','xprop','xwininfo','xrandr','xclip','wl-copy','wl-paste')})))
PROBES
''')
    (out/'guest-probes.json').write_text(probes)
    missing = [name for group in json.loads(probes).values() for name,present in group.items() if not present]
    if missing:
        raise RuntimeError('guest probe dependencies unavailable: '+', '.join(missing))
    if 'gnome' in row['desktop']:
        if row['package'] == 'appimage':
            # The image does not install a host Shell extension. Set up its
            # exact packaged extension in this disposable account before the
            # existing witness reboot, as required by GNOME AppImage setup.
            extension = '''session_data=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n 's/^XDG_DATA_HOME=//p')
export PUTGD_LAB_SHELL_DATA="${session_data:-$HOME/.local/share}"
python3 - <<'NIGHTLY_IMAGE_EXTENSION'
import hashlib,json,os,shutil
from pathlib import Path
source=Path.home()/'putgd-lab/appimage/squashfs-root/usr/share/gnome-shell/extensions/putgd@put.gd'
metadata=json.loads((source/'metadata.json').read_text())
assert metadata['uuid']=='putgd@put.gd'
target=Path(os.environ['PUTGD_LAB_SHELL_DATA'])/'gnome-shell/extensions/putgd@put.gd'
shutil.copytree(source,target,dirs_exist_ok=True)
files={p.relative_to(source).as_posix():hashlib.file_digest(p.open('rb'),'sha256').hexdigest() for p in source.rglob('*') if p.is_file()}
assert all(hashlib.file_digest((target/name).open('rb'),'sha256').hexdigest()==digest for name,digest in files.items())
print(json.dumps(dict(metadata=metadata,files=files,scope='exact AppImage extension installed into owned disposable GNOME account')))
NIGHTLY_IMAGE_EXTENSION
'''
            (out/'appimage-gnome-extension.json').write_text(lab.guest_session(run, extension))
        from tools.lab.surface_driver.compositor.stage_gnome_witness import stage
        from tools.lab.guest_reboot import reboot
        (out/'witness.txt').write_text(stage(run, Path(__file__).parent/'surface_driver/compositor/gnome-witness'))
        (out/'reboot.json').write_text(json.dumps(reboot(run), indent=2))
        prepare_gnome_desktop(run, out)
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
        # Fedora does not grant focus to every CLI-opened private window.
        # Request the witness's explicit dock-like activation before each
        # guarded gesture; the compositor hit-test/focus check still applies.
        value.update(focus=dict(gnome['focus'], raise_before_input=True), compositor=gnome['compositor'])
    fragment.write_text(json.dumps(value, indent=2))
    lab.guest_preflight(run, stage='nightly-ready')
    return profiles
