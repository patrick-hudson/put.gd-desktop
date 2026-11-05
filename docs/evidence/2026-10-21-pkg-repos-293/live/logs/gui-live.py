#!/usr/bin/env python3
"""On-screen #293 phase 2 checks on one physical desktop (runs on the host).

  gui293.py start DESKTOP PORT    magenta cover, then the installed resident with a private
                                  profile and the lab feed; runs update.check, shows Settings
  gui293.py cli METHOD [JSON]     putgd-cli against that resident
  gui293.py frame NAME            VNC frame -> crop of everything that isn't cover or panel,
                                  kept only when the crop's edges are cover colour
  gui293.py click X Y             pointer click through the VNC server (screen coordinates)
  gui293.py stop                  quit the resident and the cover, remove the private profile
"""
import json, os, re, signal, struct, subprocess, sys, time, zlib
from pathlib import Path

LAB = Path.home() / 'work/put.gd-pkg293-live'
STATE = LAB / 'gui-state.json'
EVIDENCE = LAB / 'evidence'
sys.path.insert(0, str(LAB))
SHELLS = {'cinnamon': 'cinnamon', 'plasma': 'plasmashell', 'gnome': 'gnome-shell'}


def session_env(desktop):
    pid = subprocess.check_output(['pgrep', '-u', str(os.getuid()), '-x', SHELLS[desktop]], text=True).split()[0]
    found = dict(i.decode().split('=', 1) for i in Path(f'/proc/{pid}/environ').read_bytes().split(b'\0') if b'=' in i)
    exported = dict(l.split('=', 1) for l in subprocess.check_output(['systemctl', '--user', 'show-environment'], text=True).splitlines() if '=' in l)
    for key in ('WAYLAND_DISPLAY', 'DISPLAY', 'XAUTHORITY', 'XDG_SESSION_TYPE', 'XDG_CURRENT_DESKTOP', 'DBUS_SESSION_BUS_ADDRESS'):
        if key in exported:
            found.setdefault(key, exported[key])
    # The whole session environment (KDE_SESSION_VERSION, XDG_DATA_DIRS, ...), as a real launch has it.
    env = {k: v for k, v in found.items() if k not in ('PWD', 'OLDPWD', 'SHLVL', '_')}
    env.setdefault('XDG_SESSION_TYPE', 'wayland' if 'WAYLAND_DISPLAY' in env else 'x11')
    return env


