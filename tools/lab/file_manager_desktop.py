#!/usr/bin/env python3
"""One-off file-manager acceptance on desktops the lab keeps no golden for (#1105).

Boots a throwaway overlay of an existing golden, installs a distro desktop into it,
and leaves a real session for a real right-click. Nothing but receipts and frames
outlives `down`. Run on the builder, one desktop per builder.

  file_manager_desktop.py up xfce --run NAME --golden G --key K --known-hosts H
  file_manager_desktop.py package NAME FILE
  file_manager_desktop.py s3 NAME
  file_manager_desktop.py open NAME [--folder DIR] [--no-file-manager]
  file_manager_desktop.py shot NAME LABEL
  file_manager_desktop.py click NAME X Y [--right]
  file_manager_desktop.py verify NAME
  file_manager_desktop.py sh NAME < script
  file_manager_desktop.py down NAME
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import socket
import sys
import threading
import time
import urllib.request
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab import lab, proton_lab
from tools.lab import provision_live_credentials as bridge
from tools.lab.desktop_lock import Desktop
from tools.lab.surface_driver.fixtures.live_outcomes import key_set_two

ROOT = Path(__file__).resolve().parents[2]
UBUNTU_GREETER = 'lightdm lightdm-gtk-greeter'
DESKTOPS = {
    'xfce': dict(profile='xfce-x11', packages='xfce4 thunar xfce4-notifyd', session='xfce'),
    'mate': dict(profile='mate-x11', packages='mate-desktop-environment-core caja mate-notification-daemon mate-applets', session='mate'),
    'lxqt': dict(profile='lxqt-x11', packages='lxqt-core lxqt-panel lxqt-notificationd lxqt-session openbox pcmanfm-qt', session='lxqt'),
    'lxde': dict(profile='lxde-x11', packages='lxde-core pcmanfm notification-daemon', session='LXDE'),
    'budgie': dict(profile='budgie-wayland', packages='budgie-desktop nemo labwc', session='budgie-desktop', gdm=True),
    # The session aborts on a missing GSettings schema from smaller sets; the metapackage is 673 MB.
    'ukui': dict(profile='ukui-x11', packages='ukui-desktop-environment', session='ukui'),
    # Ubuntu 26.04 ships no Pantheon at all (no gala, pantheon-files, wingpanel or contractor), so
    # `up` refuses. A row needs elementary OS 8.1 unpacked to a disposable disk, then these commands.
    # gala's own environment has no WAYLAND_DISPLAY or DISPLAY; wingpanel's (comm truncated) does.
    'pantheon': dict(profile='pantheon-wayland', session='pantheon-wayland', env_process='io.elementary.w',
                     unavailable='Ubuntu 26.04 has no gala, pantheon-files, wingpanel or contractor package'),
    # cosmic-session requires the whole desktop (comp, panel, files, notifications); start-cosmic
    # never puts WAYLAND_DISPLAY in cosmic-session's own environment, so read a child's.
    'cosmic': dict(profile='cosmic-wayland', packages='cosmic-session wl-clipboard firefox gnome-keyring', session='cosmic',
                   dnf=True, env_process='cosmic-panel'),
    # Ubuntu 24.04's own desktop, Nautilus and DING are already in the golden.
    'gnome': dict(profile='gnome-x11', preinstalled=True),
}
FILE_MANAGERS = dict(xfce='thunar', mate='caja', lxqt='pcmanfm-qt', lxde='pcmanfm', budgie='nemo', ukui='peony',
                     pantheon='io.elementary.files', cosmic='cosmic-files', gnome='nautilus')


def run_path(name):
    return ROOT/'work/lab-runs'/name


def record(name, **fields):
    path = run_path(name)/'fm.json'
    value = json.loads(path.read_text()) if path.exists() else {}
    value.update(fields)
    path.write_text(json.dumps(value, indent=2)+'\n')
    return value


def session_shell(name, body, timeout=120):
    """The real session's environment and real config: no private XDG directories."""
    run = run_path(name)
    process = lab.load_profile(lab.state(run)['profile'])['session_process']
    process = DESKTOPS[json.loads((run/'fm.json').read_text())['desktop']].get('env_process', process)
    return lab.ssh(run, f'''set -eu
pid=$(pgrep -u "$(id -u)" -x {shlex.quote(process)} | head -n 1)
[ -n "$pid" ]
for key in DISPLAY XAUTHORITY DBUS_SESSION_BUS_ADDRESS XDG_SESSION_TYPE XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP WAYLAND_DISPLAY XDG_DATA_DIRS XDG_CONFIG_DIRS; do
 value=$(tr '\\0' '\\n' < /proc/$pid/environ | sed -n "s/^$key=//p")
 [ -z "$value" ] || export "$key=$value"
done
export XDG_RUNTIME_DIR=/run/user/$(id -u)
{body}
''', timeout=timeout)


