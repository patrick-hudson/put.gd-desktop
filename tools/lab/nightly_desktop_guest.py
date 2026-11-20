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
# Fedora 45 Budgie: the panel, labwc and its bridge, the display manager, the
# portals, the media stack and the Secret Service the session starts.
BUDGIE_DESKTOP = ('budgie*', 'labwc*', 'wlroots*', 'gtk-layer-shell*', 'gtklock*', 'swaybg*', 'sddm*',
                  'xdg-desktop-portal*', 'pipewire*', 'wireplumber*', 'mesa*', 'oo7*', 'kernel*')
PROTECTED_DESKTOPS = {'fedora45-budgie': BUDGIE_DESKTOP}
FEDORA_TOOLS = ('python3-pyqt6 python3-pyatspi python3-dbus python3-pillow tesseract '
                'xdotool xprop xwininfo xrandr xclip wl-clipboard gstreamer1-plugins-good-gtk gnome-keyring '
                'openh264 gstreamer1-plugin-openh264')

# The Debian-family guests' own package updaters. Ubuntu's apt-daily-upgrade fires between 06:00 and 07:00
# guest time, which is when the nightly runs: on 2026-11-20 it upgraded openssh-server mid-row in the
# Kubuntu guest, restarting sshd under a live cell, so the cell and every cleanup step lost the guest.
# Fedora installs no updates by itself (dnf-makecache only refreshes metadata), Aurora's uupd is held by
# ATOMIC_TOOLS and Omarchy has no updater unit.
UPDATE_TIMERS = ('apt-daily.timer', 'apt-daily-upgrade.timer', 'mintupdate-automation-upgrade.timer',
                 'mintupdate-automation-autoremove.timer')
UPDATE_SERVICES = ('apt-daily.service', 'apt-daily-upgrade.service', 'mintupdate-automation-upgrade.service',
                   'mintupdate-automation-autoremove.service')
UPDATE_UNITS = UPDATE_TIMERS + UPDATE_SERVICES
# The apt goldens' only passwordless root command is apt-get, and their lab passwords differ (Mint's is
# not the standard one), so root steps run from apt-get's Pre-Invoke hook, as lab.py's helper staging
# does: no sources (nothing is fetched), no package cache written, and no lists lock, which a running
# apt-daily holds. No updater is ever stopped: apt-daily-upgrade's KillMode=process would leave
# unattended-upgrade running behind an inactive unit, and Mint's upgrade service would lose dpkg mid-way.
# The timers are stopped and masked, so nothing new starts; then a running or queued updater is left to
# finish (its unit inactive, no job, an empty cgroup) within the deadline, and only then masked.
UPDATE_DRAIN_SECONDS = 1200
# Idle needs positive evidence: a job list that was read, and a unit cgroup that a successful listing of
# system.slice doesn't show or whose cgroup.events reports `populated 0`. A failed read counts as busy.
_IDLE = r'''idle() {
 [ -e /sys/fs/cgroup/cgroup.controllers ] || return 1
 case "$(systemctl show -P ActiveState "$1" 2>/dev/null)" in inactive|failed) ;; *) return 1 ;; esac
 jobs=$(systemctl list-jobs --no-legend --no-pager 2>/dev/null) || return 1
 printf '%s\n' "$jobs" | awk '{print $2}' | grep -qxF -- "$1" && return 1
 groups=$(ls -A /sys/fs/cgroup/system.slice 2>/dev/null) || return 1
 if printf '%s\n' "$groups" | grep -qxF -- "$1"; then
  grep -qx 'populated 0' "/sys/fs/cgroup/system.slice/$1/cgroup.events" 2>/dev/null || return 1
 fi
 return 0
}
'''
HOLD_UPDATES = (r'''set -u
# The root hooks live in a private directory only lab can write; any failure to make them stops here.
hold=$(mktemp -d) || exit 1
''' + _IDLE + r'''as_root() {
 command -v apt-get >/dev/null 2>&1 || return 0
 LC_ALL=C sudo -n /usr/bin/apt-get -o Debug::NoLocking=1 -o Dir::Etc::sourcelist=/dev/null \
  -o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 -o Dir::Cache::pkgcache= -o Dir::Cache::srcpkgcache= \
  -o "APT::Update::Pre-Invoke::=sh $1" update >/dev/null 2>&1 < /dev/null || true
}
for unit in @TIMERS@ @SERVICES@; do
 printf '%s %s\n' "$unit" "$(systemctl show -P LoadState "$unit" 2>/dev/null)"
done > "$hold/before" || exit 1
cat > "$hold/timers" <<'NIGHTLY_TIMERS' || exit 1
for unit in @TIMERS@; do
 if [ "$(systemctl show -P LoadState "$unit" 2>/dev/null)" = loaded ]; then
  systemctl stop "$unit" >/dev/null 2>&1 && systemctl mask "$unit" >/dev/null 2>&1
 fi
done
exit 0
NIGHTLY_TIMERS
as_root "$hold/timers"
deadline=$(( $(date +%s) + @DRAIN@ ))
while :; do
 busy=
 for unit in @SERVICES@; do idle "$unit" || busy=1; done
 [ -z "$busy" ] || [ "$(date +%s)" -ge "$deadline" ] && break
 sleep 5
done
{ cat <<'NIGHTLY_IDLE'
''' + _IDLE + r'''NIGHTLY_IDLE
cat <<'NIGHTLY_SERVICES'
for unit in @SERVICES@; do
 if [ "$(systemctl show -P LoadState "$unit" 2>/dev/null)" = loaded ] && idle "$unit"; then
  systemctl mask "$unit" >/dev/null 2>&1
 fi
done
exit 0
NIGHTLY_SERVICES
} > "$hold/services" || exit 1
as_root "$hold/services"
while read -r unit before; do
 if idle "$unit"; then drained=drained; else drained=busy; fi
 printf '%s %s %s %s %s\n' "$unit" "${before:-unknown}" "$(systemctl show -P LoadState "$unit" 2>/dev/null)" \
  "$(systemctl show -P ActiveState "$unit" 2>/dev/null)" "$drained"
done < "$hold/before"
rm -f -- "$hold/before" "$hold/timers" "$hold/services"
rmdir -- "$hold"
''').replace('@TIMERS@', ' '.join(UPDATE_TIMERS)).replace(
    '@SERVICES@', ' '.join(UPDATE_SERVICES)).replace('@DRAIN@', str(UPDATE_DRAIN_SECONDS))


