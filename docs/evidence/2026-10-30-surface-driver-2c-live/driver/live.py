"""PR 726 live evidence on a boat lab guest. Synthetic values only; run on the QEMU host.

Usage (from the PR checkout): python3 sd726_live.py --run RUN --out OUT --catalog CATALOG
       --boat-id ID --desktop PROFILE --build COMMIT [--positions top,bottom]

Plants a fake share link, a B2 link, a presigned URL and a custom-endpoint link
in History rows (real loopback-S3 uploads with fake public URL bases), put.gd
notifications (both Cinnamon positions), the clipboard, a hovered tooltip, a
foreign window over put.gd and a Destinations credential field; raises a fake
token in recipe failures. The driver's evidence lives in OUT/live (live
destination label 's3'); OUT/summary.json holds only verdicts and metadata.
Nothing here contacts a real provider.
"""
import argparse
import json
import shlex
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, '.')
from PIL import Image
from tools.lab.surface_driver.api import Recipe, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import uploads
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import endpoint_hosts, guard_target, run_recipes
from tools.lab.surface_driver.target import Target

# Synthetic planted values come from a private JSON file (never committed):
# SD726_VALUES=/private/values.json with keys SHARE_BASE, B2_BASE, CUSTOM_BASE,
# SHARE, B2, PRESIGNED, CUSTOM, TOKEN, ACCESS_KEY, SECRET_KEY. The live runs
# used a fake put.gd share path, a B2 friendly URL, a B2 S3-endpoint presigned
# URL with X-Amz-Credential/Signature, a custom :9443 endpoint link, a pmtok_
# token, an AKIA-shaped access key ID and a secret key; none ever worked.
import os as _os
globals().update(json.load(open(_os.environ['SD726_VALUES'])))
FIXTURE = r'''import gi,signal
gi.require_version('Gtk','3.0')
from gi.repository import Gtk,GLib
w=Gtk.Window(title='sd726 foreign window');w.set_default_size(700,220);w.move(MOVE_X,MOVE_Y)
box=Gtk.Box(orientation=Gtk.Orientation.VERTICAL,spacing=8)
for line in LINES:
    box.pack_start(Gtk.Label(label=line,xalign=0),False,False,0)
w.add(box);w.connect('destroy',Gtk.main_quit);signal.signal(signal.SIGTERM,lambda *_:GLib.idle_add(Gtk.main_quit))
w.set_keep_above(True);w.show_all();Gtk.main()
'''