# Wayland-only sessions cannot autologin through LightDM; the golden's GDM stays.
GDM_LOGIN = '''sudo -n systemctl disable lightdm 2>/dev/null || true
sudo -n systemctl enable --force gdm3
echo /usr/sbin/gdm3 | sudo -n tee /etc/X11/default-display-manager >/dev/null
printf '[daemon]\\nAutomaticLoginEnable=True\\nAutomaticLogin=lab\\nWaylandEnable=true\\n' | sudo -n tee /etc/gdm3/custom.conf >/dev/null
sudo -n mkdir -p /var/lib/AccountsService/users
printf '[User]\\nSession={session}\\nSystemAccount=false\\n' | sudo -n tee /var/lib/AccountsService/users/lab >/dev/null
'''


# Config of a golden's old put.gd, removed with its package.
OLD_STATE = '''for dir in "$HOME/.config/putgd" "$HOME/.local/share/putgd" "$HOME/.local/state/putgd" "$HOME/.cache/putgd"; do
 [ ! -d "$dir" ] || {{ find "$dir" ! -type d -delete; find "$dir" -depth -type d -empty -delete; }}
done
'''


def install_script(desktop):
    spec = DESKTOPS[desktop]
    if spec.get('dnf'):
        # Fedora goldens grant dnf alone. A local package's scriptlet is the same
        # overlay-only use of that authority as the apt hook below.
        return f'''set -eu
sudo -n dnf install -y --setopt=install_weak_deps=False rpm-build >/dev/null
mkdir -p "$HOME/fm-grant"
cat > "$HOME/fm-grant/grant.spec" <<'SPEC'
Name: zz-fm-grant
Version: 1
Release: 1
Summary: Disposable overlay sudo grant
License: None
BuildArch: noarch
%description
Disposable overlay sudo grant.
%post
printf 'lab ALL=(ALL) NOPASSWD: ALL\\n' > /etc/sudoers.d/zz-fm
chmod 440 /etc/sudoers.d/zz-fm
%files
SPEC
rpmbuild -bb --define "_rpmdir $HOME/fm-grant" "$HOME/fm-grant/grant.spec" >/dev/null 2>&1
sudo -n dnf install -y "$HOME/fm-grant/noarch/zz-fm-grant-1-1.noarch.rpm" >/dev/null
sudo -n dnf remove -y --setopt=clean_requirements_on_remove=False putgd >/dev/null 2>&1 || true
'''+OLD_STATE.format()+f'''sudo -n dnf install -y --setopt=install_weak_deps=False {spec['packages']} >/dev/null
sudo -n mkdir -p /etc/gdm /var/lib/AccountsService/users
printf '[User]\\nSession={spec['session']}\\nSystemAccount=false\\n' | sudo -n tee /var/lib/AccountsService/users/lab >/dev/null
printf '[daemon]\\nAutomaticLoginEnable=True\\nAutomaticLogin=lab\\n' | sudo -n tee /etc/gdm/custom.conf >/dev/null
# COSMIC's first-login wizard would cover the desktop under test.
mkdir -p "$HOME/.config/autostart"
printf '[Desktop Entry]\\nHidden=true\\n' > "$HOME/.config/autostart/cosmic-initial-setup.desktop"
touch "$HOME/.config/gnome-initial-setup-done"
'''
    # Goldens grant apt-get alone; this overlay is thrown away (see lab.stage_install_helper).
    grant = 'printf "lab ALL=(ALL) NOPASSWD: ALL\\n" > /etc/sudoers.d/zz-fm; chmod 440 /etc/sudoers.d/zz-fm'
    prepare = f'''set -eu
export DEBIAN_FRONTEND=noninteractive
sudo -n apt-get -o 'APT::Update::Pre-Invoke::={grant}' update -qq
# The golden's preinstalled put.gd must not run, or leave state, before the build under test.
sudo -n -E apt-get -o DPkg::Lock::Timeout=600 purge -y putgd >/dev/null 2>&1 || true
'''+OLD_STATE.format()+'''sudo -n systemctl disable --now apt-daily.timer apt-daily-upgrade.timer unattended-upgrades.service 2>/dev/null || true
sudo -n systemctl stop apt-daily.service apt-daily-upgrade.service 2>/dev/null || true
'''
    if spec.get('preinstalled'):
        return prepare+'''sudo -n -E apt-get -o DPkg::Lock::Timeout=600 install -y --no-install-recommends xclip >/dev/null
'''
    return prepare+f'''echo lightdm shared/default-x-display-manager select lightdm | sudo -n debconf-set-selections
sudo -n -E apt-get -o DPkg::Lock::Timeout=600 -o Dpkg::Options::=--force-confnew install -y --no-install-recommends {UBUNTU_GREETER} xserver-xorg xclip dbus-x11 gnome-keyring firefox {spec['packages']} >/dev/null
sudo -n mkdir -p /etc/lightdm/lightdm.conf.d
printf '[Seat:*]\\nautologin-user=lab\\nautologin-session={spec['session']}\\n' | sudo -n tee /etc/lightdm/lightdm.conf.d/50-lab.conf >/dev/null
sudo -n systemctl disable gdm3 2>/dev/null || true
echo /usr/sbin/lightdm | sudo -n tee /etc/X11/default-display-manager >/dev/null
sudo -n systemctl enable --force lightdm
'''+(GDM_LOGIN.format(session=spec['session']) if spec.get('gdm') else '')