def hold_package_updates(run, out):
    """Hold the guest's own package updaters in this disposable overlay, before anything is installed.

    Every unit must end absent or masked, not running, with no queued job and no process left in its
    cgroup, or the row stops here: an updater left running can restart sshd or the desktop under a live
    cell. The report is written whatever happens.
    """
    lab.verify_owned_guest(run)
    report = dict(overlayOnly=True, units={})
    try:
        for line in lab.ssh(run, HOLD_UPDATES, timeout=UPDATE_DRAIN_SECONDS+300).splitlines():
            fields = line.split()
            if len(fields) == 5 and fields[0] in UPDATE_UNITS:
                report['units'][fields[0]] = dict(before=fields[1], after=fields[2], active=fields[3],
                                                  drained=fields[4])
    except Exception as error:
        report['error'] = type(error).__name__
        raise
    finally:
        units = report['units']
        held = {name for name, unit in units.items()
                if unit['after'] in ('not-found', 'masked') and unit['active'] in ('inactive', 'failed')
                and unit['drained'] == 'drained'}
        report.update(held=sorted(name for name in held if units[name]['after'] == 'masked'),
                      notHeld=sorted(set(UPDATE_UNITS) - held))
        lab.write_json(Path(out)/'package-updates.json', report)
    if report['notHeld']:
        raise RuntimeError('guest package updaters could not be held: '+', '.join(report['notHeld']))
    return report


def rpm_inventory(run):
    """Every installed RPM in the guest: {name.arch: [epoch:version-release]}."""
    value = lab.ssh(run, "rpm -qa --qf '%{NAME}.%{ARCH} %{EPOCHNUM}:%{VERSION}-%{RELEASE}\\n'\n")
    packages = {}
    for line in value.splitlines():
        name, version = line.split(maxsplit=1)
        packages.setdefault(name, []).append(version)
    return {name: sorted(versions) for name, versions in packages.items()}


def protected_packages(packages, desktop):
    return {name: version for name, version in packages.items()
            if any(fnmatch.fnmatchcase(name, pattern) for pattern in desktop)}


