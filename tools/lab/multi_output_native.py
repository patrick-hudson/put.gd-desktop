#!/usr/bin/env python3
"""Run multi_output.py on a locked builder with a private AppImage or installed native package.

  multi_output_native.py --session LEASE --run NEW_SHORT_DIR --appimage FILE
                         --sha256 SHA --vnc-port 5902 [--case NAME ...]

Arrange/restore the native outputs outside this driver. Keep the original VNC
server; --vnc-port names an already-visible combined stream covering every head.
The driver refuses an existing resident and verifies fresh motion on every head
before capture. It starts/stops only its private resident, fixture and uinput
unit. No package install, login, boot change or display change is performed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace
import uuid

import headless_output
import lab
import multi_output
import record_run
import rfb
import wall_probe


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--session', required=True); p.add_argument('--run', type=Path, required=True)
    package_choice = p.add_mutually_exclusive_group(required=True)
    package_choice.add_argument('--appimage', type=Path)
    package_choice.add_argument('--native-package', type=Path, help='Already installed exact DEB/RPM; verified against its archive')
    p.add_argument('--sha256', required=True)
    p.add_argument('--vnc-port', type=int, required=True)
    p.add_argument('--pointer-route', choices=('portal-vnc', 'uinput'), default='portal-vnc')
    p.add_argument('--case', action='append', choices=multi_output.CASES)
    p.add_argument('--change-command', type=Path)
    a = p.parse_args(); headless_output.lease(a.session)
    run = a.run.resolve()
    paths = [run / 'qmp.sock', run / 'runtime/putgd/session.sock']
    if any(len(str(s).encode()) > 107 for s in paths):
        raise RuntimeError('Resolved control socket exceeds 107 UTF-8 bytes')
    artifact = a.appimage or a.native_package
    if digest(artifact) != a.sha256:
        raise RuntimeError('Frozen package checksum mismatch')
    if subprocess.run(['pgrep', '-u', str(os.getuid()), '-x', 'putgd'], capture_output=True).returncode == 0:
        raise RuntimeError('An existing resident must be preserved; choose a clean authorized slot')
    run.mkdir(mode=0o700, exist_ok=False)
    for name in ('runtime', 'config', 'data', 'cache', 'state', 'bin'):
        (run / name).mkdir(mode=0o700)
    multi_output.save(run / 'socket-preflight.json', {'paths': [{'path': str(s), 'utf8Bytes': len(str(s).encode())} for s in paths]})
    env, desktop = record_run.session_environment()
    session_env = dict(env)
    env.update(XDG_CONFIG_HOME=str(run / 'config'), XDG_DATA_HOME=str(run / 'data'),
               XDG_CACHE_HOME=str(run / 'cache'), XDG_STATE_HOME=str(run / 'state'),
               XDG_RUNTIME_DIR=str(run / 'runtime'), PIPEWIRE_RUNTIME_DIR=session_env['XDG_RUNTIME_DIR'])
    # AppRun's KWin permission desktop entries must be visible to the compositor.
    applications = Path.home() / '.local/share/applications'
    applications.mkdir(parents=True, exist_ok=True)
    before_entries = {str(f): f.read_bytes() for f in applications.glob('*putgd*.desktop')}
    (run / 'data/applications').symlink_to(applications)
    if a.appimage:
        build = record_run.Build('frozen', a.appimage, run)
        appdir = Path(build.cli).parents[2]
        cli_env = dict(env, APPDIR=str(appdir), LD_LIBRARY_PATH=str(appdir / 'usr/lib') + ':' + str(appdir / 'usr/lib/putgd'),
                       QT_PLUGIN_PATH=str(appdir / 'usr/plugins'))
        binaries = {'/usr/bin/' + f.name: digest(f) for f in (appdir / 'usr/bin').glob('putgd*') if f.is_file() and os.access(f, os.X_OK)}
        package = {'format': 'appimage', 'analysisRoot': str(appdir)}
        version_command = [str(a.appimage.resolve()), '--version-json']
    else:
        build = SimpleNamespace(launcher=['/usr/bin/putgd-launcher', '--background'], cli='/usr/bin/putgd-cli')
        cli_env = dict(env)
        # Reuse the package-versus-running-files verifier on the actual builder.
        lab.ssh = lambda _run, body, **_kw: subprocess.run(['sh', '-s'], input=body, env=env, text=True, capture_output=True, check=True).stdout
        binaries = lab.verify_installed_binaries(run, a.native_package)
        package = {'format': 'rpm' if a.native_package.suffix == '.rpm' else 'deb'}
        version_command = ['/usr/bin/putgd', '--version-json']
    profile = {'name': 'native-multi-output', 'desktop': env['XDG_CURRENT_DESKTOP'], 'session': env['XDG_SESSION_TYPE'],
               'qt_platform': env['QT_QPA_PLATFORM'], 'session_process': desktop}
    if desktop == 'plasmashell':
        profile['desktop'] = 'KDE'
    profile['guest_user'] = os.environ['USER']
    profile['nativeAdapter'] = {'sha256': digest(__file__), 'pointerRoute': a.pointer_route,
                                'fixtureSha256': digest(Path(__file__).with_name('multi_output_fixture.py'))}
    package.update(sha256=a.sha256, filename=artifact.name, installedBinarySha256=binaries,
                   version=json.loads(subprocess.check_output(version_command, env=env)))
    multi_output.save(run / 'package.json', package)
    multi_output.save(run / 'state.json', {'id': str(uuid.uuid4()), 'profile': profile['name'], 'guest_user': profile['guest_user'],
                                         'acceleration': 'physical', 'host': os.uname().nodename,
                                         'visuals': 'combined VNC stream, every head independently freshness-gated'})
    unit = 'headless-input-' + uuid.uuid4().hex[:12]
    fixture_path = Path('/tmp/putgd-multi-output-fixture.py')
    if fixture_path.exists() or fixture_path.is_symlink():
        raise RuntimeError('Temporary fixture path already exists; preserve it')
    resident = None; wall_fixture = None; campaign = None; input_started = False; input_stopped = False
    control = None
    def local_shell(_run, body, timeout=60, bootstrap=False):
        return subprocess.run(['sh', '-s'], input=body, env=env, capture_output=True, text=True, check=True, timeout=timeout).stdout
    def local_get(_run, source, target):
        source = Path(source).expanduser()
        if not source.is_absolute():
            source = Path.home() / source
        shutil.copyfile(source, target)
    def local_put(_run, source, target):
        target = Path(target)
        if target.resolve() != Path(source).resolve():
            shutil.copyfile(source, target)
    def cli(_run, method, params=None):
        result = subprocess.run([build.cli, '--socket', str(paths[1]), method, json.dumps(params or {})], env=cli_env,
                                capture_output=True, text=True, timeout=70)
        reply = json.loads(result.stdout)
        if not reply.get('ok'):
            raise RuntimeError('Production CLI ' + method + ': ' + reply.get('error', {}).get('code', 'no-code'))
        return reply['result']
    def shot(_run, name='witness', head=0):
        client = rfb.RFB(a.vnc_port)
        try:
            client.frame(); time.sleep(.1)
            w, h, bgrx = client.frame()
        finally:
            client.close()
        rgb = bytearray(w * h * 3); rgb[0::3] = bgrx[2::4]; rgb[1::3] = bgrx[1::4]; rgb[2::3] = bgrx[0::4]
        path = run / (name + '-combined.ppm')
        path.write_bytes(b'P6\n%d %d\n255\n' % (w, h) + rgb)
        return {'path': str(path), 'sha256': digest(path)}
    lab.ssh = local_shell; lab.guest_session = local_shell; lab.get = local_get; lab.put = local_put
    lab.cli = cli; lab.screenshot = shot; lab.load_profile = lambda _name: profile
    try:
        # Probe coverage/freshness before starting the app or keeping any host pixels.
        wall_dir = run / 'wall-fixture'
        wall_fixture = subprocess.Popen([sys.executable, str(Path(__file__).with_name('multi_output_fixture.py')),
            '--output', str(wall_dir), '--motion'], env=env, stdout=(run / 'wall-fixture.log').open('w'), stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 12
        while not (wall_dir / 'ready.json').exists():
            if wall_fixture.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('Moving native wall fixture did not become ready')
            time.sleep(.2)
        layout = json.loads((wall_dir / 'ready.json').read_text())['monitors']
        for m in layout:
            m['name'] = 'GDK head ' + str(m['index'])
        client = rfb.RFB(a.vnc_port)
        try:
            client.frame(); time.sleep(.25); first = client.frame(); time.sleep(.5); second = client.frame()
            checked = wall_probe.check_frames(first, second, layout)
            multi_output.save(run / 'wall-freshness.json', checked)
            if not checked['ok']:
                raise RuntimeError('Every head must be fresh on the wall; renew pilot-owned w0 stream when extent is stale')
            client.screenshot(run / 'wall-all-heads.png')
        finally:
            client.close()
        wall_fixture.terminate(); wall_fixture.wait(5); wall_fixture = None
        subprocess.run(['sudo', '-n', 'systemd-run', '--unit=' + unit, '--collect', '/usr/bin/python3',
            str(Path(__file__).with_name('host_input.py')), 'serve', '--socket', str(paths[0]), '--user', os.environ['USER']], check=True, capture_output=True)
        input_started = True
        resident = subprocess.Popen(build.launcher, env=env, stdin=subprocess.DEVNULL,
                                    stdout=(run / 'resident.log').open('w'), stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 30
        while True:
            try:
                cli(run, 'capabilities'); break
            except (ValueError, RuntimeError):
                if time.monotonic() > deadline:
                    raise
                time.sleep(.2)
        # KWin can map an absolute uinput tablet to one output. The portal VNC
        # stream's absolute pointer is explicitly in its verified combined
        # logical coordinate space; keep one connection so a drag retains buttons.
        control = rfb.RFB(a.vnc_port) if a.pointer_route == 'portal-vnc' else None
        def native_pointer(_run, x, y, width, height, down=None):
            if (width, height) != (control.width, control.height):
                raise RuntimeError('Native pointer viewport differs from the verified combined wall stream')
            control.pointer(x, y, down)
        original_input = lab.input_events
        def native_input(_run, events, head=None):
            if all(e['type'] == 'btn' for e in events):
                for event in events:
                    bit = {'left': 1, 'middle': 2, 'right': 4}[event['data']['button']]
                    control.buttons = control.buttons | bit if event['data']['down'] else control.buttons & ~bit
                    control.pointer(40, 40)
            else:
                original_input(_run, events, head)
        if control:
            lab.pointer = native_pointer; lab.input_events = native_input
        options = SimpleNamespace(run=str(run), output=str(run / ('g' + uuid.uuid4().hex[:6])), appimage=None,
                                  change_command=a.change_command)
        campaign = multi_output.Campaign(options)
        campaign.setup()
        for name in a.case or multi_output.CASES:
            headless_output.lease(a.session)
            campaign.recording() if name == 'record-region' else campaign.one(name)
    finally:
        cleanup_errors = []
        if campaign:
            try:
                campaign.cleanup()
            except Exception as error:
                cleanup_errors.append('campaign: ' + str(error))
        if resident:
            try:
                cli(run, 'ui.quit')
                resident.wait(10)
            except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
                if resident.poll() is None:
                    try:
                        resident.terminate(); resident.wait(10)
                    except subprocess.SubprocessError:
                        cleanup_errors.append('resident: ' + str(error))
        if wall_fixture and wall_fixture.poll() is None:
            try:
                wall_fixture.terminate(); wall_fixture.wait(5)
            except subprocess.SubprocessError as error:
                cleanup_errors.append('wall fixture: ' + str(error))
        if input_started:
            try:
                subprocess.run(['sudo', '-n', 'systemctl', 'stop', unit], check=True)
                input_stopped = True
            except subprocess.SubprocessError as error:
                cleanup_errors.append('input: ' + str(error))
        if control:
            control.close()
        if fixture_path.is_file() and digest(fixture_path) == digest(Path(__file__).with_name('multi_output_fixture.py')):
            fixture_path.unlink()
        current = {str(f): f.read_bytes() for f in applications.glob('*putgd*.desktop')}
        # AppRun owns its generated entries; preserve baseline entries byte-for-byte.
        remaining = [name for name in current if name not in before_entries]
        baseline_same = all(Path(name).exists() and Path(name).read_bytes() == data for name, data in before_entries.items())
        multi_output.save(run / 'cleanup.json', {'inputUnit': unit, 'inputStarted': input_started, 'inputStopped': input_stopped,
            'residentExited': resident is None or resident.poll() is not None, 'baselineDesktopEntriesPreserved': baseline_same,
            'newDesktopEntriesRemaining': remaining, 'temporaryFixtureRemoved': not fixture_path.exists(), 'errors': cleanup_errors})
        if remaining or not baseline_same or cleanup_errors:
            raise RuntimeError('Desktop entry cleanup differs from baseline; inspect before handoff')
    return 0 if campaign and campaign.results and all(r['ok'] for r in campaign.results) else 1


if __name__ == '__main__':
    sys.exit(main())
