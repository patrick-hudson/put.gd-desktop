#!/usr/bin/env python3
"""#391 rail pictures: a started afa run gets the worst reachable rail, then its main window is shown maximized.

    rail391.py feed PORT                      serve a loopback nightly feed naming 26.1022.999-nightly (foreground)
    rail391.py prep NAME [--feed PORT]        before `afa start`: point the resident at the loopback feed
    rail391.py show NAME [--size WxH]         three-line capture output, update check, History shown and
                                              maximized (or sized WxH, as a maximized 1280x800 window gets)
    rail391.py state NAME                     the rail as the UI reports it (compact, scroll range, update row)
    rail391.py close NAME                     close the main window (KWin/wmctrl by its own class only)

Runs where afa.py runs (guest or host). Prints one JSON document.
"""
import argparse, http.server, json, os, re, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import afa
from afa import Run

FEED = {'schema': 1, 'channel': 'nightly', 'release': {'version': '26.1022.999-nightly', 'tag': 'v26.1022.999-nightly', 'publishedAt': '2026-10-22T00:00:00Z'}}


def feed(args):
    if args.version: FEED['release'].update(version=args.version, tag='v' + args.version)
    body = json.dumps(FEED).encode()
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.rstrip('/').endswith('nightly.json'):
                self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
            else:
                self.send_response(404); self.end_headers()
        def log_message(self, *a): pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    print(json.dumps({'feed': f'http://127.0.0.1:{args.port}/', 'pid': os.getpid()}), flush=True)
    server.serve_forever()


def prep(args):
    run = Run(args.name)
    if args.feed: run.meta['session']['PUTGD_UPDATE_FEED_URL'] = f'http://127.0.0.1:{args.feed}/'
    else: run.meta['session'].pop('PUTGD_UPDATE_FEED_URL', None)
    run.save(); afa.out({'feed': run.meta['session'].get('PUTGD_UPDATE_FEED_URL')})


def kwin(run, body):
    """Run a one-off KWin script in the real session (Plasma), then unload it."""
    env = run.env(private=False)
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f: f.write(body); path = f.name
    name = 'putgd-i391-' + str(os.getpid())
    try:
        out = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', '/Scripting', '--method',
                              'org.kde.kwin.Scripting.loadScript', path, name], env=env, capture_output=True, text=True, timeout=20).stdout
        number = out.strip().strip('(),').split()[0] if out.strip() else ''
        if number.lstrip('-').isdigit() and int(number) >= 0:
            for obj in (f'/Scripting/Script{number}', f'/{number}'):
                r = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', obj, '--method', 'org.kde.kwin.Script.run'],
                                   env=env, capture_output=True, text=True, timeout=20)
                if r.returncode == 0: break
            time.sleep(1)
        subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.kde.KWin', '--object-path', '/Scripting', '--method',
                        'org.kde.kwin.Scripting.unloadScript', name], env=env, capture_output=True, text=True, timeout=20)
        return number
    finally:
        os.unlink(path)


def place(run, size):
    shell = run.meta.get('shell')
    if shell == 'plasmashell':
        geometry = f'w.frameGeometry = {{x: 0, y: 0, width: {size[0]}, height: {size[1]}}};' if size else 'w.setMaximize(true, true);'
        script = ('for (const w of workspace.windowList()) { if (w.resourceClass == "putgd-ui" && w.caption.startsWith("put.gd")) { '
                  'if (w.minimized) w.minimized = false; ' + geometry + ' workspace.activeWindow = w; } }')
        return {'kwinScript': kwin(run, script)}
    env = run.env(private=False)
    ids = subprocess.run(['wmctrl', '-lx'], env=env, capture_output=True, text=True).stdout.splitlines()
    mine = [l.split()[0] for l in ids if 'putgd-ui' in l and l.split(None, 4)[-1].startswith('put.gd')]
    for wid in mine:
        if size: subprocess.run(['wmctrl', '-i', '-r', wid, '-b', 'remove,maximized_vert,maximized_horz'], env=env)
        subprocess.run(['wmctrl', '-i', '-r', wid, '-e', f'0,0,0,{size[0]},{size[1]}'] if size else ['wmctrl', '-i', '-r', wid, '-b', 'add,maximized_vert,maximized_horz'], env=env)
        subprocess.run(['wmctrl', '-i', '-a', wid], env=env)
    return {'wmctrl': mine}