# Stock Fedora GNOME has no tray host. A Fedora user who wants put.gd's tray installs and enables
# Fedora's AppIndicator extension, which serves org.kde.StatusNotifierWatcher (docs/desktops/gnome.md).
TRAY_HOST_PACKAGE = 'gnome-shell-extension-appindicator'
TRAY_HOST_EXTENSION = 'appindicatorsupport@rgcjonas.gmail.com'
# The tray host joins the desktop; nothing it brings may move the Shell, Mutter, GDM or the portals.
TRAY_HOST_PROTECTED = ('gnome-shell.*', 'gnome-shell-common.*', 'gnome-shell-extension-common.*', 'mutter*',
                       'xdg-desktop-portal*', 'gdm*', 'pipewire*', 'kernel*')


def install_tray_host(run, out):
    """Install Fedora's AppIndicator extension in the disposable guest, before the Shell restarts."""
    lab.verify_owned_guest(run)
    run, out = Path(run), Path(out)
    before = rpm_inventory(run)
    exclude = shlex.quote('--exclude=' + ','.join(p.rstrip('.*') if p.endswith('.*') else p
                                                   for p in TRAY_HOST_PROTECTED))
    result = lab.command(lab.ssh_args(run)+['sh', '-s'], check=False, timeout=900,
        input=f'sudo -n dnf --color=never {exclude} install -y {TRAY_HOST_PACKAGE}\n')
    (out/'tray-host-install.txt').write_text(result.stdout+'\n'+result.stderr)
    after = rpm_inventory(run)
    changes = [dict(package=name, before=before.get(name), after=after.get(name))
               for name in sorted(before.keys() | after.keys()) if before.get(name) != after.get(name)]
    report = dict(exitCode=result.returncode, package=TRAY_HOST_PACKAGE, packagesChanged=changes,
                  installed=any(name.startswith(TRAY_HOST_PACKAGE + '.') for name in after),
                  desktopUnchanged=protected_packages(before, TRAY_HOST_PROTECTED) == protected_packages(after, TRAY_HOST_PROTECTED),
                  # New packages only: an upgrade of something the desktop already runs is refused.
                  nothingUpgraded=all(c['before'] is None for c in changes))
    lab.write_json(out/'tray-host.json', report)
    if result.returncode or not report['installed']:
        raise RuntimeError('Fedora AppIndicator tray host could not be installed; private transaction retained')
    if not report['desktopUnchanged'] or not report['nothingUpgraded']:
        raise RuntimeError('installing the AppIndicator tray host changed the desktop; private transaction retained')
    return report


TRAY_HOST_READY = r'''python3 - <<'NIGHTLY_TRAY_HOST'
import dbus,json,subprocess,time
uuid='EXTENSION'
subprocess.run(['gnome-extensions','enable',uuid],check=True,capture_output=True)
bus=dbus.SessionBus()
deadline=time.monotonic()+20
while True:
 state=None
 try:
  watcher=bus.get_object('org.kde.StatusNotifierWatcher','/StatusNotifierWatcher')
  state=bool(dbus.Interface(watcher,'org.freedesktop.DBus.Properties').Get(
   'org.kde.StatusNotifierWatcher','IsStatusNotifierHostRegistered'))
 except dbus.DBusException:pass
 if state:break
 assert time.monotonic()<deadline, 'no StatusNotifierWatcher with a registered host after enabling '+uuid
 time.sleep(.25)
info=subprocess.run(['gnome-extensions','info',uuid],capture_output=True,text=True).stdout
print(json.dumps(dict(extension=uuid,hostRegistered=True,info=[l.strip() for l in info.splitlines() if l.strip()])))
NIGHTLY_TRAY_HOST
'''.replace('EXTENSION', TRAY_HOST_EXTENSION)


