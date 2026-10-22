#!/usr/bin/env python3
"""Checks this rerun adds on top of afa.py (items A-K), one per fix since nightly 842.

Run on the desktop's host (or inside a VM guest) from SSH, beside afa.py, rfb.py and i326.py,
against a run that `afa.py setup` created. Each subcommand prints one JSON document and records
it in the run's results.json.

updates-first-open  #348: a resident whose Settings was never shown; one `ui.show updates`, then a frame.
status-clean        #349: every string in update.status (and the run's install record) free of control characters.
channel             #344: switch preferences.updates.channel, Check now, update.status and a frame of Updates.
channel-options     #344: on a fresh profile, the stored channel, update.status, and the Channel dropdown opened
                    with a real click (two options, Nightly preselected on a nightly build).
menu-shortcut       #341: open a menu with real input, press an action's own shortcut with real input,
                    and check what the capture holds (the open menu) or that region selection starts.
region-size         #333: a real-input region drag of W x H must give a W x H PNG.
update-check        J: appimageupdatetool --check-for-update, optionally with SSL_CERT_FILE (#297 on Fedora 44).
"""
import argparse, hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import afa  # noqa: E402
from afa import Run, out, iso, tools, park, crop, fullshot, operation_done, summarize_op, png_read  # noqa: E402

CONTROL = re.compile(r'[\x00-\x08\x0b-\x1f\x7f]')  # tab and newline are text; ESC, CR and the rest are not


def frame_full(run, label, wait=0):
    frames, inp, size = tools(run)
    if wait: time.sleep(wait)
    park(run, inp, size)
    return fullshot(run, frames.grab(), label)


def set_pref(run, patch):
    snap = run.ok('settings.get')
    return run.cli('settings.patch', {'patch': {'preferences': patch}, 'expectedRevision': snap['metadata']['observedRevision']})


# ---------------------------------------------------------------- #348

def updates_first_open(args):
    run = Run(args.name)
    view = run.cli('ui.viewState').get('result') or {}
    before = {'settingsOpen': bool((view.get('windows') or {}).get('settings')) if isinstance(view.get('windows'), dict) else None,
              'uiShownBefore': [l.get('fields') for l in run.logs('Window shown')][:5]}
    t = time.monotonic(); r = run.cli('ui.show', {'view': 'updates'})
    time.sleep(args.wait)
    rep = {'before': before, 'show': r.get('ok'), 'seconds': round(time.monotonic() - t, 2), 'calls': 1,
           'viewState': {k: v for k, v in (run.cli('ui.viewState').get('result') or {}).items() if k in ('settings', 'appearance', 'windows')},
           'full': frame_full(run, args.label)}
    out(run.record('updatesFirstOpen', rep))


# ---------------------------------------------------------------- #349

def control_strings(value, path=''):
    found = []
    if isinstance(value, dict):
        for k, v in value.items(): found += control_strings(v, f'{path}.{k}')
    elif isinstance(value, list):
        for i, v in enumerate(value): found += control_strings(v, f'{path}[{i}]')
    elif isinstance(value, str) and CONTROL.search(value):
        found.append({'path': path or '.', 'sample': value[-160:].encode('unicode_escape').decode()})
    return found


def status_clean(args):
    run = Run(args.name)
    st = run.ok('update.status')
    rep = {'statusControl': control_strings(st), 'zsyncOutput': ((st.get('install') or {}).get('result') or {}).get('zsync', {}).get('output')}
    if args.install_record:
        rec = json.loads(Path(args.install_record).read_text())
        rep['installRecordControl'] = control_strings(rec)
    rep['pass'] = not rep['statusControl'] and not rep.get('installRecordControl')
    out(run.record('statusClean:' + args.label, rep))


# ---------------------------------------------------------------- #344

def channel(args):
    run = Run(args.name)
    patch = set_pref(run, {'updates': {'channel': args.to}})
    time.sleep(1)
    t = time.monotonic(); chk = run.cli('update.check', timeout=90)
    st = run.ok('update.status')
    run.ok('ui.show', {'view': 'updates'}); time.sleep(1.5)
    rep = {'to': args.to, 'patch': patch.get('ok'), 'patchError': patch.get('error'),
           'check': {'ok': chk.get('ok'), 'seconds': round(time.monotonic() - t, 2), 'result': chk.get('result'), 'error': chk.get('error')},
           'status': {k: st.get(k) for k in ('channel', 'channelSetting', 'supported', 'reason', 'notifies', 'error', 'result', 'build', 'install')},
           'notifications': run.logs('Notification shown')[-3:],
           'full': frame_full(run, f'{args.label}-{args.to}')}
    out(run.record(f'channel:{args.label}:{args.to}', rep))