def up(args):
    name, spec = args.run, DESKTOPS[args.desktop]
    if spec.get('unavailable'):
        raise SystemExit(args.desktop+': '+spec['unavailable'])
    lock = Desktop(Path.home()/'putgd-lab/locks', args.host)
    if args.join:
        # A second guest on a builder with room shares the first guest's lease.
        session = json.loads((run_path(args.join)/'fm.json').read_text())['session']
        lock.extend(session, 45)
    else:
        session = 'fm-'+uuid.uuid4().hex[:10]
        lock.acquire('i1105-remaining', session, '#1105 '+args.desktop+' disposable guest', 45)
    stop = threading.Event()
    def beat():
        while not stop.wait(240):
            lock.extend(session, 45)
    threading.Thread(target=beat, daemon=True).start()
    run = run_path(name)
    known = Path(os.environ.get('TMPDIR', '/tmp'))/(name+'-known')
    pins = Path(args.known_hosts).read_text().splitlines()
    key = next(' '.join(p.split()[1:3]) for p in pins if ' ssh-ed25519 ' in p)
    known.write_text(f'[127.0.0.1]:{args.ssh_port} {key}\n')
    lab.command([sys.executable, ROOT/'tools/lab/lab.py', 'launch', '--profile', args.base_profile, '--golden', args.golden,
                 '--run', run, '--bootstrap-key', args.key, '--known-hosts', known, '--ssh-port', args.ssh_port,
                 '--vnc-display', args.vnc, '--timeout', 420], timeout=600)
    record(name, desktop=args.desktop, host=args.host, session=session, imagesRoot=str(Path(args.golden).resolve().parents[1]))
    started = time.monotonic()
    lab.ssh(run, install_script(args.desktop), timeout=1800)
    record(name, installSeconds=round(time.monotonic()-started))
    lab.ssh(run, 'sudo -n sync; (sleep 1; sudo -n systemctl reboot) >/dev/null 2>&1 &', timeout=30)
    time.sleep(20)
    info = lab.state(run)
    info['profile'] = spec['profile']
    lab.write_json(run/'state.json', info)
    process = lab.load_profile(spec['profile'])['session_process']
    deadline = time.monotonic()+420
    while time.monotonic() < deadline:
        try:
            if lab.ssh(run, f'pgrep -x {shlex.quote(process)} || true', timeout=20).strip():
                break
        except lab.LabError:
            pass
        time.sleep(5)
    else:
        raise SystemExit('session process '+process+' never started; frame is in the run')
    print(json.dumps(record(name, overlayBytes=(run/'disk.qcow2').stat().st_blocks*512)))