def install_rpm(run, package, out):
    """Install the published RPM through the golden's authorized package manager.

    Runtime dependencies may advance in this disposable overlay; the desktop
    under test must retain its exact installed packages. Record the full RPM
    inventory delta as well as verifying the installed application bytes.
    """
    info = lab.verify_owned_guest(run)
    run, package, out = Path(run), Path(package), Path(out)
    (run/'package.json').unlink(missing_ok=True)
    # The guest's own profile names the desktop stack that must not move.
    desktop = PROTECTED_DESKTOPS.get(info.get('profile'), FEDORA_DESKTOP)

    def inventory():
        return rpm_inventory(run)

    def protected(packages):
        return protected_packages(packages, desktop)

    before = inventory()
    lab.write_json(out/'rpm-before.json', before)
    lab.put(run, str(package), 'putgd-nightly.rpm')
    exclude = shlex.quote('--exclude='+','.join(desktop))
    result = lab.command(lab.ssh_args(run)+['sh', '-s'], check=False, timeout=900,
        input=f'sudo -n dnf --color=never {exclude} install -y "$HOME/putgd-nightly.rpm" {FEDORA_TOOLS}\n')
    (out/'rpm-install.txt').write_text(result.stdout+'\n'+result.stderr)
    after = inventory()
    lab.write_json(out/'rpm-after.json', after)
    changes = [dict(package=name, before=before.get(name), after=after.get(name))
               for name in sorted(before.keys() | after.keys()) if before.get(name) != after.get(name)]
    report = dict(exitCode=result.returncode, excluded=list(desktop),
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


ISOLATED_SECRET_SERVICES = ('ksecretd', 'oo7-daemon')


LIVE_KEYRING_ROOT = '/dev/shm/nd-keys'
# One gnome-keyring-daemon for the live keyring. FRESH makes its folders on the guest's
# tmpfs; a restart reuses them, so the blank collection and its items come back as they were.
LIVE_KEYRING_DAEMON = '''import json,os,subprocess
from pathlib import Path
root=Path(ROOT)
if FRESH:
 root.mkdir(mode=0o700)
 for name in ('config','data','cache','control'):(root/name).mkdir(mode=0o700)
log=root/'daemon.log'
for index in range(1,100):
 if FRESH or not log.exists():break
 log=root/('daemon-restart-%d.log'%index)
with log.open('xb') as stream:
 p=subprocess.Popen(['gnome-keyring-daemon','--foreground','--components=secrets',
                     '--control-directory='+str(root/'control')],
  env=dict(os.environ,XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),
           XDG_CACHE_HOME=str(root/'cache')),stdin=subprocess.DEVNULL,stdout=stream,stderr=stream,start_new_session=True)
print(json.dumps(dict(pid=p.pid,start=Path('/proc/%d/stat'%p.pid).read_text().rsplit(')',1)[1].split()[19])))
'''
# Read only: whether the recorded connection still has an owner, and which daemons still
# serve this keyring's control folder (none may be running when it is started again).
LIVE_KEYRING_STATE = '''import json,os
from pathlib import Path
from gi.repository import Gio,GLib
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
owned=bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','NameHasOwner',
 GLib.Variant('(s)',(NAME,)),GLib.VariantType('(b)'),Gio.DBusCallFlags.NONE,3000,None).unpack()[0]
control=('--control-directory='+ROOT+'/control').encode()
daemons=[]
for entry in Path('/proc').iterdir():
 try:
  if entry.name.isdigit() and entry.stat().st_uid==os.getuid() and control in (entry/'cmdline').read_bytes().split(b'\\0'):
   daemons.append(int(entry.name))
 except OSError:pass
print(json.dumps(dict(owned=owned,daemons=daemons,data=(Path(ROOT)/'data/keyrings').is_dir())))
'''


class KeyringRestartRefused(RuntimeError):
    """The live keyring was not started again; the message is fixed text, safe to record."""


def _start_live_keyring(run, fresh):
    from tools.lab.surface_driver.fixtures.keyring import UNIQUE_SERVICE
    import time
    script = LIVE_KEYRING_DAEMON.replace('ROOT', repr(LIVE_KEYRING_ROOT)).replace('FRESH', repr(fresh))
    identity = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+script+'\nPY\n'))
    probe = UNIQUE_SERVICE.replace('IDENTITY', repr(identity))
    deadline = time.monotonic()+10
    try:
        while True:
            names = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+probe+'\nPY\n'))
            if len(names) == 1:
                return identity, names[0]
            if len(names) > 1 or time.monotonic() >= deadline:
                raise RuntimeError('isolated live Secret Service connection unavailable or ambiguous')
            time.sleep(.2)
    except BaseException:
        _stop_live_keyring(run, identity)  # never a daemon nobody can name
        raise


def _stop_live_keyring(run, identity):
    """End exactly the daemon this restart started (pid and start time), best effort."""
    pid, start = int(identity['pid']), str(identity['start'])
    if not start.isdigit():
        return
    try:
        lab.guest_session(run, f'''p=/proc/{pid}
[ "$(sed 's/.*) //' $p/stat 2>/dev/null | cut -d' ' -f20)" = {start} ] && kill -TERM {pid} || true
''')
    except (lab.LabError, OSError, ValueError):
        pass


