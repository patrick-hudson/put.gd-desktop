#!/usr/bin/env python3
"""#415 trials: banner click timing, put.gd vs a plain gdbus Notify control.

    trial415.py putgd NAME --hover 0.2 --label L     upload -> banner -> move, wait HOVER, press/release
    trial415.py control NAME --hover 0.2 --label L   gdbus Notify (default action) -> same click

Prints one JSON document: the dbus members seen, ActionInvoked, timings and the banner crop.
Frames stay on the host; only edge-checked banner crops are written (afa.crop).
"""
import argparse, json, re, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import afa
from afa import Run, tools, park, notify_monitor, set_notifications, operation_done, summarize_op, crop, iso


def click(inp, target, hover):
    inp.move(*target); inp.vnc_pos = target; time.sleep(hover)
    t = time.monotonic(); inp.button(True); time.sleep(.12); inp.button(False)
    return t


def gdbus_notify(run, label):
    out = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path', '/org/freedesktop/Notifications',
                          '--method', 'org.freedesktop.Notifications.Notify', 'put.gd #415 control', '0', 'dialog-information',
                          f'#415 control {label}', 'Plain gdbus notification with a default action.', '["default", "Open"]', '{}', '10000'],
                         env=run.env(private=False), capture_output=True, text=True, timeout=20)
    return out.stdout.strip() or out.stderr.strip()


def main():
    p = argparse.ArgumentParser(); p.add_argument('kind', choices=('putgd', 'control')); p.add_argument('name')
    p.add_argument('--hover', type=float, default=.2); p.add_argument('--label', required=True); p.add_argument('--settle', type=float, default=2.0); p.add_argument('--at', help='x,y relative to the banner component box (default: inner centre, top third)')
    a = p.parse_args()
    run = Run(a.name); frames, inp, size = tools(run)
    rep = {'kind': a.kind, 'hover': a.hover, 'settle': a.settle, 'at': iso()}
    park(run, inp, size); base = frames.grab()
    monitor = notify_monitor(run); time.sleep(.6)
    if a.kind == 'putgd':
        seen = len(run.logs('Notification shown'))
        src = run.dir / 'out' / 'capture-region.png'
        eid = run.ok('editor.open', {'source': 'file', 'path': str(src)})['operationId']
        run.wait(lambda: (lambda x: x if x.get('ok') and x['result'].get('phase') == 'editing' else None)(run.cli('editor.inspect', {'id': eid})), 30)
        sub = run.ok('editor.submit', {'id': eid})
        rep['operation'] = summarize_op(operation_done(run, sub.get('operationId') or eid) or {})
        shown = run.wait(lambda: run.logs('Notification shown')[seen:] or None, 30, .25)
        rep['logged'] = [s.get('time') for s in (shown or [])]
    else:
        rep['notifyReply'] = gdbus_notify(run, a.label)
    t0 = time.monotonic()
    time.sleep(1.2); f = frames.grab()
    rep['banner'] = crop(run, f, base, f'{a.label}-banner', pick=lambda c: 30 < c['blocks'] < 4000, min_changed=.3)
    box = rep['banner'].get('inner') or (rep['banner']['components'][0]['box'] if rep['banner']['components'] else None)
    if box:
        target = ((box[0] + box[2]) // 2, box[1] + (box[3] - box[1]) // 3)
        if a.at:
            outer = rep['banner']['components'][0]['box']; dx, dy = (int(v) for v in a.at.split(','))
            target = (outer[0] + dx, outer[1] + dy); rep['outer'] = outer
        while time.monotonic() - t0 < a.settle: time.sleep(.05)
        t = click(inp, target, a.hover)
        rep.update(clicked=target, pressAfterShownSeconds=round(t - t0, 2))
        time.sleep(3); f2 = frames.grab()
        after = crop(run, f2, base, f'{a.label}-after', pick=lambda c: 30 < c['blocks'] < 4000, min_changed=.3)
        rep['bannerAfter'] = {'components': after['components'][:2]}
        park(run, inp, size)
    monitor.terminate(); signals = monitor.communicate(timeout=5)[0]
    (run.dir / f'logs/{a.label}-dbus.txt').write_text(signals)
    rep['dbusMembers'] = re.findall(r'member=(\w+)', signals)
    rep['actionInvoked'] = re.findall(r'member=ActionInvoked\n\s+uint32 \d+\n\s+string "([^"]*)"', signals)
    urls = run.dir / 'opened-urls.txt'
    rep['openedUrls'] = urls.read_text().splitlines() if urls.exists() else []
    print(json.dumps(rep, indent=1, default=str))


if __name__ == '__main__':
    main()