BUILD_STATE = r'''
if command -v dpkg-query >/dev/null; then dpkg-query -W -f='installed ${Version}\n' putgd 2>/dev/null || true; else rpm -q putgd || true; fi
for pid in $(pgrep -u "$(id -u)" '^putgd' || true); do
 echo "running $pid $(readlink /proc/$pid/exe) $(tr '\0' ' ' < /proc/$pid/cmdline | cut -c1-70)"
done
'''


def build_state(name):
    return lab.ssh(run_path(name), BUILD_STATE, timeout=30).strip()


def shell(args):
    print(session_shell(args.run, sys.stdin.read(), timeout=args.timeout))


def status(args):
    print(build_state(args.run))


def require_build(name):
    """A golden's preinstalled put.gd must never pass for the build under test."""
    state = build_state(name)
    value = json.loads((run_path(name)/'fm.json').read_text())
    version = value.get('package', '').split('putgd-')[-1].split('-nightly')[0]
    if value.get('appimage'):
        # Purged at `up`, so no distro package can answer for the image.
        if any(line.startswith('installed') for line in state.splitlines()) or (
                'running' in state and '.mount_' not in state):
            raise SystemExit('a distro put.gd is present beside the AppImage: '+state)
        return state
    if not version or not state or version not in state.splitlines()[0]:
        raise SystemExit('installed build is not the one under test: '+state)
    if '(deleted)' in state:
        raise SystemExit('a resident from before the install is still running: '+state)
    return state


def package(args):
    run, path = run_path(args.run), Path(args.file)
    lab.ssh(run, 'for pid in $(pgrep -u "$(id -u)" "^putgd" || true); do kill "$pid" || true; done; sleep 3')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    home = lab.ssh(run, 'printf %s "$HOME"')
    target = home+'/'+path.name
    lab.put(run, str(path), target)
    if path.suffix == '.deb':
        lab.ssh(run, f'sudo -n DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y {shlex.quote(target)} >/dev/null', timeout=900)
    elif path.suffix == '.rpm':
        lab.ssh(run, f'sudo -n dnf install -y {shlex.quote(target)} >/dev/null', timeout=900)
    else:
        # The extracted tree only supplies putgd-cli for checks; the app runs from the image.
        lab.ssh(run, f'''set -eu
chmod +x {shlex.quote(target)}
if command -v apt-get >/dev/null; then
 sudo -n DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y --no-install-recommends fuse3 >/dev/null 2>&1 || true
else
 # qtkeychain-qt6 is for the lab's secret bridge (the RPM path gets it as a dependency); the image brings its own.
 sudo -n dnf install -y --setopt=install_weak_deps=False fuse3 fuse-libs qtkeychain-qt6 >/dev/null 2>&1 || true
fi
mkdir -p "$HOME/fm-cli"
cd "$HOME/fm-cli"
{shlex.quote(target)} --appimage-extract >/dev/null
''', timeout=300)
        record(args.run, appimage=target)
    print(json.dumps(record(args.run, package=path.name, packageSha256=digest)))