def prepare_live_keyring(run, out):
    """Use the local fixture's unique GNOME connection alongside Atomic KWallet.

    The daemon and its blank collection live on the disposable guest's tmpfs.
    Its unique bus name is shared by the bridge and every private app resident.
    KWallet keeps its original owner, collections and default alias.
    """
    from tools.lab.surface_driver.fixtures.keyring import OWNER
    lab.verify_owned_guest(run)
    prior = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+OWNER+'\nPY\n'))
    # Atomic's KWallet (ksecretd) and Fedora 45 Budgie's oo7-daemon, whose login
    # collection stays locked after autologin, keep their name and collections.
    if prior['provider'] not in ISOLATED_SECRET_SERVICES:
        return
    root = LIVE_KEYRING_ROOT
    from tools.lab.surface_driver.target import checked_socket
    checked_socket(root+'/control/control')
    identity, name = _start_live_keyring(run, fresh=True)
    # Establish the collection before importing either real credential.
    lab.put(run, str(Path(__file__).with_name('lab_keyring.py')), root+'/lab_keyring.py')
    info = lab.state(run)
    info['secret_service'] = name
    prefix = lab.guest_secret_prefix(info)
    created = json.loads(lab.guest_session(run, prefix+'python3 '+root+'/lab_keyring.py'))
    status = json.loads(lab.guest_session(run, prefix+'python3 '+root+'/lab_keyring.py --status'))
    after = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+OWNER+'\nPY\n'))
    if after != prior or status.get('locked') is not False or status.get('default') != created['collection']:
        raise RuntimeError('isolated live keyring readiness or preserved KWallet owner failed')
    lab.write_json(run/'state.json', info)
    (out/'live-keyring.json').write_text(json.dumps(dict(identity, busName=name,
        root=root, collection=status, desktopServicePreserved=after == prior), indent=2))


def restart_live_keyring(run):
    """Start the guest's live keyring daemon again if it has died; None when the guest has none.

    The recorded unique connection must have no owner and no daemon may still serve the
    keyring's folder: a daemon that is only slow is never replaced. The desktop's own Secret
    Service must be up before and unchanged after, so the new daemon never takes its name.
    The new daemon reads the same tmpfs files, so the blank collection and the lent entry are
    back once it is opened with its empty password. Only then does the run's state name the
    new connection for the bridge and every later guest step. One restart per run at a time.
    Returns only connection names, the process identity and the collection's label and lock state.
    """
    import fcntl
    from tools.lab.surface_driver.fixtures.keyring import OWNER
    run = Path(run)
    lab.verify_owned_guest(run)
    with (run/'live-keyring-restart.lock').open('a') as serial:
        fcntl.flock(serial, fcntl.LOCK_EX)
        info = lab.state(run)
        previous = info.get('secret_service')
        if previous is None:
            return None
        lab.guest_secret_prefix(info)  # the recorded name is a unique connection
        check = LIVE_KEYRING_STATE.replace('NAME', repr(previous)).replace('ROOT', repr(LIVE_KEYRING_ROOT))
        found = json.loads(lab.guest_session(run, "python3 - <<'PY'\n"+check+'\nPY\n'))
        if found['owned']:
            return {'restarted': False, 'busName': previous, 'reason': 'the live keyring daemon still answers'}
        if found['daemons']:
            raise KeyringRestartRefused('a daemon still serves the live keyring folder without its connection')
        if not found['data']:
            raise KeyringRestartRefused('the live keyring files are missing from the guest')
        owner = "python3 - <<'PY'\n"+OWNER+'\nPY\n'
        prior = json.loads(lab.guest_session(run, owner))
        if prior.get('provider') not in ISOLATED_SECRET_SERVICES:
            raise KeyringRestartRefused("the desktop's own Secret Service is not running")
        identity, name = _start_live_keyring(run, fresh=False)
        try:
            # A new daemon loads the blank collection locked: open it with its empty password, on
            # the new connection, before the run's state names it.
            lab.put(run, str(Path(__file__).with_name('lab_keyring.py')), LIVE_KEYRING_ROOT+'/lab_keyring.py')
            prefix = lab.guest_secret_prefix(dict(info, secret_service=name))
            status = json.loads(lab.guest_session(run, prefix+'python3 '+LIVE_KEYRING_ROOT+'/lab_keyring.py --unlock'))
            if status.get('label') != 'putgd lab' or status.get('locked') is not False:
                raise KeyringRestartRefused('the restarted live keyring is not the unlocked lab collection')
            if json.loads(lab.guest_session(run, owner)) != prior:
                raise KeyringRestartRefused("the desktop's own Secret Service changed during the restart")
            info['secret_service'] = name
            lab.write_json(run/'state.json', info)
        except BaseException:
            # Never leave the new daemon behind: it could hold the desktop's name or the folder.
            _stop_live_keyring(run, identity)
            raise
        return dict(restarted=True, previousBusName=previous, busName=name, pid=identity['pid'],
                    start=identity['start'], collection=status)