def channel_options(args):
    run = Run(args.name); frames, inp, size = tools(run)
    doc = run.ok('settings.get')['document']
    st = run.ok('update.status')
    run.ok('ui.show', {'view': 'updates'}); time.sleep(2); park(run, inp, size)
    rep = {'storedChannel': ((doc.get('preferences') or {}).get('updates') or {}).get('channel'),
           'status': {k: st.get(k) for k in ('channel', 'channelSetting', 'build')},
           'closed': fullshot(run, frames.grab(), f'{args.label}-closed')}
    if args.click:
        x, y = (int(v) for v in args.click.split(','))
        inp.click(x, y); time.sleep(1.2)
        rep['open'] = fullshot(run, frames.grab(), f'{args.label}-open'); rep['clicked'] = [x, y]
        inp.key('Escape'); time.sleep(.5)
    out(run.record('channelOptions:' + args.label, rep))


# ---------------------------------------------------------------- #341

def binding(run, action):
    keys = (run.ok('settings.get')['document'].get('actions') or {}).get(action, {}).get('shortcut')
    if not keys: raise SystemExit(f'no shortcut configured for {action}')
    names = []
    for part in keys.split('+'):
        p = part.strip()
        names.append({'Ctrl': 'ctrl', 'Control': 'ctrl', 'Shift': 'shift', 'Alt': 'alt', 'Meta': 'super', 'Super': 'super'}.get(p, p.lower() if len(p) == 1 else p))
    return keys, names


def history_ids(run):
    return [h['id'] for h in run.ok('history.list', {'limit': 20, 'offset': 0})['records']]


def ops_since(run, seen):
    ops = (run.cli('operation.recent').get('result') or {}).get('operations') or []
    return [o for o in ops if o.get('id') not in seen]


def menu_share(png_path, menu_frame, box, tol=24):
    """Share of the menu's pixels (box from the live frame) that the captured PNG holds at the same place."""
    img = png_read(Path(png_path).read_bytes()); w, h = img[:2]; same = total = 0
    for y in range(box[1], box[3], 2):
        for x in range(box[0], box[2], 2):
            if x >= w or y >= h: continue
            total += 1; same += not afa.differs(afa.png_rgb(img, x, y), afa.pixel(menu_frame, x, y), tol)
    return round(same / total, 3) if total else None