def cli_command(name):
    value = json.loads((run_path(name)/'fm.json').read_text())
    if value['package'].endswith('.AppImage'):
        root = '"$HOME/fm-cli/squashfs-root"'
        return f'env LD_LIBRARY_PATH={root}/usr/lib {root}/usr/bin/putgd-cli'
    return 'putgd-cli'


def start_command(name):
    value = json.loads((run_path(name)/'fm.json').read_text())
    return shlex.quote(value['appimage'])+' --background' if value.get('appimage') else 'putgd --background'


def call(name, method, params=None):
    reply = session_shell(name, f'{cli_command(name)} {shlex.quote(method)} {shlex.quote(json.dumps(params or {}))}')
    result = json.loads(reply)
    if not result.get('ok'):
        raise SystemExit(method+': '+result.get('error', {}).get('code', 'unknown'))
    return result['result']


def s3(args):
    """Same key set and bridge as the live nightly cell; credentials never leave memory."""
    name, run = args.run, run_path(args.run)
    require_build(name)
    guest = proton_lab.Guest(run)
    broker = guest.place_brokers(Path.home()/'putgd-lab/proton/guest-bridge')['qt']
    guest.prepare_keyring()
    selected = key_set_two(bridge.read_private_credentials(Path.home()/'Documents/creds.txt'))
    reference = 'putgd-lab-'+str(uuid.uuid4())
    bridge.guest_call(str(run), broker, 's3', dict(op='import', reference=reference, credentials=selected['credentials']))
    settings = selected['settings']
    prefix = f'acceptance/i1105/{value_host(name)}/{uuid.uuid4().hex}/'
    provider = json.loads((ROOT/'config/development.example.json').read_text())['profiles'][0]['settings']
    region = settings['region']
    provider.update(bucket=settings['bucket'], region=region, endpoint='https://s3.'+region+'.amazonaws.com',
                    objectPrefix=prefix, publicUrlBase='https://put.gd', aclMode='public-read', addressing='auto')
    profile = dict(id='i1105', label='i1105 live S3', provider='s3', settings=provider,
                   credentials=dict(mode='keychain', reference=reference))
    session_shell(name, f'nohup {start_command(name)} >/dev/null 2>&1 &\nsleep 8')
    old = call(name, 'settings.get')
    call(name, 'settings.patch', {'patch': {'profiles': [profile]}, 'expectedRevision': old['metadata']['acceptedRevision']})
    call(name, 'ui.quit')
    record(name, prefix=prefix, reference=reference)
    print(json.dumps(dict(s3='configured', prefix=prefix)))


def value_host(name):
    return json.loads((run_path(name)/'fm.json').read_text())['host']


def open_files(args):
    stamp = uuid.uuid4().hex[:8]
    files = f'i1105-{stamp}.png'
    folder = args.folder
    session_shell(args.run, f'''mkdir -p "$HOME/i1105" "$HOME/{folder}"
python3 -c "import zlib,struct,sys,os
w,h=64,64
raw=b''.join(b'\\\\0'+os.urandom(3)*w for _ in range(h))
c=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
open(os.path.expanduser('~/{folder}/{files}'),'wb').write(b'\\\\x89PNG\\\\r\\\\n\\\\x1a\\\\n'+c(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+c(b'IDAT',zlib.compress(raw))+c(b'IEND',b''))"
: > "$HOME/i1105/.notify.log"
# Bash (Fedora's sh) keeps the list's subshell, and with it ssh's stdout, open unless the whole list is redirected.
{{ pgrep -u "$(id -u)" -x dbus-monitor >/dev/null || nohup dbus-monitor "interface='org.freedesktop.Notifications',member='Notify'" >> "$HOME/i1105/.notify.log" 2>&1; }} </dev/null >/dev/null 2>&1 &
''')
    fm = FILE_MANAGERS[json.loads((run_path(args.run)/'fm.json').read_text())['desktop']]
    if not args.no_file_manager:
        session_shell(args.run, f'nohup {fm} "$HOME/{folder}" </dev/null >/dev/null 2>&1 &\nsleep 4')
    print(json.dumps(record(args.run, file=files, folder=folder, fileManager=fm)))