# A broken repository stops rpm-ostree from layering anything, and it reads its
# repository list from the image, so no guest configuration can leave one out.
# Fedora's updates-archive has broken this way (#1206) and is not needed for
# these tools. Only then, install them with dnf on a /usr overlay, which lasts
# until the next reboot; the sudo rule does not cover dnf, so this uses the
# standard guest password (docs/lab/remote-golden-images.md).
ATOMIC_TOOLS = '''set -eu
sudo -n systemctl stop uupd.service
sudo -n systemctl disable --now uupd.timer
sudo -n systemctl mask uupd.timer
missing=''
for package in python3-pyqt6 python3-pyatspi python3-dbus python3-pillow python3-gobject tesseract xdotool xprop xwininfo xrandr xclip wl-clipboard gstreamer1-plugins-good-gtk gnome-keyring; do
 if ! rpm -q "$package" >/dev/null; then missing="$missing $package"; fi
done
if [ -n "$missing" ]; then
 status=0
 layered=$(sudo -n rpm-ostree install --apply-live $missing 2>&1) || status=$?
 printf '%s\n' "$layered"
 if [ "$status" -ne 0 ]; then
  case "$layered" in *"rpm-md repo 'updates-archive'"*) ;; *) exit "$status" ;; esac
  sudo -n rpm-ostree usroverlay
  printf '%s\n' putgd-lab | sudo -S -p '' dnf -y --setopt=install_weak_deps=False --disablerepo=updates-archive install $missing
  echo NIGHTLY_TOOLS_DNF_OVERLAY
 fi
 echo NIGHTLY_TOOLS_APPLIED
fi
'''


# Omarchy's golden has no synced package databases, and its mirror moves: a
# sync then -S is a partial upgrade. Only packages that are not installed may
# enter; a transaction that would upgrade, downgrade or remove any installed
# package (Hyprland, quickshell, Qt, Mesa, PipeWire, portals...) is refused.
PACMAN_TOOLS = ('python-atspi', 'python-pyqt6', 'python-pillow', 'gst-plugin-gtk', 'gst-plugins-base')
PACMAN = r'''set -eu
before=$(pacman -Q)
sudo -n pacman -Sy --noconfirm >/dev/null
plan=$(sudo -n pacman -S --needed --print --print-format '%n %v' TOOLS)
printf '%s\n' "$plan" > "$HOME/nightly-pacman-plan.txt"
conflict=''
while read -r name version; do
 [ -n "$name" ] || continue
 installed=$(printf '%s\n' "$before" | awk -v n="$name" '$1==n{print $2}')
 if [ -n "$installed" ] && [ "$installed" != "$version" ]; then conflict="$conflict $name:$installed->$version"; fi
done <<PLAN
$plan
PLAN
if [ -n "$conflict" ]; then echo "NIGHTLY_PACMAN_REFUSED$conflict"; exit 3; fi
[ -z "$plan" ] || sudo -n pacman -S --needed --noconfirm TOOLS
after=$(pacman -Q)
changed=$(printf '%s\n' "$before" | while read -r name version; do
 now=$(printf '%s\n' "$after" | awk -v n="$name" '$1==n{print $2}')
 [ "$now" = "$version" ] || printf '%s %s->%s\n' "$name" "$version" "${now:-removed}"
done)
if [ -n "$changed" ]; then printf 'NIGHTLY_PACMAN_CHANGED\n%s\n' "$changed"; exit 4; fi
printf 'NIGHTLY_PACMAN_OK\n%s\n' "$plan"
'''.replace('TOOLS', ' '.join(PACMAN_TOOLS))