def environment(state):
    env = {k: v for k, v in os.environ.items() if k not in ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_SESSION_TYPE', 'QT_QPA_PLATFORM')}
    env.update(state['env'])
    return env


def cli(state, method, params=None, timeout=80):
    out = subprocess.run(['/usr/bin/putgd-cli', method, json.dumps(params or {})], env=environment(state), capture_output=True,
                         text=True, timeout=timeout).stdout
    return json.loads(out)


def png(path, w, h, rgb_rows):
    raw = b''.join(b'\0' + row for row in rgb_rows)
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def magenta(b, g, r):
    return r > 230 and b > 230 and g < 40


def main():
    command = sys.argv[1]
    if command == 'start':
        desktop, port = sys.argv[2], int(sys.argv[3])
        EVIDENCE.mkdir(parents=True, exist_ok=True)
        profile = Path(subprocess.check_output(['mktemp', '-d', '/tmp/pg293.XXXX'], text=True).strip())
        env = session_env(desktop)
        runtime = profile / 'run'; runtime.mkdir(mode=0o700)
        if 'WAYLAND_DISPLAY' in env and not env['WAYLAND_DISPLAY'].startswith('/'):
            env['WAYLAND_DISPLAY'] = env.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}') + '/' + env['WAYLAND_DISPLAY']
        session = dict(env)
        for key, name in (('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'), ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')):
            (profile / name).mkdir(mode=0o700); env[key] = str(profile / name)
        env['XDG_RUNTIME_DIR'] = str(runtime)
        if env['XDG_SESSION_TYPE'] == 'x11':
            env['QT_QPA_PLATFORM'] = 'xcb'
        state = {'desktop': desktop, 'port': port, 'profile': str(profile), 'env': env}
        # The cover closes at end of input, so a sleeper holds its stdin (a FIFO) open.
        fifo = profile / 'cover.in'; os.mkfifo(fifo)
        holder = subprocess.Popen(['sh', '-c', f'exec sleep infinity > {fifo}'], start_new_session=True)
        cover_log = profile / 'cover.out'
        cover = subprocess.Popen(['python3', str(LAB / 'cover_player.py'), 'play', '--color', 'ff00ff', '--title', 'pg293live cover'],
                                 env={**os.environ, **session}, stdin=open(fifo), stdout=open(cover_log, 'w'), stderr=subprocess.STDOUT, start_new_session=True)
        state['cover'] = cover.pid; state['holder'] = holder.pid
        for _ in range(100):
            text = cover_log.read_text()
            if 'covered' in text or cover.poll() is not None: break
            time.sleep(.2)
        state['coverLine'] = cover_log.read_text().strip()[:400]
        if 'covered' not in state['coverLine']:
            print(json.dumps({'cover': state['coverLine']})); os.kill(cover.pid, signal.SIGTERM); os.kill(holder.pid, signal.SIGTERM)
            subprocess.run(['rm', '-rf', str(profile)]); sys.exit(3)
        log = open(profile / 'resident.log', 'wb')
        resident = subprocess.Popen(['/usr/bin/putgd', '--background'], env=environment(state), stdout=log, stderr=log, start_new_session=True)
        state['resident'] = resident.pid
        STATE.write_text(json.dumps(state))
        for _ in range(100):
            try:
                if cli(state, 'diagnostics', timeout=5)['result']['journalReady']: break
            except Exception: pass
            time.sleep(.2)
        doc = cli(state, 'settings.developmentTemplate')['result']['document']
        doc['profiles'] = []
        doc['actions'] = {a: {'destination': 'local', 'profileId': None, 'shortcut': ''} for a in doc['actions']}
        doc['archive']['root'] = str(profile / 'archive')
        doc['preferences'] = {'notifications': False, 'launchAtLogin': False}
        revision = cli(state, 'settings.get')['result']['metadata']['observedRevision']
        print(json.dumps({'patch': cli(state, 'settings.patch', {'patch': doc, 'expectedRevision': revision})['ok']}))
        check = cli(state, 'update.check')
        result = check.get('result') or {}
        print(json.dumps({'check': check['ok'], 'error': check.get('error'), 'latest': (result.get('result') or {}).get('latest'),
                          'current': (result.get('build') or {}).get('version'), 'notifies': result.get('notifies'),
                          'action': result.get('action')}, indent=1))
        print(json.dumps(cli(state, 'ui.show', {'view': 'settings'})))
    elif command == 'cli':
        state = json.loads(STATE.read_text())
        print(json.dumps(cli(state, sys.argv[2], json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}), indent=1))
    elif command == 'frame':
        from rfb import RFB
        state = json.loads(STATE.read_text()); name = sys.argv[2]
        # The VNC frame includes the pointer: park it in the top edge strip first.
        client = RFB(state['port']); client.pointer(client.width - 2, 1); time.sleep(.4); w, h, px = client.frame(); client.close()
        edge_y, edge_x = int(h * .045), int(w * .03)
        xs, ys = [], []
        def cover_at(x, y):
            i = (y * w + x) * 4
            return magenta(px[i], px[i + 1], px[i + 2])
        for y in range(edge_y, h - edge_y, 2):
            row = px[y * w * 4:(y + 1) * w * 4]
            # A panel row spans the output; windows never touch both sides.
            if not cover_at(edge_x, y) and not cover_at(w - edge_x - 1, y):
                continue
            for x in range(edge_x, w - edge_x, 2):
                if not magenta(row[x * 4], row[x * 4 + 1], row[x * 4 + 2]):
                    xs.append(x); ys.append(y)
        if not xs:
            print(json.dumps({'ok': False, 'reason': 'nothing but cover'})); sys.exit(3)
        m = 36  # past the window shadow
        x0, x1, y0, y1 = max(min(xs) - m, 0), min(max(xs) + m, w - 1), max(min(ys) - m, 0), min(max(ys) + m, h - 1)
        # Stop short of a panel along the top or bottom edge (rows covered on neither side).
        panel = [y for y in range(h) if not cover_at(edge_x, y) and not cover_at(w - edge_x - 1, y)]
        below = [y for y in panel if y > max(ys)]
        above = [y for y in panel if y < min(ys)]
        if below: y1 = min(y1, min(below) - 4)
        if above: y0 = max(y0, max(above) + 4)
        def fraction(points):
            hits = sum(1 for x, y in points if magenta(*px[(y * w + x) * 4:(y * w + x) * 4 + 3]))
            return hits / max(len(points), 1)
        edges = {'top': fraction([(x, y0 + 2) for x in range(x0, x1, 3)]), 'bottom': fraction([(x, y1 - 2) for x in range(x0, x1, 3)]),
                 'left': fraction([(x0 + 2, y) for y in range(y0, y1, 3)]), 'right': fraction([(x1 - 2, y) for y in range(y0, y1, 3)])}
        ok = all(v >= .9 for v in edges.values())
        info = {'ok': ok, 'frame': [w, h], 'crop': [x0, y0, x1 - x0 + 1, y1 - y0 + 1], 'edges': {k: round(v, 3) for k, v in edges.items()}}
        if ok:
            rows = []
            for y in range(y0, y1 + 1):
                line = px[(y * w + x0) * 4:(y * w + x1 + 1) * 4]
                rgb = bytearray(len(line) // 4 * 3); rgb[0::3] = line[2::4]; rgb[1::3] = line[1::4]; rgb[2::3] = line[0::4]
                rows.append(bytes(rgb))
            png(EVIDENCE / f'{name}.png', x1 - x0 + 1, y1 - y0 + 1, rows)
            info['file'] = str(EVIDENCE / f'{name}.png')
            (EVIDENCE / f'{name}.json').write_text(json.dumps(info))
        print(json.dumps(info))
        if not ok: sys.exit(3)
    elif command == 'click':
        from rfb import RFB
        state = json.loads(STATE.read_text())
        client = RFB(state['port']); client.click(int(sys.argv[2]), int(sys.argv[3])); time.sleep(.3); client.pointer(client.width - 2, 1); client.close()
        print(json.dumps({'clicked': [int(sys.argv[2]), int(sys.argv[3])]}))
    elif command == 'scroll':  # wheel down N notches at X Y
        from rfb import RFB
        state = json.loads(STATE.read_text()); x, y, n = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
        client = RFB(state['port']); client.pointer(x, y); time.sleep(.1)
        for _ in range(abs(n)):
            client.sock.sendall(struct.pack('>BBHH', 5, 16 if n > 0 else 8, x, y)); time.sleep(.03)
            client.sock.sendall(struct.pack('>BBHH', 5, 0, x, y)); time.sleep(.06)
        time.sleep(.4); client.pointer(client.width - 2, 1); client.close()
        print(json.dumps({'scrolled': n}))
    elif command == 'stop':
        state = json.loads(STATE.read_text())
        try: print(json.dumps(cli(state, 'ui.quit', timeout=10)))
        except Exception as error: print(json.dumps({'quit': str(error)}))
        for key in ('resident', 'cover', 'holder'):
            for _ in range(50):
                try: os.kill(state[key], 0)
                except ProcessLookupError: break
                if _ == 20: os.kill(state[key], signal.SIGTERM)
                if _ == 40: os.kill(state[key], signal.SIGKILL)
                time.sleep(.2)
        subprocess.run(['rm', '-rf', state['profile']]); STATE.unlink()
        print(json.dumps({'stopped': True, 'left': subprocess.run(['pgrep', '-af', '/usr/bin/putgd|cover_player.py'], capture_output=True, text=True).stdout.strip()}))


if __name__ == '__main__':
    main()
