#!/usr/bin/env python3
"""#490 lab driver: a resident ends with its login session. Runs on a builder (stdlib only).

  r490.py state               residents, their sessions, the graphical session, loginSession diagnostics
  r490.py configure ARCHIVE   Launch at login on; the desktop action saves only to ARCHIVE (no upload)
  r490.py capture             one desktop capture through the resident; reports the operation, not pixels
  r490.py lock | unlock       the desktop's screen locker
  r490.py logout              ends the graphical session through its session manager
  r490.py relogin             restarts the display manager (autologin)
  r490.py wait-resident OLD   waits for a resident other than PID OLD in the current graphical session
  r490.py log PID             the resident log records of PID that name the session
  r490.py rss PID SECONDS     idle RSS/PSS of PID after SECONDS, with the host load
  r490.py frame OUT.png       a full-desktop X11 frame of the session's display (xwd -root)
Only exact PIDs are ever signalled; this script signals none.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

UID = os.getuid()
SHELLS = ('cinnamon-session-binary', 'cinnamon', 'gnome-shell', 'plasmashell')
VARIABLES = ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'DBUS_SESSION_BUS_ADDRESS', 'XDG_RUNTIME_DIR', 'XDG_SESSION_ID',
             'XDG_CURRENT_DESKTOP', 'XDG_SESSION_TYPE', 'XDG_SESSION_DESKTOP', 'DESKTOP_SESSION', 'APPIMAGE')
CLI = os.environ.get('R490_CLI', '/usr/bin/putgd-cli')


def sh(*args, env=None, timeout=60):
    r = subprocess.run(args, capture_output=True, text=True, env=env, timeout=timeout)
    return r.returncode, r.stdout.strip(), r.stderr.strip()


def environ(pid):
    try:
        return dict(p.split('=', 1) for p in Path(f'/proc/{pid}/environ').read_bytes().decode(errors='replace').split('\0') if '=' in p)
    except OSError:
        return {}


def session_of(pid):
    try:
        m = re.search(r'/session-([A-Za-z0-9]+)\.scope', Path(f'/proc/{pid}/cgroup').read_text())
        return m.group(1) if m else None
    except OSError:
        return None


def graphical_session():
    """The newest active graphical session of this user, with its leader's environment."""
    for line in sh('loginctl', 'list-sessions', '--no-legend')[1].splitlines():
        sid = line.split()[0]
        props = dict(l.split('=', 1) for l in sh('loginctl', 'show-session', sid, '-p', 'Type', '-p', 'State', '-p', 'User', '-p', 'Leader',
                                                 '-p', 'Display', '-p', 'LockedHint', '-p', 'Active')[1].splitlines() if '=' in l)
        if props.get('User') == str(UID) and props.get('Type') in ('x11', 'wayland') and props.get('State') == 'active':
            return sid, props
    return None, {}


def session_env():
    for name in SHELLS:
        pids = sh('pgrep', '-u', str(UID), '-x', name)[1].split()
        for pid in pids:
            found = environ(pid)
            # gnome-shell's own environment has no WAYLAND_DISPLAY; the user manager's has it.
            if found.get('XDG_SESSION_TYPE') in ('x11', 'wayland') and (found.get('DISPLAY') or found.get('WAYLAND_DISPLAY') or name == 'gnome-shell'):
                env = {'HOME': os.environ['HOME'], 'PATH': '/usr/bin:/bin', 'USER': os.environ.get('USER', ''), 'LANG': found.get('LANG', 'C.UTF-8')}
                env.update({k: found[k] for k in VARIABLES if found.get(k)})
                env.setdefault('XDG_RUNTIME_DIR', f'/run/user/{UID}')
                if name == 'gnome-shell':
                    shown = dict(l.split('=', 1) for l in sh('systemctl', '--user', 'show-environment')[1].splitlines() if '=' in l)
                    for k in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY'):
                        if k in shown: env.setdefault(k, shown[k])
                return env, name, int(pid)
    raise SystemExit('no graphical session process')