# Probe programs each desktop's cells use. X11 tools only where X11 runs the session.
WAYLAND_TOOLS = {'fedora45-budgie': ('tesseract', 'wl-copy', 'wl-paste', 'grim', 'wlr-randr'),
                 'omarchy-hyprland': ('tesseract', 'wl-copy', 'wl-paste', 'grim')}
X11_TOOLS = ('tesseract', 'xdotool', 'xprop', 'xwininfo', 'xrandr', 'xclip', 'wl-copy', 'wl-paste')

QUIET_OMARCHY = r'''python3 - <<'NIGHTLY_OMARCHY'
import json,os,subprocess
from pathlib import Path
# Omarchy's update and first-run notices are critical quickshell cards that stay on screen.
state=Path.home()/'.local/state/omarchy/notifications/live.json'
cards=[]
try:cards=json.loads(state.read_text())
except (OSError,ValueError):pass
print(json.dumps(dict(overlayOnly=True,liveNotificationsBefore=len(cards) if isinstance(cards,list) else None,
                      stayAwake=(Path.home()/'.local/state/omarchy/indicators/stay-awake').exists())))
NIGHTLY_OMARCHY
'''


def prepare_pacman(run, out):
    """Probe tools for the Omarchy golden: new packages only, nothing installed may move."""
    result = lab.command(lab.ssh_args(run)+['sh', '-s'], check=False, timeout=900, input=PACMAN)
    (out/'pacman-install.txt').write_text(result.stdout+'\n'+result.stderr)
    report = dict(exitCode=result.returncode, tools=list(PACMAN_TOOLS),
                  installedUnchanged=result.returncode == 0 and 'NIGHTLY_PACMAN_OK' in result.stdout,
                  refused=next((line for line in result.stdout.splitlines() if line.startswith('NIGHTLY_PACMAN_REFUSED')), None))
    lab.write_json(out/'pacman-transaction.json', report)
    if not report['installedUnchanged']:
        raise RuntimeError('Omarchy probe tools would change an installed package; private transaction retained')
    return report


def prepare(run, row, out):
    lab.verify_owned_guest(run)
    # AppImages also run on Debian-derived guests. Their probe dependencies
    # still come from apt; selecting the probes never changes the app artifact.
    guest = lab.state(run)['profile']
    package = 'pacman' if guest == 'omarchy-hyprland' else row.get('probePackageManager', row['package'])
    if package == 'pacman':
        prepare_pacman(run, out)
    elif package == 'deb':
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
        preparation = lab.guest_session(run, ATOMIC_TOOLS, timeout=1200)
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
print(json.dumps(dict(modules=modules,tools={name:bool(shutil.which(name)) for name in TOOLS})))
PROBES
'''.replace('TOOLS', repr(WAYLAND_TOOLS.get(guest, X11_TOOLS))))
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
        if row['desktop'] == 'fedora-gnome-wayland':
            # The Shell loads a newly installed extension only when it starts: the reboot below.
            install_tray_host(run, out)
        (out/'witness.txt').write_text(stage(run, Path(__file__).parent/'surface_driver/compositor/gnome-witness'))
        (out/'reboot.json').write_text(json.dumps(reboot(run), indent=2))
        prepare_gnome_desktop(run, out)
        if row['desktop'] == 'fedora-gnome-wayland':
            # The resident starts only after this: its tray must find a registered host.
            try:
                report = lab.guest_session(run, 'export XDG_CONFIG_HOME="$HOME/.config"\n'+TRAY_HOST_READY, timeout=60)
            except lab.LabError as error:
                raise RuntimeError('could not verify that the Fedora AppIndicator extension registered a tray host '
                                   'after the reboot') from error
            (out/'tray-host-ready.json').write_text(report)
    elif 'kde' in row['desktop']:
        from tools.lab.surface_driver.compositor.stage_plasma_accessibility import stage
        (out/'accessibility.txt').write_text(stage(run))
    (out/'nag-suppression.json').write_text(lab.guest_session(run, QUIET, timeout=60))
    if guest == 'omarchy-hyprland':
        (out/'omarchy-state.json').write_text(lab.guest_session(run, QUIET_OMARCHY, timeout=60))
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
