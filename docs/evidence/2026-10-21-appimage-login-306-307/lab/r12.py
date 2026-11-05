#!/usr/bin/env python3
"""Guest driver for the #306/#307 AppImage checks (Mint 22.3 Cinnamon guest, user lab).

  r12.py start IMAGE        start IMAGE --background in the running Cinnamon session
  r12.py enable             local-only settings with launch at login on (resident must run)
  r12.py state [LABEL]      print the autostart entry, the login script's values, the images
                            and the running resident's APPIMAGE and version, as JSON
  r12.py quit               ui.quit the running resident and wait for it to exit
  r12.py summary LABEL      the same facts as short lines, for an on-screen terminal

Uses the guest's real XDG folders: autostart must be read by the real login.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

HOME = Path.home()
AUTOSTART = HOME / '.config/autostart/putgd.desktop'
SCRIPT = HOME / '.local/share/putgd/login/appimage-start.sh'
FOLDERS = [HOME / 'Applications', HOME / 'AppImages', HOME / 'Apps']


def environ(pid):
    data = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
    return dict(item.decode(errors='replace').split('=', 1) for item in data if b'=' in item)


def pids(name):
    result = subprocess.run(['pgrep', '-u', str(os.getuid()), '-x', name], capture_output=True, text=True)
    return [int(pid) for pid in result.stdout.split()]


def session():
    cinnamon = pids('cinnamon')
    if not cinnamon:
        raise SystemExit('no Cinnamon session')
    source = environ(cinnamon[0])
    env = dict(os.environ)
    for key in ('DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_RUNTIME_DIR', 'XDG_CURRENT_DESKTOP',
                'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'XDG_DATA_DIRS', 'XDG_CONFIG_DIRS'):
        if key in source:
            env[key] = source[key]
    env['XDG_SESSION_TYPE'] = 'x11'
    return env


def resident():
    for pid in pids('putgd'):
        env = environ(pid)
        if env.get('APPDIR'):
            return pid, env
    return None, None


def cli(method, params=None):
    pid, env = resident()
    if not pid:
        raise SystemExit('no put.gd resident')
    result = subprocess.run([env['APPDIR'] + '/usr/bin/putgd-cli', method, json.dumps(params or {})],
                            env=session(), capture_output=True, text=True, timeout=30)
    return json.loads(result.stdout)


def wait_ready(seconds=40):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if cli('diagnostics')['result']['journalReady']:
                return True
        except (SystemExit, Exception):
            pass
        time.sleep(0.5)
    raise SystemExit('resident not ready')


def main():
    command = sys.argv[1]
    if command == 'start':
        image = sys.argv[2]
        log = open(HOME / ('r12-start-' + time.strftime('%H%M%S') + '.log'), 'wb')
        subprocess.Popen([image, '--background'], env=session(), stdout=log, stderr=log,
                         stdin=subprocess.DEVNULL, start_new_session=True)
        wait_ready()
        print(json.dumps(state('started'), indent=1))
    elif command == 'enable':
        document = cli('settings.developmentTemplate')['result']['document']
        document['profiles'] = []
        document['actions'] = {a: {'destination': 'local', 'profileId': None, 'shortcut': ''} for a in document['actions']}
        (HOME / 'r12-archive').mkdir(exist_ok=True)
        document['archive']['root'] = str(HOME / 'r12-archive')
        document['preferences'] = {'notifications': False, 'launchAtLogin': True}
        revision = cli('settings.get')['result']['metadata']['observedRevision']
        print(json.dumps(cli('settings.patch', {'patch': document, 'expectedRevision': revision})['ok']))
        deadline = time.monotonic() + 10
        while not AUTOSTART.exists() and time.monotonic() < deadline:
            time.sleep(0.2)
        print(json.dumps(state('enabled'), indent=1))
    elif command == 'state':
        print(json.dumps(state(sys.argv[2] if len(sys.argv) > 2 else ''), indent=1))
    elif command == 'summary':
        s = state(sys.argv[2] if len(sys.argv) > 2 else '')
        r = s['resident']
        print('== ' + s['label'] + '  ' + s['time'])
        print('login session:  ' + ' | '.join(' '.join(line.split()) for line in s['sessions']))
        print('put.gd running: ' + (f"pid {r['pid']}, started {r['started']}" if r['pid'] else 'NO'))
        print('  version:      ' + str(r['version']))
        print('  APPIMAGE:     ' + str(r['APPIMAGE']))
        print('autostart:      ' + str(s['autostartExec']))
        print('login script:   configured=' + s['loginScript'].get('configured', '-'))
        print('                update_information=' + s['loginScript'].get('update_information', '-'))
        for folder, names in s['images'].items():
            print(folder + ':')
            for name in names:
                print('  ' + name)
    elif command == 'quit':
        pid, _ = resident()
        if pid:
            cli('ui.quit')
            deadline = time.monotonic() + 20
            while Path(f'/proc/{pid}').exists() and time.monotonic() < deadline:
                time.sleep(0.2)
        print(json.dumps({'quit': pid, 'running': resident()[0]}))
    else:
        raise SystemExit(__doc__)


def state(label):
    pid, env = resident()
    version = None
    if pid:
        try:
            version = cli('diagnostics')['result'].get('build', {}).get('version')
        except Exception:
            version = None
        if not version:
            exe = env['APPDIR'] + '/usr/bin/putgd'
            version = subprocess.run([exe, '--version'], capture_output=True, text=True).stdout.strip()
    script = {}
    if SCRIPT.exists():
        for line in SCRIPT.read_text().splitlines():
            if line.startswith(('configured=', 'update_information=')):
                key, value = line.split('=', 1)
                script[key] = value
        script['mode'] = oct(SCRIPT.stat().st_mode & 0o777)
    exec_line = next((line for line in AUTOSTART.read_text().splitlines() if line.startswith('Exec=')), None) \
        if AUTOSTART.exists() else None
    images = {str(folder): sorted(path.name for path in folder.iterdir() if 'utgd' in path.name.lower() or path.suffix.lower() == '.appimage')
              for folder in FOLDERS if folder.is_dir()}
    login = subprocess.run(['loginctl', 'list-sessions', '--no-legend'], capture_output=True, text=True).stdout.strip()
    started = None
    if pid:
        started = subprocess.run(['ps', '-o', 'lstart=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()
    return {'label': label, 'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'sessions': login.splitlines(),
            'resident': {'pid': pid, 'started': started, 'APPIMAGE': env.get('APPIMAGE') if env else None,
                         'version': version},
            'autostartExec': exec_line, 'loginScript': script, 'images': images}


if __name__ == '__main__':
    main()