def show(args):
    run = Run(args.name)
    snap = run.ok('settings.get'); doc = snap['document']
    doc['captureDefaults'] = {'destination': 'upload', 'profileId': 'afa-fixture', 'publicationMode': 'default'}
    for name, action in doc['actions'].items():
        if name == 'record': continue
        action['destination'] = 'local' if name == 'region' else 'default'; action['profileId'] = None
    saved = run.cli('settings.save', {'document': doc, 'expectedRevision': snap['metadata']['observedRevision']})
    check = run.cli('update.check', timeout=90)
    shown = run.cli('ui.show', {'view': 'history'})
    time.sleep(2.5)
    size = [int(v) for v in args.size.split('x')] if args.size else None
    placed = place(run, size); time.sleep(2.5)
    afa.out({'saved': saved.get('ok'), 'saveError': saved.get('error'), 'update': (check.get('result') or {}).get('result', check.get('error')),
             'shown': shown.get('ok'), 'placed': placed, 'state': state_of(run)})


def state_of(run):
    snap = run.cli('ui.snapshot')
    d = (snap.get('result') or {}).get('diagnostics', {})
    return {'update': d.get('update', {}).get('result'), 'ui': d.get('ui')}


def state(args): afa.out(state_of(Run(args.name)))


def close(args):
    run = Run(args.name)
    if run.meta.get('shell') == 'plasmashell':
        n = kwin(run, 'for (const w of workspace.windowList()) { if (w.resourceClass == "putgd-ui" && w.caption.startsWith("put.gd")) w.closeWindow(); }')
        afa.out({'kwinScript': n}); return
    env = run.env(private=False)
    ids = subprocess.run(['wmctrl', '-lx'], env=env, capture_output=True, text=True).stdout.splitlines()
    mine = [l.split()[0] for l in ids if 'putgd-ui' in l and l.split(None, 4)[-1].startswith('put.gd')]
    for wid in mine: subprocess.run(['wmctrl', '-i', '-c', wid], env=env)
    afa.out({'wmctrl': mine})


def crop(args):
    """X11: the main window's client area from the desktop frame, kept only if it is mostly put.gd's own colours."""
    run = Run(args.name); env = run.env(private=False)
    ids = subprocess.run(['wmctrl', '-lx'], env=env, capture_output=True, text=True).stdout.splitlines()
    mine = [l.split()[0] for l in ids if 'putgd-ui' in l and l.split(None, 4)[-1].startswith('put.gd')]
    if len(mine) != 1: afa.out({'error': 'expected one put.gd window', 'found': mine}); return
    info = subprocess.run(['xwininfo', '-id', mine[0]], env=env, capture_output=True, text=True).stdout
    get = lambda k: int(re.search(rf'{k}:\s+(-?\d+)', info).group(1))
    box = [get('Absolute upper-left X'), get('Absolute upper-left Y')]; box += [box[0] + get('Width'), box[1] + get('Height')]
    frames, _, size = afa.tools(run); f = frames.grab()
    box = [max(0, box[0]), max(0, box[1]), min(size[0], box[2]), min(size[1], box[3])]
    share = afa.own_share(f, box)
    rep = {'window': mine[0], 'box': box, 'ownShare': round(share, 3), 'screen': size}
    if share >= .4:
        path = run.dir / 'crops' / f'{args.label}.png'; afa.write_png(f, box, path); rep['kept'] = str(path)
    afa.out(rep)


def main():
    p = argparse.ArgumentParser(); sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('crop'); a.add_argument('name'); a.add_argument('label')
    a = sub.add_parser('feed'); a.add_argument('port', type=int); a.add_argument('--version')
    a = sub.add_parser('prep'); a.add_argument('name'); a.add_argument('--feed', type=int)
    a = sub.add_parser('show'); a.add_argument('name'); a.add_argument('--size')
    for n in ('state', 'close'): a = sub.add_parser(n); a.add_argument('name')
    args = p.parse_args(); {'crop': crop, 'feed': feed, 'prep': prep, 'show': show, 'state': state, 'close': close}[args.cmd](args)


if __name__ == '__main__':
    main()