def main():
    a = argparse.ArgumentParser()
    for name in ('--run', '--catalog', '--boat-id', '--desktop', '--build'):
        a.add_argument(name, required=True)
    a.add_argument('--out', type=Path, required=True)
    a.add_argument('--positions', default='top')
    args = a.parse_args()
    args.out.mkdir(mode=0o700, parents=True, exist_ok=False)
    source = subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip()
    target = Target(run=Path(args.run))
    guard_target(argparse.Namespace(placement='boat', host='boat'), target)
    profile = load_profile(args.desktop)
    cinnamon = 'cinnamon' in args.desktop
    environment = dict(desktop=args.desktop, host='boat', placement='boat', guest=target.info['profile'], package='deb',
                       destination='s3', build=args.build, driver_source=source,
                       placement_details={'boat_sandbox_id': args.boat_id, 'run_id': target.info['id']})
    summary = {'driver_source': source, 'desktop': args.desktop, 'boat_sandbox_id': args.boat_id,
               'run_id': target.info['id'], 'cases': {}, 'notes': []}
    pids = []
    ctx = None

    def save():
        (args.out/'summary.json').write_text(json.dumps(summary, indent=1, default=str)+'\n')

    def case(name, fn):
        print('==', name, flush=True)
        ctx._deadline = time.monotonic()+600
        try:
            summary['cases'][name] = {'ok': True, 'value': fn()}
        except Exception as error:
            # Exception text can quote driver data; keep only type and location.
            summary['cases'][name] = {'ok': False, 'type': type(error).__name__,
                                      'where': traceback.extract_tb(error.__traceback__)[-1][:3]}
        print(name, 'ok' if summary['cases'][name]['ok'] else 'FAILED '+summary['cases'][name]['type'], flush=True)
        save()

    def frame(label, window=None, **kw):
        result = ctx.frame(label, window, **kw)
        meta = ctx.evidence.frames[result.path]
        with Image.open(ctx.out/result.path) as image:
            rgb = image.convert('RGB')
            black = sum(1 for p in rgb.getdata() if p == (0, 0, 0))
            share = round(black/(rgb.width*rgb.height), 3)
        return {'label': label, 'path': result.path, 'guard': meta['guard'], 'fallback': meta['fallback'],
                'policy': meta.get('policy'), 'size': meta['size'], 'black_share': share,
                'masks': len(meta['regions']), 'fully_black': share == 1.0}

    def shell(script):
        reply = ctx.shell(script)
        if reply['returncode']:
            raise RecipeFailed('helper shell failed')
        return reply['stdout']

    def owned_notify(summary_text, body):
        # A Notify from the private family's environment: the monitor keeps it.
        return shell("gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications "
                     "--method org.freedesktop.Notifications.Notify put.gd 0 '' "+shlex.quote(summary_text)+" \"$(cat <<'SDBODY'\n"
                     + body+"\nSDBODY\n)\" '[]' '{}' 8000")

    def foreign_window(lines, x, y):
        code = 'LINES='+repr(lines)+'\nMOVE_X='+str(x)+'\nMOVE_Y='+str(y)+'\n'+FIXTURE
        script = "env -u XDG_RUNTIME_DIR XDG_RUNTIME_DIR=/run/user/$(id -u) nohup python3 - <<'SDFIX' >/dev/null 2>&1 &\n"+code+"SDFIX\necho $!\n"
        pid = int(shell(script).strip())
        pids.append(pid)
        ctx.wait(lambda: any('sd726 foreign' in (w.get('title') or '') for w in ctx._layout()), 'foreign window mapped', 20)
        time.sleep(1)
        return pid

    def close_foreign(pid):
        shell('kill '+str(pid)+' 2>/dev/null || true')
        pids.remove(pid)
        time.sleep(1)

    try:
        target.start()
        ctx = DriverContext(target, environment, profile, args.catalog, args.out/'live')
        ctx._deadline = time.monotonic()+600
        if cinnamon:
            found = ctx.shell('pgrep -x mintUpdate')['stdout'].split()
            for pid in found:
                ctx.shell('kill '+pid)
            summary['notes'].append('stopped mintUpdate by exact PID: '+(','.join(found) or 'none running'))
        ctx.start_notifications()
        summary['monitor'] = {'pid': target.monitor_pid}

        # History rows: real loopback-S3 uploads whose public URL bases are fake
        # share, B2 and custom-endpoint bases. The app copies each link and
        # announces it, so notifications and the clipboard carry them too.
        def history_rows():
            first = ctx.fixture('upload')
            scope = ctx._fixture_manager.scope
            service = ctx.fixture('loopback-s3')
            links = {'loopback': first.link}
            for name, base in (('share', SHARE_BASE), ('b2', B2_BASE), ('custom', CUSTOM_BASE)):
                changed = json.loads(json.dumps(service.profile))
                changed['settings']['publicUrlBase'] = base
                scope.patch({'profiles': [changed]})
                ctx.evidence.redactor.add_link_hosts(endpoint_hosts(ctx.call('settings.get')))
                value = uploads.submit(scope, remote=True)
                links[name] = value.link
                ctx.history_ids = getattr(ctx, 'history_ids', {}) | {name: value.id}
            return {'kinds': sorted(links), 'link_hosts': sorted(ctx.evidence.redactor.link_hosts),
                    'links_registered': all(l in ctx.evidence.redactor.audit_literals for l in links.values() if l)}
        case('history_rows', history_rows)

        def history_frames():
            ctx.view('ui.show', {'view': 'history'})
            time.sleep(1.5)
            frames = [frame('history-list', 'history')]
            ctx.call('ui.history', {'action': 'select', 'ids': [ctx.history_ids['custom']]})
            time.sleep(1.5)
            frames.append(frame('history-selected-custom', 'history'))
            # Tooltip over the selected row's link field.
            rows = ctx.accessible({'owned': True, 'showing': True, 'visible': True})
            link = next((r for r in rows if r.get('objectName') == 'historyLink' and r.get('rect')), None)
            if link:
                x, y, w, h = link['rect']
                geometry = ctx.geometry('history')
                scale = geometry.get('scale', 1)
                ctx._current_window = 'history'
                ctx.hover(round((x+w/2-geometry.get('screen_x', 0))*scale), round((y+h/2-geometry.get('screen_y', 0))*scale), geometry.get('head', 0))
                time.sleep(2.5)
                frames.append(frame('history-link-hover-tooltip', 'history'))
            return {'frames': frames, 'link_field_found': bool(link)}
        case('history_frames', history_frames)

        def destinations_and_settings():
            ctx.view('ui.show', {'view': 'destinations'})
            time.sleep(1.5)
            frames = [frame('destinations', 'destinations')]
            typed = []
            import re as _re
            for selector in ('addDestinationCredentials', 'text-glob:*credentials*'):
                try:
                    ctx.click(selector, 'destinations')
                    time.sleep(1.5)
                    break
                except Exception as error:
                    summary['notes'].append('credentials button '+selector+': '+type(error).__name__)
            geometry = ctx.geometry('destinations')
            bottom = geometry['y']+geometry['height']-60
            def editable():
                return [r for r in ctx.accessible({'owned': True, 'showing': True, 'visible': True, 'pid': geometry['pid']})
                        if r['role'] in ('text', 'password text') and r.get('rect') and min(r['rect'][2:]) > 0]
            fields = editable()
            for _ in range(8):
                target_field = next((r for r in fields if _re.search(r'secret', r['name'], _re.I)), None)
                if not target_field or target_field['rect'][1]+target_field['rect'][3] < bottom:
                    break
                ctx._current_window = 'destinations'
                ctx.scroll(round(geometry['width']*0.6), round(geometry['height']*0.6), -3, geometry.get('head', 0))
                time.sleep(.8)
                fields = editable()
            summary['field_names'] = [(r['role'], r['name'][:40], r['rect']) for r in fields]
            # A pointer click here hit a QMP release error on this guest
            # (recorded below); reach the fields with real Tab key input.
            visible = next((r for r in fields if r['rect'][1]+r['rect'][3] < bottom and r['rect'][1] > geometry['y']), None)
            if visible:
                try:
                    ctx._current_window = 'destinations'
                    ctx.click_at(round(visible['rect'][0]+visible['rect'][2]/2), round(visible['rect'][1]+visible['rect'][3]/2), head=0)
                except Exception as error:
                    cause = error.__cause__
                    summary['notes'].append('pointer click: '+type(error).__name__+' cause '+type(cause).__name__+': '+str(cause)[:200])
                    ctx._input_poisoned = False  # the press was refused before dispatch or released by the guest
            for pattern, value in ((r'access key|key id', ACCESS_KEY), (r'secret', SECRET_KEY)):
                for _ in range(30):
                    rows = ctx.accessible({'owned': True, 'showing': True, 'visible': True, 'pid': geometry['pid']})
                    if any(r['role'] in ('text', 'password text') and r.get('states', {}).get('focused') and _re.search(pattern, r['name'], _re.I) for r in rows):
                        ctx.type_text(ctx.sensitive(value))
                        typed.append(pattern)
                        break
                    ctx.focus('destinations', raise_window=False)
                    ctx.keys('Tab')
            time.sleep(1)
            frames.append(frame('destinations-credentials-typed', 'destinations'))
            ctx.view('ui.show', {'view': 'settings'})
            time.sleep(1.5)
            frames.append(frame('settings', 'settings'))
            return {'frames': frames, 'typed_credentials': typed}
        case('destinations_and_settings', destinations_and_settings)

        def clipboard():
            command = ('wl-copy' if profile['session'] == 'wayland' else 'xclip -selection clipboard -in -loops 2')
            shell("(cat <<'SDCLIP' | "+command+" >/dev/null 2>&1 &)\n"+PRESIGNED+"\nSDCLIP\nsleep .5\n")
            planted = ctx.clipboard_text()
            return {'presigned_read': PRESIGNED in planted, 'registered': PRESIGNED in ctx.evidence.redactor.audit_literals}
        case('clipboard', clipboard)

        def foreign_over_history():
            ctx.view('ui.show', {'view': 'history'})
            time.sleep(1)
            geometry = ctx.geometry('history')
            pid = foreign_window(['Presigned: '+PRESIGNED[:96], 'B2: '+B2.split('://')[1], 'Custom: '+CUSTOM],
                                 int(geometry['x'])+120, int(geometry['y'])+160)
            try:
                return {'frame': frame('history-under-foreign-window', 'history'),
                        'undeclared_desktop': frame('desktop-foreign-window'),
                        'declared_desktop': frame('desktop-foreign-window-declared', desktop_safe='live proof: foreign window must still be masked')}
            finally:
                close_foreign(pid)
        case('foreign_window', foreign_over_history)

        def notifications(position):
            def run():
                if cinnamon:
                    shell('gsettings set org.cinnamon.desktop.notifications bottom-notifications '+('true' if position == 'bottom' else 'false'))
                ctx.view('ui.show', {'view': 'history'})
                frames = []
                for label, body in (('share', SHARE), ('b2', B2.split('://')[1]), ('presigned', PRESIGNED), ('custom', CUSTOM)):
                    owned_notify('Link copied', body)
                    shell("notify-send -t 8000 -a put.gd 'Shared link' \"$(cat <<'SDN'\n"+body+"\nSDN\n)\"")
                    time.sleep(1.5)
                    frames.append(frame(position+'-'+label+'-desktop'))
                    frames.append(frame(position+'-'+label+'-desktop-declared', desktop_safe='live proof: banner areas stay masked'))
                    frames.append(frame(position+'-'+label+'-history', 'history'))
                    time.sleep(7)
                rows = ctx.notifications()
                return {'frames': frames, 'notifications_read': len(rows),
                        'bodies_registered': sum(1 for r in rows if r['body'] in ctx.evidence.redactor.audit_literals)}
            return run
        for position in [p for p in args.positions.split(',') if p]:
            case('notifications_'+position, notifications(position))
        if cinnamon:
            ctx.shell('gsettings reset org.cinnamon.desktop.notifications bottom-notifications')

        def exceptions():
            def registered(c):
                c.sensitive(TOKEN)
                raise RecipeFailed('session token '+TOKEN+' rejected')
            def schemeless(c):
                raise RecipeFailed('History shows '+B2.split('://')[1]+' and '+CUSTOM.split('://')[1])
            def raw(c):
                raise RuntimeError('Authorization: Bearer '+TOKEN+' '+PRESIGNED)
            def framed(c):
                c.view('ui.show', {'view': 'history'})
                c.frame('recipe-history', 'history')
            live = ('s3',)
            rows = run_recipes(ctx, [Recipe(('editor.undo',), registered, destinations=live), Recipe(('editor.redo',), schemeless, destinations=live),
                                     Recipe(('editor.tool-text',), raw, destinations=live), Recipe(('editor.tool-redact',), framed, destinations=live)],
                               ctx.environment, ctx.out, 'sd726', args.out/'export'/'coverage.json')
            return {'rows': [(r.recipe, r.functional, r.visual) for r in rows],
                    'scan': json.loads((ctx.out/'receipt.json').read_text()).get('secret_scan', {}).get('passed'),
                    'export': ctx.environment.get('coverage_export')}
        case('exceptions_and_audit', exceptions)
    finally:
        if ctx:
            for pid in list(pids):
                target.execute('kill '+str(pid)+' 2>/dev/null || true')
        summary['cleanup'] = {'errors': target.close(), 'private_root': getattr(target, 'private_cleanup', None)}
        save()


if __name__ == '__main__':
    main()