def shot(args):
    from PIL import Image
    out = run_path(args.run)/('shot-'+args.label+'.png')
    path = lab.screenshot(run_path(args.run), 'fmshot')['path']
    Image.open(path).save(out)
    Path(path).unlink()
    print(out)


def click(args):
    run = run_path(args.run)
    width, height = lab.QMP_SIZE if hasattr(lab, 'QMP_SIZE') else (args.width, args.height)
    button = 'right' if args.right else 'left'
    move = [dict(type='abs', data=dict(axis='x', value=int(args.x*32767/width))),
            dict(type='abs', data=dict(axis='y', value=int(args.y*32767/height)))]
    with lab.QMP(run/'qmp.sock') as qmp:
        qmp.execute('input-send-event', {'events': move})
        time.sleep(.4)
        for down in (True, False):
            qmp.execute('input-send-event', {'events': [dict(type='btn', data=dict(button=button, down=down))]})
            time.sleep(.1)
    print(json.dumps(dict(clicked=[args.x, args.y], button=button)))


def verify(args):
    name = args.run
    value = json.loads((run_path(name)/'fm.json').read_text())
    build = require_build(name)
    deadline = time.monotonic()+90
    row = None
    while time.monotonic() < deadline and not row:
        try:
            rows = call(name, 'history.list', {'limit': 20}).get('records', [])
        except (SystemExit, lab.LabError, ValueError):
            rows = []
        row = next((r for r in rows if r.get('name') == value['file'] and (r.get('receipt') or {}).get('directUrl')), None)
        time.sleep(3)
    if not row:
        raise SystemExit('no finished history row for '+value['file'])
    link = row['receipt']['directUrl']
    path = f'"$HOME/{value.get("folder", "i1105")}/{value["file"]}"'
    local = hashlib.sha256(bytes.fromhex(lab.ssh(run_path(name), f'od -An -tx1 -v {path} | tr -d " \\n"').strip())).hexdigest()
    remote = hashlib.sha256(urllib.request.urlopen(link, timeout=30).read()).hexdigest()
    clip = session_shell(name, 'if [ "${XDG_SESSION_TYPE:-}" = wayland ]; then wl-paste -n 2>/dev/null || true; else xclip -selection clipboard -o 2>/dev/null || true; fi').strip()
    notify = lab.ssh(run_path(name), 'cat "$HOME/i1105/.notify.log"')
    result = dict(build=build, file=value['file'], historyId=row['id'], hashMatch=local == remote, clipboardIsLink=clip == link,
                  notified='Upload complete' in notify, notifyCalls=notify.count('member=Notify'),
                  notifySenders=sorted({line.split('sender=')[1].split()[0] for line in notify.splitlines() if 'member=Notify' in line}),
                  resident=lab.ssh(run_path(name), 'pgrep -c "^putgd" || true').strip())
    session_shell(name, f'nohup firefox {shlex.quote(link)} >/dev/null 2>&1 &\nsleep 12')
    record(name, link=link, historyId=row['id'], verify=result, verifications=value.get('verifications', [])+[result])
    print(json.dumps(result))
    if not (result['hashMatch'] and result['clipboardIsLink'] and result['notified']):
        raise SystemExit('verify failed')


def retire(run):
    """Refuses while any process still names this run, so the lock never outlives a guest."""
    marker = str(run/'disk.qcow2').encode()
    for entry in Path('/proc').iterdir():
        if entry.name.isdecimal():
            try:
                if marker in (entry/'cmdline').read_bytes():
                    raise SystemExit('guest process '+entry.name+' still owns '+str(run))
            except OSError:
                pass
    for path in run.iterdir():
        path.unlink()
    run.rmdir()


