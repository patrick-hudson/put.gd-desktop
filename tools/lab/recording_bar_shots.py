#!/usr/bin/env python3
"""Frames of a real desktop showing the recording bar's microphone states and Settings (#1364).

  recording_bar_shots.py --run DIR --build LABEL=PATH [--rect X,Y,W,H] [--vnc HOST:PORT]
                         [--states off,on,none,settings,support] [--hold 2.5]

Starts the build's resident with a private profile (as record_run.py does) and,
for each state, puts put.gd's own surface on screen and saves one frame of the
desktop, taken through the desktop's VNC tile, as DIR/shots/STATE.png:

  off       a region recording is ready; a simulated microphone is connected and
            the bar's Microphone toggle is off
  on        the same with the toggle on and a tone playing into the simulated
            microphone, so the meter beside the toggle shows a level
  none      the simulated microphone is gone: the toggle reads "No microphone".
            Where the host's default input is a real device, the default is made
            an output's monitor for this state (and put back): the real device
            is never opened
  settings  Settings is open; where it scrolls is up to the caller (the frame is
            taken after --settings-wait seconds, during which the caller may
            drive the window through the same VNC tile)
  support   Settings is open with Advanced → Desktop support details expanded
            (`ui.diagnostics` setDesktopDetails, the way an agent opens it): the
            Click highlights row says what this desktop does and what it leaves out

A real microphone is never opened: the simulated one comes from virtual_mic.py
and is removed by its module ids when the run ends, also after a failure.
Each state's `recording.inspect` and `recording.microphones` answers are kept
in DIR/shots.json beside the frames, with the build's identity. Crop the frames
to put.gd's surface before publishing them.

PATH is an AppImage or a directory holding `putgd` and `putgd-cli` (an installed
package: /usr/bin). --vnc is the VNC server that shows this desktop: the host's
loopback tile (default 127.0.0.1:5900), or in a lab guest QEMU's on the VM host
(10.0.2.2:59NN).
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import types

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lab_resident  # noqa: E402
import record_run  # noqa: E402
import rfb  # noqa: E402


def main():
    """Starts a private resident, shows each asked state of the bar (or Settings) and saves a frame of the desktop for it."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run', required=True)
    parser.add_argument('--build', required=True, metavar='LABEL=PATH')
    parser.add_argument('--rect', default='480,300,640,360', help='the region to prepare, in logical desktop pixels')
    parser.add_argument('--vnc', default='127.0.0.1:5900', metavar='HOST:PORT')
    parser.add_argument('--states', default='off,on,none')
    parser.add_argument('--hold', type=float, default=2.5, help='seconds the bar is on screen before its frame is taken')
    parser.add_argument('--settings-wait', type=float, default=8)
    parser.add_argument('--click-highlights', action='store_true', help='prepare with click highlights, so the bar lists them')
    a = parser.parse_args()
    run = Path(a.run).resolve()
    for name in ('config', 'data', 'cache', 'state', 'shots', 'logs'):
        (run / name).mkdir(parents=True, exist_ok=True)
    os.chmod(run, 0o700)
    env, desktop = record_run.session_environment()
    env.update(XDG_CONFIG_HOME=str(run / 'config'), XDG_DATA_HOME=str(run / 'data'), XDG_CACHE_HOME=str(run / 'cache'),
               XDG_STATE_HOME=str(run / 'state'))
    build = record_run.Build(*a.build.split('=', 1), run)
    socket = Path(env['XDG_RUNTIME_DIR']) / 'putgd' / 'session.sock'
    host, _, port = a.vnc.rpartition(':')
    mic_state = run / 'virtual-mic.json'
    none_state = run / 'no-microphone.json'
    tone = None
    report = {'host': os.uname().nodename, 'desktop': desktop, 'session': env.get('XDG_SESSION_TYPE'), 'build': build.identity, 'states': {}}

    driver = types.SimpleNamespace(env=env)  # lab_resident.cli reads only the driver's environment

    def cli(method, params=None, timeout=60):
        return lab_resident.cli(driver, build, method, params, timeout)

    def virtual_mic(*arguments):
        result = subprocess.run([sys.executable, str(HERE / 'virtual_mic.py'), *arguments], capture_output=True, text=True, env=env)
        try:
            return json.loads(result.stdout)
        except ValueError:
            raise record_run.RunError('virtual_mic %s: %s' % (arguments[0], (result.stdout + result.stderr).strip()[-300:]))

    def frame(name):
        client = rfb.RFB(int(port), host=host or '127.0.0.1')
        try:
            # A tile that sends only damage needs one full update first.
            client.frame()
            time.sleep(0.3)
            client.screenshot(str(run / 'shots' / (name + '.png')))
        finally:
            client.close()

    def stop_tone():
        # The tone is a gst-launch under virtual_mic.py: the whole group goes, and before the simulated microphone
        # does, or the sound server would move the orphaned tone to the real default output.
        nonlocal tone
        if tone and tone.poll() is None:
            os.killpg(tone.pid, signal.SIGTERM)
            tone.wait(10)
        tone = None

    def wait_ready(rid):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            info = cli('recording.inspect', {'id': rid})
            if info.get('state') == 'ready':
                return info
            if info.get('state') in ('failed', 'cancelled'):
                raise record_run.RunError('prepare ended %s: %s' % (info.get('state'), json.dumps(info.get('error'))))
            time.sleep(0.3)
        raise record_run.RunError('the recording did not become ready')

    if socket.exists():
        raise SystemExit('%s exists: another put.gd resident owns this session' % socket)

    def start():
        process = subprocess.Popen(build.launcher, env=env, stdin=subprocess.DEVNULL, stdout=open(run / 'logs' / 'resident.log', 'a'),
                                   stderr=subprocess.STDOUT, start_new_session=True)
        deadline = time.monotonic() + 60
        while True:
            try:
                return process, cli('recording.capabilities', timeout=40)
            except (record_run.RunError, subprocess.TimeoutExpired):
                if time.monotonic() > deadline:
                    process.terminate()
                    raise
                time.sleep(0.5)

    def quit_resident(process):
        try:
            cli('ui.quit', timeout=15)
            process.wait(20)
        except Exception:
            process.terminate()
        deadline = time.monotonic() + 10
        while socket.exists() and time.monotonic() < deadline:
            time.sleep(0.2)

    # A resident's first start with a fresh configuration opens Settings by itself: let that happen, give the
    # configuration its revision, and take the pictures from a second start, which opens nothing.
    def keep_local():
        # A fresh configuration has no output; local copies stay inside the run.
        settings = cli('settings.get')
        if not ((settings.get('document') or {}).get('archive') or {}).get('keepUploads'):
            cli('settings.patch', {'patch': {'archive': {'keepUploads': True, 'root': str(run / 'archive')}, 'preferences': {'notifications': False}},
                                   'expectedRevision': (settings.get('metadata') or {}).get('observedRevision')})

    resident, _ = start()
    try:
        keep_local()
        time.sleep(1.0)
    finally:
        quit_resident(resident)
    resident, capabilities = start()
    report['capabilities'] = {k: v for k, v in capabilities.items() if k in ('audio', 'clickHighlights', 'acquisition', 'targets')}
    try:
        keep_local()
        rect = [int(v) for v in a.rect.split(',')]
        for state in a.states.split(','):
            entry = {}
            if state in ('off', 'on') and not mic_state.exists():
                virtual_mic('create', '--name', 'putgd_lab_%d' % os.getpid(), '--state', str(mic_state))
            if state == 'none':
                if mic_state.exists():
                    stop_tone()
                    virtual_mic('remove', '--state', str(mic_state))
                    mic_state.unlink(missing_ok=True)
                default = subprocess.run(['pactl', 'get-default-source'], env=env, capture_output=True, text=True).stdout.strip()
                if default and not default.endswith('.monitor') and default != '@DEFAULT_SOURCE@':
                    # This host's default input is a real device. For the picture the default becomes an output's
                    # monitor, the state of a desktop with nothing plugged in; the real device is never opened.
                    virtual_mic('create', '--name', 'putgd_lab_none_%d' % os.getpid(), '--state', str(none_state), '--system-sink', '--monitor-default')
            if state in ('settings', 'support'):
                cli('ui.show', {'view': 'settings'})
                if state == 'support':
                    time.sleep(1.5)
                    entry['diagnostics'] = cli('ui.diagnostics', {'action': 'setDesktopDetails', 'enabled': True})
                time.sleep(a.settings_wait)
                frame(state)
                report['states'][state] = dict(entry, shown=state)
                continue
            device = json.loads(mic_state.read_text())['source'] if mic_state.exists() and state != 'none' else None
            if state == 'on' and tone is None:
                # A steady tone into the simulated microphone: the meter then shows a level in a still frame.
                tone = subprocess.Popen([sys.executable, str(HERE / 'virtual_mic.py'), 'tone', '--state', str(mic_state), '--to', 'mic', '--hz', '440',
                                         '--volume', '0.25', '--seconds', '120'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                        start_new_session=True)
                time.sleep(1.0)
            entry['microphones'] = cli('recording.microphones', {'device': device} if device else {})
            audio = {'system': False, 'microphone': state == 'on'}
            if device:
                audio['microphoneDevice'] = device
            request = {'target': {'kind': 'region', 'rect': rect}, 'format': 'mp4', 'fps': 30, 'audio': audio}
            if a.click_highlights:
                request['clickHighlights'] = True
            rid = cli('recording.prepare', request)['recordingId']
            try:
                info = wait_ready(rid)
                time.sleep(a.hold)
                frame(state)
                info = cli('recording.inspect', {'id': rid})
                entry['inspect'] = {k: info.get(k) for k in ('state', 'config', 'overlayPlacement', 'overlayExcluded', 'progress')}
            finally:
                cli('recording.cancel', {'id': rid})
                time.sleep(0.8)
            report['states'][state] = entry
    finally:
        stop_tone()
        for created in (none_state, mic_state):
            if created.exists():
                try:
                    virtual_mic('remove', '--state', str(created))
                except record_run.RunError as error:
                    report['microphoneCleanup'] = str(error)
        quit_resident(resident)
        (run / 'shots.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'shots': sorted(p.name for p in (run / 'shots').glob('*.png')), 'report': str(run / 'shots.json')}))


if __name__ == '__main__':
    main()