def menu_shortcut(args):
    run = Run(args.name); frames, inp, size = tools(run)
    keys, names = binding(run, args.action)
    park(run, inp, size); base = frames.grab()
    ids = set(history_ids(run)); seen_ops = {o.get('id') for o in ((run.cli('operation.recent').get('result') or {}).get('operations') or [])}
    kind, _, where = args.open.partition(':')
    if kind == 'click':
        x, y = (int(v) for v in where.split(',')); inp.click(x, y, args.button)
    else:
        inp.key(*where.split('+'))
    time.sleep(1.5)
    menu_frame = frames.grab()
    menu = crop(run, menu_frame, base, f'{args.label}-menu-open', pick=lambda c: c['blocks'] > 20, min_changed=.2)
    box = menu.get('inner') or (menu['components'][0]['box'] if menu.get('components') else None)
    rep = {'action': args.action, 'shortcut': keys, 'opened': args.open, 'menuBox': box, 'menuFull': fullshot(run, menu_frame, f'{args.label}-menu-open')}
    t = time.monotonic(); inp.key(*names)
    if args.action == 'region':
        time.sleep(args.selector_wait)
        sel = frames.grab(); rep['selectorFull'] = fullshot(run, sel, f'{args.label}-selector')
        if args.drag:
            x0, y0, x1, y1 = (int(v) for v in args.drag.split(','))
            inp.drag(x0, y0, x1, y1); rep['drag'] = [x0, y0, x1, y1]
    new = run.wait(lambda: [i for i in history_ids(run) if i not in ids] or None, args.timeout, .5)
    rep['seconds'] = round(time.monotonic() - t, 2)
    rep['operations'] = [summarize_op(o) for o in ops_since(run, seen_ops)]
    rep['newHistory'] = new
    rep['notices'] = [l.get('fields') for l in run.logs('Notification shown')][-3:]
    rep['shortcutLog'] = [{'message': l.get('message'), 'fields': l.get('fields')} for l in run.logs('hortcut')][-6:]
    if new:
        rec = next(h for h in run.ok('history.list', {'limit': 20, 'offset': 0})['records'] if h['id'] == new[0])
        local = (rec.get('local') or {}).get('path')
        rep['history'] = {'id': rec['id'], 'name': rec.get('name'), 'uploadStatus': rec.get('uploadStatus')}
        if local and Path(local).exists():
            dest = run.dir / 'out' / f'{args.label}.png'; dest.parent.mkdir(exist_ok=True); dest.write_bytes(Path(local).read_bytes())
            w, h = png_read(dest.read_bytes())[:2]; rep['png'] = {'path': str(dest), 'size': [w, h], 'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()}
            if args.action in ('desktop', 'window') and box:
                rep['menuInCapture'] = menu_share(dest, menu_frame, box)
            if args.drag:
                rep['expectedSize'] = [abs(x1 - x0), abs(y1 - y0)]
    inp.key('Escape'); time.sleep(.6); park(run, inp, size)
    rep['after'] = fullshot(run, frames.grab(), f'{args.label}-after')
    out(run.record('menuShortcut:' + args.label, rep))


# ---------------------------------------------------------------- #333

def region_size(args):
    run = Run(args.name); frames, inp, size = tools(run)
    x0, y0, x1, y1 = (int(v) for v in args.drag.split(','))
    ids = set(history_ids(run))
    r = run.ok('action.invoke', {'action': 'region'}); time.sleep(args.selector_wait)
    inp.drag(x0, y0, x1, y1)
    op = operation_done(run, r['operationId'])
    item = (op or {}).get('items', [{}])[0] if op else {}
    local = (item.get('local') or {}).get('path')
    rep = {'drag': [x0, y0, x1, y1], 'expected': [x1 - x0, y1 - y0], 'operation': summarize_op(op or {'id': r['operationId'], 'state': 'timeout'})}
    if local and Path(local).exists():
        dest = run.dir / 'out' / f'region-{x1 - x0}x{y1 - y0}.png'; dest.write_bytes(Path(local).read_bytes())
        rep['coverCheck'] = afa.check_capture(run, dest, f'region-{x1 - x0}x{y1 - y0}')
        if dest.exists():
            rep['png'] = list(png_read(dest.read_bytes())[:2])
    rep['newHistory'] = [i for i in history_ids(run) if i not in ids]
    rep['pass'] = rep.get('png') == rep['expected'] and bool(rep['newHistory'])
    out(run.record(f'regionSize:{x1 - x0}x{y1 - y0}', rep))


# ---------------------------------------------------------------- J

def update_check(args):
    run = Run(args.name)
    tool = Path(args.tool).resolve()
    env = dict(os.environ); env.pop('DISPLAY', None); env.pop('WAYLAND_DISPLAY', None)
    if args.ssl_cert_file: env['SSL_CERT_FILE'] = args.ssl_cert_file
    r = subprocess.run([str(tool), '--check-for-update', run.meta['appimage']], capture_output=True, text=True, timeout=180, env=env)
    rep = {'tool': tool.name, 'toolSha256': afa.sha256(tool), 'sslCertFile': args.ssl_cert_file, 'exit': r.returncode,
           'signal': -r.returncode if r.returncode < 0 else None, 'stdout': r.stdout[-1500:], 'stderr': r.stderr[-1500:],
           'feed': afa.fetch('https://app.put.gd/update/v1/nightly.json')[:1500], 'image': Path(run.meta['appimage']).name}
    out(run.record('updateCheck:' + args.label, rep))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('updates-first-open'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--wait', type=float, default=3)
    a = sub.add_parser('status-clean'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--install-record')
    a = sub.add_parser('channel'); a.add_argument('name'); a.add_argument('--to', required=True, choices=('stable', 'nightly', 'build'))  # 'build' only if the PR keeps it
    a.add_argument('--label', required=True)
    a = sub.add_parser('channel-options'); a.add_argument('name'); a.add_argument('--label', required=True); a.add_argument('--click', help='X,Y of the Channel dropdown')
    a = sub.add_parser('menu-shortcut'); a.add_argument('name'); a.add_argument('--action', required=True, choices=('region', 'window', 'desktop'))
    a.add_argument('--open', required=True, help='click:X,Y (real click) or key:super (real key)'); a.add_argument('--button', default='right')
    a.add_argument('--label', required=True); a.add_argument('--drag'); a.add_argument('--selector-wait', type=float, default=2.5); a.add_argument('--timeout', type=float, default=30)
    a = sub.add_parser('region-size'); a.add_argument('name'); a.add_argument('--drag', required=True); a.add_argument('--selector-wait', type=float, default=2.5)
    a = sub.add_parser('update-check'); a.add_argument('name'); a.add_argument('--tool', required=True); a.add_argument('--ssl-cert-file'); a.add_argument('--label', required=True)
    args = p.parse_args()
    {'updates-first-open': updates_first_open, 'channel-options': channel_options, 'status-clean': status_clean, 'channel': channel, 'menu-shortcut': menu_shortcut,
     'region-size': region_size, 'update-check': update_check}[args.cmd](args)


if __name__ == '__main__':
    main()