def down(args):
    name, run = args.run, run_path(args.run)
    value = json.loads((run/'fm.json').read_text())
    evidence = Path.home()/'fm-evidence'/name
    evidence.mkdir(parents=True, exist_ok=True)
    if value.get('historyId'):
        # Every upload this guest made, not only the last one verified.
        try:
            session_shell(name, f'pgrep -u "$(id -u)" "^putgd" >/dev/null || {{ nohup {start_command(name)} >/dev/null 2>&1 & sleep 8; }}')
            ids = [r['id'] for r in call(name, 'history.list', {'limit': 100}).get('records', []) if (r.get('receipt') or {}).get('directUrl')]
            prepared = call(name, 'delete.prepare', {'ids': ids})
            started = call(name, 'delete.confirm', {'ids': ids, 'acknowledgement': prepared['acknowledgement']})
            # The reply comes before the deletes run: stopping the guest now would leave the objects behind.
            deadline = time.monotonic()+120
            deletion = started['deletion']
            while deletion.get('state') not in ('completed', 'cancelled') and time.monotonic() < deadline:
                time.sleep(1)
                deletion = call(name, 'delete.inspect', {'id': started['deleteOperationId']})
            statuses = [item.get('status') for item in deletion.get('items', [])]
            print(json.dumps(dict(deleted=len(ids), state=deletion.get('state'), statuses=sorted(set(statuses)))), flush=True)
        except (SystemExit, lab.LabError, ValueError, KeyError) as error:
            print(json.dumps(dict(deleteFailed=str(error))), flush=True)
    for path in [run/'fm.json', *run.glob('shot-*.png')]:
        (evidence/path.name).write_bytes(path.read_bytes())
    print(json.dumps(dict(evidence=str(evidence), overlayBytes=(run/'disk.qcow2').stat().st_blocks*512)), flush=True)
    if (run/'qmp.sock').exists():
        try:
            with lab.QMP(run/'qmp.sock') as qmp:
                qmp.execute('quit')
        except (OSError, lab.LabError):
            pass
        time.sleep(4)
    retire(run)
    others = [p for p in run.parent.glob('*/fm.json') if json.loads(p.read_text()).get('session') == value['session']]
    if not others:
        Desktop(Path.home()/'putgd-lab/locks', value['host']).release(value['session'], verified_clean=True)
    print('down')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='action', required=True)
    p = commands.add_parser('up')
    p.add_argument('desktop', choices=DESKTOPS)
    for option in ('--run', '--golden', '--key', '--known-hosts', '--host'):
        p.add_argument(option, required=True)
    p.add_argument('--base-profile', default='gnome-wayland')
    p.add_argument('--ssh-port', default='22456')
    p.add_argument('--vnc', default='56')
    p.add_argument('--join', help='share this running guest\'s desktop lease')
    p.set_defaults(function=up)
    p = commands.add_parser('package'); p.add_argument('run'); p.add_argument('file'); p.set_defaults(function=package)
    p = commands.add_parser('sh'); p.add_argument('run'); p.add_argument('--timeout', type=int, default=300); p.set_defaults(function=shell)
    p = commands.add_parser('status'); p.add_argument('run'); p.set_defaults(function=status)
    p = commands.add_parser('s3'); p.add_argument('run'); p.set_defaults(function=s3)
    p = commands.add_parser('open'); p.add_argument('run'); p.add_argument('--folder', default='i1105')
    p.add_argument('--no-file-manager', action='store_true', help='make the file only (a file for the desktop icons)')
    p.set_defaults(function=open_files)
    p = commands.add_parser('shot'); p.add_argument('run'); p.add_argument('label'); p.set_defaults(function=shot)
    p = commands.add_parser('click'); p.add_argument('run'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p.add_argument('--right', action='store_true'); p.add_argument('--width', type=int, default=1280); p.add_argument('--height', type=int, default=800)
    p.set_defaults(function=click)
    p = commands.add_parser('verify'); p.add_argument('run'); p.set_defaults(function=verify)
    p = commands.add_parser('down'); p.add_argument('run'); p.set_defaults(function=down)
    args = parser.parse_args()
    if args.action != 'up':
        value = json.loads((run_path(args.run)/'fm.json').read_text())
        Desktop(Path.home()/'putgd-lab/locks', value['host']).extend(value['session'], 45)
    args.function(args)


if __name__ == '__main__':
    main()
