"""Run the lab's scenarios against a physical desktop instead of a QEMU guest.

Usage (workstation, repo root):
  python3 tools/lab/physical.py HOSTCFG.json install DEB|RPM
  python3 tools/lab/physical.py HOSTCFG.json region
  python3 tools/lab/physical.py HOSTCFG.json script tools/lab/edit_acceptance.py

HOSTCFG.json: name, host, user, identity (workstation key path), known_hosts (pinned), vnc_port (workstation
loopback tunnel), run (private directory whose qmp.sock is the forwarded
tools/lab/host_input.py socket) and profile (name, desktop, session,
qt_platform, session_process, fixture_hotkeys).

The runner's helpers are redirected: shell -> pinned SSH into the real desktop
session's environment, files -> scp, input -> forwarded uinput agent socket
(qmp.sock), screenshots -> the host's loopback VNC tile. Evidence is labelled
graphical-physical. put.gd runs with the runner's isolated XDG directories."""
import argparse, hashlib, json, runpy, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, 'tools/lab'); import lab
sys.path.insert(0, str(Path(__file__).parent)); import rfb

cfg = json.loads(Path(sys.argv[1]).read_text())
run = Path(cfg['run']).resolve(); run.mkdir(parents=True, exist_ok=True)
SSH = ['ssh', '-i', str(Path(cfg['identity']).expanduser()), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
       '-o', 'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=8', '-o', 'UserKnownHostsFile=' + cfg['known_hosts']]
SCP = ['scp', '-q'] + SSH[1:]
lab.write_json(run / 'state.json', {'schemaVersion': 1, 'id': cfg['name'], 'profile': cfg['profile']['name'],
                                    'guest_user': cfg['user'], 'acceleration': 'physical', 'host': cfg['host']})

def host_ssh(_run, script, timeout=60, bootstrap=False):
    return lab.command(SSH + [f"{cfg['user']}@{cfg['host']}", 'sh', '-s'], timeout=timeout, input=script).stdout

def host_put(_run, local, remote):
    lab.command(SCP + [str(local), f"{cfg['user']}@{cfg['host']}:{remote}"])

def host_get(_run, remote, local):
    lab.command(SCP + [f"{cfg['user']}@{cfg['host']}:{remote}", str(local)])

def host_screenshot(_run, name='witness', head=0):
    path = (run / f'{name}-head{head}.ppm').resolve()
    client = rfb.RFB(cfg['vnc_port'])
    try:
        w, h, bgrx = client.frame()
    finally:
        client.close()
    rgb = bytearray(w * h * 3); rgb[0::3] = bgrx[2::4]; rgb[1::3] = bgrx[1::4]; rgb[2::3] = bgrx[0::4]
    path.write_bytes(b'P6\n%d %d\n255\n' % (w, h) + bytes(rgb))
    return {'path': str(path), 'sha256': lab.hash_file(path)}

original_profile = lab.load_profile
lab.load_profile = lambda name: cfg['profile'] if name == cfg['profile']['name'] else original_profile(name)
original_readiness = lab.validate_readiness
def physical_readiness(profile, actual, acceleration):
    result = original_readiness(profile, actual, 'kvm')
    result['evidence_class'] = 'graphical-physical'
    return result
lab.validate_readiness = physical_readiness
lab.ssh, lab.put, lab.get, lab.screenshot = host_ssh, host_put, host_get, host_screenshot

action = sys.argv[2]
if action == 'install':
    deb = Path(sys.argv[3]).resolve()
    (run / 'package.json').unlink(missing_ok=True)
    if deb.suffix == '.rpm':
        # Remove first: dnf will not reinstall or downgrade a same-version dev build.
        host_put(run, deb, '/tmp/putgd-lab.rpm')
        host_ssh(run, 'sudo -n dnf -y -q remove putgd >/dev/null 2>&1 || true\nsudo -n dnf -y -q install /tmp/putgd-lab.rpm >/dev/null\n', timeout=300)
    else:
        host_put(run, deb, '/tmp/putgd-lab.deb')
        host_ssh(run, 'sudo -n apt-get update -qq\nsudo -n apt-get install --reinstall -y -qq /tmp/putgd-lab.deb >/dev/null\n', timeout=300)
    installed = lab.verify_installed_binaries(run, deb)
    lab.write_json(run / 'package.json', {'sha256': lab.hash_file(deb), 'filename': deb.name, 'installedBinarySha256': installed})
    print(json.dumps({'status': 'installed', 'host': cfg['host']}))
elif action == 'region':
    args = argparse.Namespace(run=str(run), scenario='region', keep=True, timeout=60, settings=None, history=False, clipboard_client='qt')
    print(json.dumps(lab.fixture_run(args)))
elif action == 'script':
    sys.argv = [sys.argv[3], str(run)] + sys.argv[4:]
    runpy.run_path(sys.argv[0], run_name='__main__')
