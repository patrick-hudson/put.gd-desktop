"""Run the lab's scenarios against a physical desktop instead of a QEMU guest.

Usage (workstation, repo root):
  python3 tools/lab/physical.py HOSTCFG.json install DEB|RPM
  python3 tools/lab/physical.py HOSTCFG.json SCENARIO   (region, window, desktop, ...: lab.SCENARIOS)
  python3 tools/lab/physical.py HOSTCFG.json script tools/lab/edit_acceptance.py

HOSTCFG.json: name, host, user, identity (workstation key path), known_hosts (pinned), vnc_port (workstation
loopback tunnel), run (private directory whose qmp.sock is the forwarded
tools/lab/host_input.py socket) and profile (name, desktop, session,
qt_platform, session_process, fixture_hotkeys). On a host with a lab lock
directory (a builder), nothing starts unless lock_session (default
$PUTGD_LAB_SESSION) holds its desktop lock.
Optional fixture_bin names a
host directory prepended to PATH, for a lab-built putgd-desktop-fixture when
the installed package was built without PUTGD_LAB_FIXTURE.

The runner's helpers are redirected: shell -> pinned SSH into the real desktop
session's environment, files -> scp, input -> forwarded uinput agent socket
(qmp.sock), screenshots -> the host's loopback VNC tile. Evidence is labelled
graphical-physical. put.gd runs with the runner's isolated XDG directories."""
import argparse, hashlib, json, os, runpy, shlex, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, 'tools/lab'); import lab
sys.path.insert(0, str(Path(__file__).parent)); import rfb

def main():
    cfg = json.loads(Path(sys.argv[1]).read_text())
    run = Path(cfg['run']).resolve(); run.mkdir(parents=True, exist_ok=True)
    from physical_host import PhysicalHost
    target = PhysicalHost(cfg)
    import desktop_lock  # checked through the transport that will do the work, whatever the host is called
    session = cfg.get('lock_session') or os.environ.get(desktop_lock.SESSION_VARIABLE, '')
    problem = json.loads(target.ssh(run, desktop_lock.held_check_script(session)).strip().splitlines()[-1])['problem']
    if problem:
        raise SystemExit(f'physical.py not started: {problem}. Set lock_session to the session that holds it')
    lab.write_json(run / 'state.json', {'schemaVersion': 1, 'id': cfg['name'], 'profile': cfg['profile']['name'],
                                        'guest_user': cfg['user'], 'acceleration': 'physical', 'host': cfg['host']})
    host_ssh, host_put, host_get, host_screenshot = target.ssh, target.put, target.get, target.screenshot

    original_profile = lab.load_profile
    lab.load_profile = lambda name: cfg['profile'] if name == cfg['profile']['name'] else original_profile(name)
    original_readiness = lab.validate_readiness
    def physical_readiness(profile, actual, acceleration):
        result = original_readiness(profile, actual, 'kvm')
        result['evidence_class'] = 'graphical-physical'
        return result
    lab.validate_readiness = physical_readiness
    lab.ssh, lab.put, lab.get, lab.screenshot = host_ssh, host_put, host_get, host_screenshot
    if cfg.get('fixture_bin'):
        original_session = lab.guest_session
        lab.guest_session = lambda _run, body: original_session(
            _run, 'export PATH=' + shlex.quote(cfg['fixture_bin']) + ':"$PATH"\n' + body)

    action = sys.argv[2]
    if action == 'install':
        deb = Path(sys.argv[3]).resolve()
        (run / 'package.json').unlink(missing_ok=True)
        # The copy goes to disk and is removed after the install: the host's /tmp is RAM (#1978).
        # scp resolves the relative path against the host's home.
        staged = 'work/tmp/putgd-lab.rpm' if deb.suffix == '.rpm' else 'work/tmp/putgd-lab.deb'
        host_ssh(run, 'mkdir -p -m 700 ~/work/tmp\n', timeout=60)
        host_put(run, deb, staged)
        if deb.suffix == '.rpm':
            # Remove first: dnf will not reinstall or downgrade a same-version dev build.
            install = ('sudo -n dnf -y -q remove putgd >/dev/null 2>&1 || true\n'
                       'sudo -n dnf -y -q install ~/' + staged + ' >/dev/null\n')
        else:
            install = 'sudo -n apt-get update -qq\nsudo -n apt-get install --reinstall -y -qq ~/' + staged + ' >/dev/null\n'
        host_ssh(run, 'status=0\n{\n' + install + '} || status=$?\nrm -f ~/' + staged + '\nexit "$status"\n', timeout=300)
        installed = lab.verify_installed_binaries(run, deb)
        lab.write_json(run / 'package.json', {'sha256': lab.hash_file(deb), 'filename': deb.name, 'installedBinarySha256': installed})
        print(json.dumps({'status': 'installed', 'host': cfg['host']}))
    elif action in lab.SCENARIOS:
        # VNC witnesses include the pointer. Park it at the right edge so an
        # earlier run's click cannot hide the fixture paint the runner locates.
        lab.pointer(run, 1279, 400, 1280, 800)
        time.sleep(.3)
        args = argparse.Namespace(run=str(run), scenario=action, keep=True, timeout=60, settings=None, history=False, clipboard_client='qt')
        print(json.dumps(lab.fixture_run(args)))
    elif action == 'script':
        sys.argv = [sys.argv[3], str(run)] + sys.argv[4:]
        runpy.run_path(sys.argv[0], run_name='__main__')


if __name__ == "__main__":
    main()