def residents():
    out = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit(): continue
        try:
            if entry.stat().st_uid != UID: continue
            exe = os.readlink(entry / 'exe')
        except OSError:
            continue
        base = os.path.basename(exe).removesuffix(' (deleted)')
        if base not in ('putgd', 'putgd-ui'): continue
        stat = (entry / 'stat').read_text()
        fields = stat[stat.rindex(')') + 2:].split()
        sid = session_of(entry.name)
        state = dict(l.split('=', 1) for l in sh('loginctl', 'show-session', sid, '-p', 'State', '-p', 'Type')[1].splitlines() if '=' in l) if sid else {}
        cg = Path(f'/proc/{entry.name}/cgroup').read_text().strip().split('::')[-1]
        out.append({'pid': int(entry.name), 'exe': exe, 'ppid': int(fields[1]), 'start': int(fields[19]), 'session': sid,
                    'sessionState': state.get('State'), 'sessionType': state.get('Type'), 'cgroup': cg,
                    'display': environ(entry.name).get('DISPLAY'), 'sessionIdEnv': environ(entry.name).get('XDG_SESSION_ID')})
    return sorted(out, key=lambda p: p['pid'])


def cli(method, params=None, timeout=60):
    env, _, _ = session_env()
    r = subprocess.run([CLI, method, json.dumps(params or {})], env=env, capture_output=True, text=True, timeout=timeout)
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {'ok': False, 'raw': r.stdout[-400:], 'stderr': r.stderr[-400:], 'exit': r.returncode}


def iso():
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())


def state():
    sid, props = graphical_session()
    snap = cli('ui.snapshot', timeout=20)
    diag = (snap.get('result') or {}).get('diagnostics') or {}
    return {'at': iso(), 'graphicalSession': sid, 'session': props, 'residents': residents(),
            'snapshot': {'ok': snap.get('ok'), 'error': (snap.get('error') or {}).get('code'), 'build': (snap.get('result') or {}).get('build'),
                         'loginSession': diag.get('loginSession'), 'loginStartup': diag.get('loginStartup'),
                         'shortcutsAvailable': (diag.get('shortcuts') or {}).get('available')}}


def main():
    action = sys.argv[1]
    if action == 'state':
        result = state()
    elif action == 'configure':
        archive = sys.argv[2]
        snap = cli('settings.get')
        patch = {'preferences': {'launchAtLogin': True}, 'archive': {'keepUploads': True, 'root': archive},
                 'actions': {'desktop': {'destination': 'local'}}}
        result = cli('settings.patch', {'patch': patch, 'expectedRevision': snap['result']['metadata']['observedRevision']})
        time.sleep(1.5)
        entry = Path.home() / '.config/autostart/putgd.desktop'
        result = {'patch': result.get('ok'), 'error': result.get('error'), 'entry': entry.read_text() if entry.exists() else None}
    elif action == 'capture':
        r = cli('action.invoke', {'action': 'desktop'})
        op = None
        if r.get('ok'):
            oid = r['result']['operationId']
            for _ in range(120):
                op = cli('operation.inspect', {'id': oid}).get('result')
                if op and op.get('state') in ('completed', 'failed', 'cancelled', 'partial', 'interrupted', 'stored'): break
                time.sleep(0.5)
        def item(i):
            local = i.get('local') or {}
            path, size = local.get('path') or '', None
            try:
                head = Path(path).read_bytes()[:24] if path else b''
                if head[:8] == b'\x89PNG\r\n\x1a\n': size = [int.from_bytes(head[16:20], 'big'), int.from_bytes(head[20:24], 'big')]
            except OSError:
                pass
            return {'mime': (i.get('artifact') or {}).get('mime'), 'bytes': (i.get('artifact') or {}).get('bytes'), 'local': local.get('status'),
                    'file': os.path.basename(path), 'pixels': size, 'error': (i.get('error') or {}).get('code')}
        result = {'at': iso(), 'invoke': {'ok': r.get('ok'), 'error': r.get('error')}, 'operationId': (op or {}).get('id'),
                  'state': (op or {}).get('state'), 'error': ((op or {}).get('error') or {}).get('code'),
                  'items': [item(i) for i in (op or {}).get('items') or []]}
    elif action in ('lock', 'unlock'):
        env, shell, _ = session_env()
        if shell.startswith('cinnamon'):
            result = sh('cinnamon-screensaver-command', '--lock' if action == 'lock' else '--deactivate', env=env)
        elif shell == 'plasmashell':
            result = sh('loginctl', 'lock-session' if action == 'lock' else 'unlock-session', graphical_session()[0])
        else:
            result = sh('loginctl', 'lock-session' if action == 'lock' else 'unlock-session', graphical_session()[0])
        time.sleep(3)
        result = {'at': iso(), 'command': result, 'locked': graphical_session()[1].get('LockedHint')}
    elif action == 'logout':
        env, shell, _ = session_env()
        sid = graphical_session()[0]
        if shell.startswith('cinnamon'): cmd = ['cinnamon-session-quit', '--logout', '--no-prompt']
        elif shell == 'gnome-shell': cmd = ['gnome-session-quit', '--logout', '--no-prompt']
        else: cmd = ['qdbus6', 'org.kde.Shutdown', '/Shutdown', 'org.kde.Shutdown.logout']
        subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        result = {'at': iso(), 'session': sid, 'command': cmd}
    elif action == 'relogin':
        dm = next(d for d in ('lightdm', 'gdm', 'sddm') if sh('systemctl', 'is-active', d)[1] == 'active')
        result = {'at': iso(), 'dm': dm, 'restart': sh('sudo', '-n', 'systemctl', 'restart', dm)}
    elif action == 'wait-resident':
        old = int(sys.argv[2])
        deadline = time.time() + float(sys.argv[3] if len(sys.argv) > 3 else 120)
        result = None
        while time.time() < deadline:
            sid, props = graphical_session()
            fresh = [p for p in residents() if p['pid'] != old and os.path.basename(p['exe']).startswith('putgd') and not p['exe'].endswith('putgd-ui')
                     and (p['session'] == sid or p['session'] is None)]
            if sid and fresh and cli('ui.snapshot', timeout=10).get('ok'):
                result = {'at': iso(), 'graphicalSession': sid, 'fresh': fresh}
                break
            time.sleep(2)
        result = result or {'at': iso(), 'timeout': True, 'residents': residents()}
    elif action == 'log':
        pid = sys.argv[2]
        root = Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')) / 'putgd/logs'
        lines = []
        for f in sorted(root.glob('putgd-*.jsonl')):
            for line in f.read_text(errors='replace').splitlines():
                if f'"pid":{pid}' in line.replace(' ', '') and re.search(r'session|Resident st|Replaced|X11 connection', line):
                    lines.append(json.loads(line))
        result = lines
    elif action == 'frame':
        import struct
        from PIL import Image
        env, _, _ = session_env()
        data = subprocess.run(['xwd', '-root', '-silent'], env=env, capture_output=True, timeout=30, check=True).stdout
        header = struct.unpack('>25I', data[:100])
        size, width, height, bpp, line = header[0], header[4], header[5], header[11], header[12]
        ncolors = header[19]
        if bpp not in (24, 32): raise SystemExit(f'unsupported xwd depth {bpp}')
        start = size + ncolors * 12
        lsb = header[7] == 0
        mode, raw = ('RGBX', 'BGRX' if lsb else 'XRGB') if bpp == 32 else ('RGB', 'BGR' if lsb else 'RGB')
        image = Image.frombuffer(mode, (width, height), data[start:start + line * height], 'raw', raw, line, 1)
        image.convert('RGB').save(sys.argv[2])
        result = {'at': iso(), 'frame': sys.argv[2], 'size': [width, height]}
    elif action == 'rss':
        pid, wait = int(sys.argv[2]), float(sys.argv[3])
        time.sleep(wait)
        status = dict(l.split(':', 1) for l in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in l)
        rollup = dict(l.split(':', 1) for l in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:] if ':' in l)
        kib = lambda v: int(v.split()[0])
        result = {'at': iso(), 'pid': pid, 'exe': os.readlink(f'/proc/{pid}/exe'), 'idleSeconds': wait,
                  'rssMiB': round(kib(status['VmRSS']) / 1024, 2), 'pssMiB': round(kib(rollup['Pss']) / 1024, 2),
                  'threads': int(status['Threads']), 'load': Path('/proc/loadavg').read_text().split()[:3],
                  'libsystemdMapped': 'libsystemd' in Path(f'/proc/{pid}/maps').read_text()}
    else:
        raise SystemExit(__doc__)
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
