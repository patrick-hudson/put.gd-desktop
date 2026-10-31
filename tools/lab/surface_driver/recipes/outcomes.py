"""Tier 1 Phase A: synthetic outcomes, real input, local/loopback only.

Run with --destination local --recipes outcomes. Each recipe owns its fixtures;
no live credentials, preuploaded results, or production desktop are admitted.
The frozen player's independent VNC frame is the capture oracle, taken before
invoking capture. Pixel tolerance is zero (these runs require scale 1).
"""
from contextlib import contextmanager
import base64
import hashlib
import io
import json
import re
import shlex
import time
from urllib.parse import urlsplit

from ..api import RecipeBlocked, RecipeFailed, recipe
from ..browser import default_browser, open_and_verify
from ..catalog import image_modules
from ..fixtures.base import Scope
from . import editor, history, selectors


def pixels_equal(actual, expected):
    """Reject wrong dimensions, offsets, black frames and retained overlays."""
    return actual.size == expected.size and actual.convert('RGBA').tobytes() == expected.convert('RGBA').tobytes()


def pattern_visible(image, rect):
    """The known cover is magenta everywhere outside the player's video."""
    Image, _, _ = image_modules()
    x, y, w, h = rect
    background = image.convert('RGB').copy()
    background.paste((255, 0, 255), (x, y, x + w, y + h))
    return (pixels_equal(background, Image.new('RGB', image.size, (255, 0, 255)))
            and len(image.crop((x, y, x + w, y + h)).getcolors(w * h) or []) > 5)


def edited_pixels(actual, original):
    """Independent, fixed image-space oracle, not an editor-state assertion."""
    if actual.size != original.size or actual.convert('RGBA').getextrema()[3] != (255, 255):
        return False
    actual, original = actual.convert('RGB'), original.convert('RGB')
    # Interiors of the requested redaction must be solid black, not a blur.
    if set(actual.crop((365, 195, 545, 275)).getdata()) != {(0, 0, 0)}:
        return False
    boxes = ((20, 20, 265, 105), (340, 20, 605, 125), (30, 195, 290, 265), (350, 180, 560, 290))
    _, ImageChops, _ = image_modules()
    bounds = []
    for box in boxes:
        changed = ImageChops.difference(actual.crop(box), original.crop(box)).getbbox()
        if changed is None:
            return False
        bounds.append(tuple(value + box[i % 2] for i, value in enumerate(changed)))
    # Stroke edges and mouse-to-image quantization get four raster pixels;
    # glyph bearings get a small anchor band, independent of desktop fonts.
    for index, wanted in ((0, (30, 30, 251, 91)), (1, (350, 30, 591, 111)), (3, (360, 190, 551, 281))):
        if any(abs(a - b) > 4 for a, b in zip(bounds[index], wanted)):
            return False
    left, top, right, bottom = bounds[2]
    if not (35 <= left <= 45 and 217 <= top <= 231 and 20 <= right - left <= 180 and 5 <= bottom - top <= 60):
        return False
    for box in boxes[:3]:
        if sum(a != b for a, b in zip(actual.crop(box).getdata(), original.crop(box).getdata())) < 20:
            return False
    # No marks may appear elsewhere; this also rejects a shifted canvas/export.
    masked = actual.copy()
    for box in boxes:
        masked.paste(original.crop(box), box)
    return pixels_equal(masked, original)


def _frame(ctx, label):
    return selectors._frame(ctx, label)


@contextmanager
def _lane(ctx):
    ctx.expect(ctx.destination == 'local' and ctx.placement in ('boat', 'guest')
               and ctx.profile.get('guest') and not ctx.target.physical,
               'Phase A uses only an owned disposable guest and synthetic loopback destination')
    service = ctx.fixture('loopback-s3')
    scope = Scope(ctx)
    scope.initialize()
    scope.patch({'actions': {name: {'destination': 'upload', 'profileId': service.profile_id,
                                   'shortcut': {'window': 'Ctrl+Shift+2', 'desktop': 'Ctrl+Shift+3'}.get(name, '')}
                             for name in ('region', 'window', 'desktop', 'clipboard', 'edit', 'record')},
                 'archive': {'keepUploads': True},
                 'preferences': {'uploadClipboardBehavior': 'each_link'}})
    try:
        yield scope, service
    except BaseException:
        try:
            _frame(ctx, 'outcome-failed-before-cleanup')
        except Exception as error:
            ctx.note('failure frame unavailable: ' + type(error).__name__)
        raise
    finally:
        ctx.expect(not scope.close(), 'outcome scope cleanup succeeded')


def _clipboard_reader(scope):
    from ..fixtures.clipboard import tool
    wayland = scope.ctx.profile['session'] == 'wayland'
    executable = tool(scope, 'wl-paste' if wayland else 'xclip')
    return [executable, '--no-newline', '--type'] if wayland else [executable, '-selection', 'clipboard', '-out', '-target']


def _clipboard_bytes(scope, mime):
    # Binary data stays in memory until the synthetic image is retained. Neither
    # failed readers' stderr nor clipboard text enters evidence/command output.
    # Match ctx.clipboard_text's text selector: GNOME advertises only the
    # UTF-8-qualified MIME, whereas KDE also supplies text/plain aliases.
    selection = 'text' if mime == 'text/plain' and scope.ctx.profile['session'] == 'wayland' else mime
    result = scope.json('''import base64,json,subprocess
try:
 reply=subprocess.run(READ,capture_output=True,timeout=3)
 assert len(reply.stdout)<=32*1024*1024
 print(json.dumps({'available':reply.returncode==0,'data':base64.b64encode(reply.stdout).decode() if reply.returncode==0 else ''}))
except subprocess.TimeoutExpired:print(json.dumps({'timeout':True}))
'''.replace('READ', repr(_clipboard_reader(scope) + [selection])))
    if result.get('timeout'):
        raise RecipeBlocked('independent desktop clipboard reader timed out')
    return base64.b64decode(result['data']) if result['available'] else None


def _auto_copy(ctx, scope, detail):
    wanted = detail['receipt']['directUrl'].encode('utf-8')
    ctx.wait(lambda: _clipboard_bytes(scope, 'text/plain') == wanted,
             'auto-copy desktop clipboard equals exact uploaded URL without a copy click')
    ctx.note('auto_copy_exact_link_count=1; independent desktop clipboard reader; no copy click')


def _payload(ctx, path, kind, digest):
    selectors._retain(ctx, path, kind)
    saved = ctx.out.with_name(ctx.out.name + '-functional') / (digest + '.' + kind)
    data = saved.read_bytes()
    ctx.expect(hashlib.sha256(data).hexdigest() == digest, 'retained result has verified SHA-256')
    return data


def _object(scope, service, detail, *, absent=False):
    """Independent HTTP GET, confined to this fixture's exact loopback endpoint."""
    url = detail['receipt']['directUrl']
    parsed, endpoint = urlsplit(url), urlsplit(service.endpoint)
    scope.ctx.expect((parsed.scheme, parsed.hostname, parsed.port) == (endpoint.scheme, '127.0.0.1', endpoint.port),
                     'object verification cannot contact a live provider')
    local = detail['local']['path']
    result = scope.json('''import hashlib,json,urllib.request,urllib.error
from pathlib import Path
p=Path(LOCAL);assert p.resolve(strict=True).is_relative_to(Path(ROOT))
digest=hashlib.sha256(p.read_bytes()).hexdigest()
try:
 with urllib.request.urlopen(URL,timeout=10) as response:
  data=response.read(32*1024*1024+1);assert len(data)<=32*1024*1024
  result={'status':response.status,'sha256':hashlib.sha256(data).hexdigest()}
except urllib.error.HTTPError as error:result={'status':error.code}
print(json.dumps(dict(result,local_sha256=digest)))
'''.replace('LOCAL', repr(local)).replace('ROOT', repr(scope.ctx.target.root)).replace('URL', repr(url)))
    scope.ctx.expect(result['status'] == (404 if absent else 200), 'object no longer resolves' if absent else 'uploaded object exists')
    if not absent:
        scope.ctx.expect(result['sha256'] == result['local_sha256'], 'uploaded SHA-256 equals locally saved file')
        scope.ctx.note('uploaded/local sha256=' + result['sha256'])
    return result['local_sha256']


def _complete(scope, service, oid, kind='png'):
    ctx = scope.ctx
    ctx.note('operationId=' + oid)
    def terminal():
        op = ctx.call('operation.inspect', {'id': oid})
        return op if op['state'] in ('completed', 'failed', 'partial', 'cancelled') else None
    op = ctx.wait(terminal, 'publication settled', timeout=120)
    _frame(ctx, 'publication-result')
    ctx.expect(op['state'] == 'completed', 'publication completed: ' + op['state'])
    records = ctx.wait(lambda: [r for r in ctx.call('history.list', {'limit': 100})['records']
                               if r.get('operationId') == oid], 'durable result row')
    ctx.expect(len(records) == 1, 'one result has one durable history row')
    detail = ctx.call('history.detail', {'id': records[0]['id']})
    ctx.note('historyId=' + detail['id'])
    ctx.expect(detail['uploadStatus'] == 'uploaded' and detail['local']['status'] == 'saved', 'remote and local writes confirmed')
    digest = _object(scope, service, detail)
    return detail, _payload(ctx, detail['local']['path'], kind, digest)


def _pattern(ctx):
    scene = ctx.fixture('selector-desktop')
    scene.command('freeze')
    scene.command('raise')
    previous = ctx.profile.get('windows', {})
    ctx.profile['windows'] = {**previous, 'outcome-pattern': {'title': '^putgd synthetic fixture$'}}
    ctx._fixture_manager.scope.defer('pattern role', lambda: ctx.profile.__setitem__('windows', previous))
    ctx.focus('outcome-pattern', raise_window=False)
    ctx.expect(ctx.geometry('outcome-pattern').get('scale', 1) == 1, 'pixel-exact oracle requires scale 1; no implicit scaling tolerance')
    # Wait for a genuinely frozen frame, after freeze/raise have been presented.
    last = [None]
    def stable():
        current = ctx.target.capture_image()
        same = last[0] is not None and pixels_equal(last[0], current)
        last[0] = current
        return current if same else None
    image = ctx.wait(stable, 'frozen independent pattern frame', interval=.5)
    if not pattern_visible(image, scene.rect):
        from ..receipts import write_json
        layout = ctx._probe('layout', {'compositor': ctx.profile.get('compositor')})
        if ctx.profile.get('session') == 'x11':
            ids = [str(w['id']) for w in layout['windows']]
            result = ctx.shell("python3 - <<'CLASSES'\nimport json,subprocess\n"+
                "ids="+repr(ids)+"\nprint(json.dumps({wid:subprocess.run(['xprop','-id',wid,'WM_CLASS'],capture_output=True,text=True).stdout.strip() for wid in ids}))\nCLASSES")
            ctx.expect(result['returncode'] == 0, 'pattern obstruction classes read')
            classes = json.loads(result['stdout'])
            for window in layout['windows']:
                window['class'] = classes.get(str(window['id']), '')
        layout['pattern_rect'] = list(scene.rect)
        Image, ImageChops, _ = image_modules()
        background = image.convert('RGB').copy()
        x, y, w, h = scene.rect
        background.paste((255, 0, 255), (x, y, x+w, y+h))
        layout['outside_pattern_difference_bounds'] = ImageChops.difference(
            background, Image.new('RGB', image.size, (255, 0, 255))).getbbox()
        colors = image.crop((x, y, x+w, y+h)).convert('RGB').getcolors(4)
        layout['video_color_count'] = len(colors) if colors is not None else '>4'
        write_json(ctx.out/'pattern-occlusion.json', layout, redactor=ctx.evidence.redactor)
        ctx.evidence.frame(image, ctx.out/'frames/pattern-occlusion.png',
                           [[0, 0, image.width, image.height]], fallback=True)
    ctx.expect(pattern_visible(image, scene.rect), 'known fullscreen pattern covers the chosen monitor without foreign UI')
    _frame(ctx, 'independent-frozen-pattern')
    return scene, image


def _capture(ctx, scope, service, kind):
    _clipboard_reader(scope)  # Missing tools are infrastructure, before capture.
    scene, oracle = _pattern(ctx)
    with selectors._portal_scope(ctx):
        if kind == 'region':
            x, y, w, h = scene.rect
            rect = (x + 20, y + 20, x + w - 20, y + h - 20)
            oid = selectors._open_region(ctx, accessible=False)
            ctx.drag(rect[:2], rect[2:])
            expected = oracle.crop(rect)
        else:
            # The active fullscreen pattern is the exact window/monitor oracle.
            ctx.focus('outcome-pattern', raise_window=False)
            ctx.click_at(scene.rect[0] + 10, scene.rect[1] + 10)
            before = {o['id'] for o in ctx.call('operation.recent')['operations']}
            shortcut = ctx.call('settings.get')['document']['actions'][kind]['shortcut']
            ctx.expect(bool(shortcut), 'capture has a real keyboard shortcut')
            ctx.wait(lambda: ctx.call('ui.snapshot')['diagnostics']['shortcuts']['bindings'].get(kind, {}).get('bound'),
                     'native capture shortcut bound')
            ctx.keys(shortcut)
            fresh = ctx.wait(lambda: [o for o in ctx.call('operation.recent')['operations'] if o['id'] not in before], 'shortcut capture accepted')
            ctx.expect(len(fresh) == 1, 'shortcut created exactly one operation')
            oid = fresh[0]['id']
            expected = oracle
        detail, data = _complete(scope, service, oid)
        if kind == 'region':
            _auto_copy(ctx, scope, detail)
    Image, _, _ = image_modules()
    actual = Image.open(io.BytesIO(data))
    ctx.expect(pixels_equal(actual, expected), kind + ' pixels equal independent frozen pattern (tolerance=0)')
    ctx.note('pixel oracle: ' + str(actual.size) + ', tolerance=0')
    return detail


@recipe('region.to-upload', 'region.drag', desktop_drawn=True, timeout=300)
def region_upload(ctx):
    with _lane(ctx) as (scope, service):
        _capture(ctx, scope, service, 'region')


@recipe('region.to-upload', desktop_drawn=True, timeout=300)
def auto_copy(ctx):
    with _lane(ctx) as (scope, service):
        _capture(ctx, scope, service, 'region')


@recipe('shell.command-upload-clipboard', desktop_drawn=True, timeout=420)
def upload_clipboard(ctx):
    from ..fixtures.clipboard import owner
    from ..fixtures.uploads import clipboard_files, track
    with _lane(ctx) as (scope, service):
        source = ctx.fixture('image')
        image_digest = history._local_digest(ctx, source.path)
        text = 'putgd Tier 1 clipboard: exact UTF-8 \u2713\nsecond line\n'
        text_path = scope.root + '/source.txt'
        scope.write(text_path, text.encode('utf-8'))
        failures = []
        for kind, mime, path, expected in (
                ('image', 'image/png', source.path, image_digest),
                ('file', 'text/uri-list', text_path, hashlib.sha256(text.encode('utf-8')).hexdigest()),
                ('text', 'text/plain', text_path, hashlib.sha256(text.encode('utf-8')).hexdigest())):
            try:
                if kind == 'file':
                    clipboard_files(scope, [path])
                else:
                    owner(scope, path, mime, name=kind+'-clipboard')
                ctx.view('ui.show', {'view': 'history'})
                before = {o['id'] for o in ctx.call('operation.recent')['operations']}
                _frame(ctx, 'clipboard-'+kind+'-before-upload')
                history._click_history_node(ctx, history._command_control('clipboard'))
                oid = history._new_operation(ctx, before, 'clipboard')['id']
                track(scope, 'operation.cancel', oid)
                detail, data = _complete(scope, service, oid, 'png' if kind == 'image' else 'txt')
                _auto_copy(ctx, scope, detail)
                ctx.expect(hashlib.sha256(data).hexdigest() == expected,
                           'uploaded clipboard '+kind+' SHA-256 equals independent source bytes')
                ctx.note('upload_clipboard_'+kind+'_hash_match_count=1')
            except RecipeFailed as error:
                failures.append(str(error))
                ctx.note(kind+' clipboard oracle failed: '+str(error))
        ctx.expect(not failures, '; '.join(failures))


@recipe('history.inspector-more', desktop_drawn=True, timeout=300)
def copy_image(ctx):
    from ..fixtures.clipboard import owner
    with _lane(ctx) as (scope, service):
        _clipboard_reader(scope)
        detail, data = _complete(scope, service, ctx.fixture('upload').operation_id)
        # Replace any earlier image with a text-only source so stale pixels
        # cannot satisfy the oracle if History's copy action is broken.
        sentinel = scope.root + '/text-only'
        scope.write(sentinel, b'putgd synthetic text-only clipboard')
        owner(scope, sentinel, 'text/plain', name='copy-image-sentinel')
        _history(ctx, detail)
        ctx.click('historyInspectorMore', window='history')
        history._wait_menu(ctx, 'Copy image')
        history._click_history_node(ctx, {'name': 'Copy image', 'role': 'menu item'})
        copied = ctx.wait(lambda: _clipboard_bytes(scope, 'image/png'), 'History Copy image offers desktop image/png')
        Image, _, _ = image_modules()
        try:
            actual, expected = Image.open(io.BytesIO(copied)), Image.open(io.BytesIO(data))
            ctx.expect(actual.format == 'PNG' and pixels_equal(actual, expected), 'desktop image/png pixels equal retained original (tolerance=0)')
        except (OSError, ValueError) as error:
            raise RecipeFailed('desktop image/png is not a decodable PNG') from error
        ctx.note('copy_image_exact_pixels_count=1; independent desktop image/png reader')
        _frame(ctx, 'copy-image-exact-pixels')


def _clipboard_handlers(scope):
    # Query only metadata: known process names, bus owner and ICCCM manager.
    return scope.json('''import ctypes,ctypes.util,json,os
from pathlib import Path
import dbus
names={'klipper','plasmashell','gnome-shell','cinnamon','copyq','clipit','parcellite','xfce4-clipman','diodon','gpaste-daemon'}
processes=[]
for p in Path('/proc').iterdir():
 try:
  if p.name.isdigit() and p.stat().st_uid==os.getuid():
   name=(p/'comm').read_text().strip()
   if name in names:processes.append({'name':name,'pid':int(p.name)})
 except OSError:pass
bus=dbus.SessionBus()
klipper=str(bus.get_name_owner('org.kde.klipper')) if bus.name_has_owner('org.kde.klipper') else None
manager=None
if __X11__:
 lib=ctypes.CDLL(ctypes.util.find_library('X11'))
 lib.XOpenDisplay.argtypes=[ctypes.c_char_p];lib.XOpenDisplay.restype=ctypes.c_void_p
 lib.XInternAtom.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int];lib.XInternAtom.restype=ctypes.c_ulong
 lib.XGetSelectionOwner.argtypes=[ctypes.c_void_p,ctypes.c_ulong];lib.XGetSelectionOwner.restype=ctypes.c_ulong
 lib.XCloseDisplay.argtypes=[ctypes.c_void_p]
 display=lib.XOpenDisplay(None);assert display
 try:manager=int(lib.XGetSelectionOwner(display,lib.XInternAtom(display,b'CLIPBOARD_MANAGER',False)))
 finally:lib.XCloseDisplay(display)
print(json.dumps({'processes':processes,'klipperBusOwner':klipper,'x11ManagerWindow':manager}))
'''.replace('__X11__', repr(scope.ctx.profile['session'] == 'x11')))


@recipe('tray.menu-quit', 'cli.launcher-background', desktop_drawn=True, timeout=360)
def survives_exit(ctx):
    from ..fixtures.resident import lifecycle
    with _lane(ctx) as (scope, service):
        resident = lifecycle(ctx._fixture_manager.scope)
        handlers = _clipboard_handlers(scope)
        ctx.note('clipboard_handlers='+json.dumps(handlers, sort_keys=True))
        detail = _capture(ctx, scope, service, 'region')
        wanted = detail['receipt']['directUrl'].encode('utf-8')
        resident.quit()  # Production CLI Quit, never terminate/kill the owner.
        ctx.wait(resident.stopped, 'private resident quit and released socket', timeout=30)
        started = time.monotonic()
        ctx.wait(lambda: time.monotonic()-started >= 2, 'two seconds after Quit', timeout=4)
        after = _clipboard_bytes(scope, 'text/plain')
        expected_loss = (ctx.profile['session'] == 'x11' and handlers['x11ManagerWindow'] == 0
                         and not handlers['klipperBusOwner']
                         and not any(p['name'] in ('klipper', 'copyq', 'clipit', 'parcellite', 'xfce4-clipman', 'diodon', 'gpaste-daemon')
                                     for p in handlers['processes'])
                         and after in (None, b''))
        ctx.expect(after == wanted or expected_loss, 'clipboard retains exact auto-copied URL two seconds after Quit')
        _frame(ctx, 'clipboard-after-quit')
        resident.launch()
        started = time.monotonic()
        ctx.wait(lambda: time.monotonic()-started >= 2, 'two seconds after restart', timeout=4)
        ctx.expect(_clipboard_bytes(scope, 'text/plain') == after, 'restart does not clobber desktop clipboard')
        _frame(ctx, 'clipboard-after-restart')
        if expected_loss:
            ctx.blocked('Expected X11 clipboard loss without CLIPBOARD_MANAGER; docs/desktops/cinnamon.md: '
                        'resident must continue running to serve clipboard results. Restart preserved empty clipboard.')
        if handlers['klipperBusOwner']:
            handler = 'KDE Klipper (org.kde.klipper); text re-ownership is allowed'
        elif any(p['name'] == 'gnome-shell' for p in handlers['processes']):
            handler = 'GNOME Shell / Mutter clipboard handling'
        else:
            handler = 'X11 clipboard manager window '+str(handlers['x11ManagerWindow'])
        ctx.note('survives_exit_exact_link_count=1; persistence_handler='+handler)


@recipe('shell.command-capture-window', 'shell.command-capture-desktop', desktop_drawn=True, timeout=360)
def window_desktop(ctx):
    with _lane(ctx) as (scope, service):
        for kind in ('window', 'desktop'):
            _capture(ctx, scope, service, kind)


def _measure(ctx, helper, path):
    reply = ctx.shell(shlex.quote(helper) + ' --motion ' + shlex.quote(path) + ' 8')
    match = re.search(r'motion=([\d.]+) compared=(\d+) samples=8 durationMs=(\d+) size=640x320', reply.get('stdout', ''))
    ctx.expect(reply['returncode'] == 0 and match is not None and int(match[2]) >= 6, 'real 640x320 clip decoded for motion and duration')
    score, duration = float(match[1]), int(match[3])
    ctx.expect(4000 <= duration <= 30000, 'decoded file duration in declared 4–30 second range')
    return score, duration


def _clip(ctx, scene, frozen):
    scene.command('freeze' if frozen else 'resume')
    scene.command('raise')
    # Tier 1 prepares the known source rectangle, then drives the actual bar
    # and explicit Upload. Recording-selector gestures belong to Tier 2.
    rid = selectors._open_record(ctx, rect=scene.rect)
    ctx.expect(selectors._inspect(ctx, rid)['target']['rect'] == scene.rect, 'recording uses the known pattern rectangle')
    try:
        selectors._start(ctx, rid)
        started = time.monotonic()
        ctx.wait(lambda: time.monotonic() - started >= 4.5, 'four seconds of actual capture', timeout=7)
        role = 'bar-handle' if selectors._inspect(ctx, rid).get('overlayPlacement') == 'handle' else 'bar-recording'
        selectors._bar_click(ctx, 'stop', window=role)
        state = selectors._state(ctx, rid, 'reviewing')
        ctx.expect(4_000_000 <= state['durationUs'] <= 30_000_000, 'recording duration in declared 4–30 second range')
        path = scene.output_dir + '/' + rid + '.mp4'
        ctx.call('recording.saveCopy', {'id': rid, 'path': path, 'overwrite': False})
        exported = ctx.wait(lambda: (s if (s := selectors._inspect(ctx, rid)).get('lastExport') else None), 'clip exported', timeout=60)
        ctx.expect(exported['lastExport']['status'] == 'saved', 'clip export succeeded')
        score, duration = _measure(ctx, scene.record_helper, path)
        selectors._retain(ctx, path, 'mp4')
        ctx.note(('frozen' if frozen else 'moving') + f' clip motion={score}, durationMs={duration}')
        return rid, score
    except BaseException:
        selectors._cancel_record(ctx, rid)
        raise


@recipe('review.upload', desktop_drawn=True, timeout=480)
def recording_upload(ctx):
    with _lane(ctx) as (scope, service), selectors._portal_scope(ctx):
        ctx.expect(not ctx.call('recording.capabilities').get('active'), 'private recording session starts idle')
        def cleanup():
            active = ctx.call('recording.capabilities').get('active')
            if active:
                selectors._cancel_record(ctx, active)
        scope.defer('owned active recording', cleanup, priority=100)
        scene = ctx.fixture('selector-desktop')
        frozen, control = _clip(ctx, scene, True)
        selectors._cancel_record(ctx, frozen)
        ctx.expect(control <= 1.0, 'frozen control contains no motion')
        moving, score = _clip(ctx, scene, False)
        try:
            ctx.expect(score > max(1.0, control + 1.0), 'moving clip beats frozen control by >1.0')
            _frame(ctx, 'recording-private-review')
            ctx.click('upload', window='review')
            detail, _ = _complete(scope, service, moving, 'mp4')
            uploaded_score, duration = _measure(ctx, scene.record_helper, detail['local']['path'])
            ctx.expect(uploaded_score > max(1.0, control + 1.0), 'exact uploaded clip beats frozen control by >1.0')
            ctx.note(f'uploaded clip motion={uploaded_score}, durationMs={duration}, frozen={control}')
        finally:
            selectors._cancel_record(ctx, moving)


@recipe('editor.upload', 'editor.mouse-draw', 'editor.text-typing', desktop_drawn=True, timeout=360)
def edit_upload(ctx):
    with _lane(ctx) as (scope, service), editor._session(ctx) as oid:
        for kind, start, end in (('arrow', (30, 30), (250, 90)), ('rectangle', (350, 30), (590, 110)),
                                 ('text', (40, 220), None), ('redact', (360, 190), (550, 280))):
            editor._tool(ctx, oid, kind)
            if end:
                ctx.drag(editor._point(ctx, oid, *start), editor._point(ctx, oid, *end))
            else:
                ctx.click_at(*editor._point(ctx, oid, *start))
                ctx.wait(lambda: editor._inspect(ctx, oid)['view']['editingText'], 'mouse text anchor opened')
                ctx.type_text('Tier 1')
                ctx.keys('Return')
        shapes = editor._inspect(ctx, oid)['shapes']
        ctx.expect([s['kind'] for s in shapes] == ['arrow', 'rectangle', 'text', 'redact'], 'four real mouse marks exist')
        ctx.expect(shapes[2]['text'] == 'Tier 1', 'mouse-placed text contains the typed label')
        _frame(ctx, 'four-marks-before-upload')
        ctx.click('editorUpload', window='editor')
        _, data = _complete(scope, service, oid)
        Image, _, _ = image_modules()
        original = Image.new('RGB', (640, 320))
        from tools.lab.lab import FIXTURE_RGB
        for i, color in enumerate(FIXTURE_RGB):
            x, y = (i % 2) * 320, (i // 2) * 160
            original.paste(color, (x, y, x + 320, y + 160))
        ctx.expect(edited_pixels(Image.open(io.BytesIO(data)), original), 'solid redaction and annotations occupy the drawn image coordinates')


def _history(ctx, detail):
    ctx.view('ui.show', {'view': 'history'})
    state = ctx.view('ui.history', {'action': 'select', 'ids': [detail['id']]})
    ctx.expect(state['selectedIds'] == [detail['id']], 'History contains and selects the exact uploaded row')
    ctx.view('ui.history', {'action': 'openDetails'})


@recipe('history.copy-link', 'history.open-link', desktop_drawn=True, timeout=300)
def link_browser(ctx):
    with _lane(ctx) as (scope, service):
        detail, _ = _complete(scope, service, ctx.fixture('upload').operation_id)
        with default_browser(ctx, 'firefox') as browser:
            previous = ctx.profile.get('windows', {})
            ctx.profile['windows'] = {**previous, 'outcome-browser': {'title': '.*Firefox.*'}}
            scope.defer('browser input role', lambda: ctx.profile.__setitem__('windows', previous))
            _history(ctx, detail)
            ctx.click('copyHistoryLink', window='history')
            ctx.wait(lambda: ctx.clipboard_text() == detail['receipt']['directUrl'], 'copied link equals uploaded object URL')
            ctx.click('windowClose', window='history')
            def paste():
                # The helper proves the private blank kiosk's foreground first;
                # bind keyboard input through the normal owned-window guard.
                try:
                    ctx.focus('outcome-browser', raise_window=False)
                except RecipeBlocked:
                    # Local-only metadata diagnosis. Never emit argv, profile
                    # bytes, environment values, page text or a share URL.
                    try:
                        proof=scope.json('''import json,os
from pathlib import Path
s=json.loads((Path(DIRECTORY)/'transaction.json').read_text());p=Path('/proc')/str(s['pid'])
result={'pid':s['pid'],'processPresent':p.exists(),'environmentReadable':False,'runtimeMatches':False}
try:
 env=(p/'environ').read_bytes().split(b'\\0');result['environmentReadable']=True
 result['runtimeMatches']=('XDG_RUNTIME_DIR='+os.environ['XDG_RUNTIME_DIR']).encode() in env
except OSError:pass
print(json.dumps(result))
'''.replace('DIRECTORY', repr(browser.state['directory'])))
                        query={'pid':proof['pid']}
                        if ctx.profile.get('session')=='x11':query['owned']=False
                        rows=[r for r in ctx._probe('windows',query) if r.get('pid')==proof['pid']]
                        proof.update(accessibleWindows=len(rows),ownedAccessibleWindows=
                                     sum(bool(r['owned']) for r in rows) if all('owned' in r for r in rows) else None)
                        ctx.note('private Firefox observation: '+json.dumps(proof,sort_keys=True))
                    except Exception as error:
                        ctx.note('private Firefox metadata unavailable: '+type(error).__name__)
                    raise
                ctx.keys('Ctrl+L', 'Ctrl+V', 'Return')
            result = open_and_verify(ctx, paste,
                                     {'kind': 's3', 'name': detail['name'], 'preview': True, 'url_host': '127.0.0.1'})
            ctx.expect(result['matched'] and result['frames'], 'guarded Firefox shows uploaded image with full-desktop frame')
            if not all(ctx.evidence.frames[f['path']]['visible_pixels'] for f in result['frames']):
                ctx.blocked('Firefox navigation matched, but its required content frame is fully masked', issue=952)


@recipe('history.delete-remote', 'confirm.history-delete', desktop_drawn=True, timeout=300)
def history_delete(ctx):
    with _lane(ctx) as (scope, service):
        detail, _ = _complete(scope, service, ctx.fixture('upload').operation_id)
        row = history._row(ctx, 'upload')
        ctx.expect(row['id'] == detail['id'], 'real History row selection names the uploaded object')
        _frame(ctx, 'owned-history-row')
        ctx.keys('Shift+F10')
        history._wait_menu(ctx, 'Delete from destination…')
        ctx.click('text:Delete from destination…', window='historyRowMenu')
        history._wait_window(ctx, 'confirmAction')
        ctx.call('ui.history', {'action': 'copyDeletionDetails'})
        prepared = json.loads(ctx.clipboard_text())
        ctx.expect([t['historyId'] for t in prepared['targets']] == [detail['id']]
                   and bool(prepared['acknowledgement']), 'confirmation prepares exact owned ID and effects')
        _frame(ctx, 'exact-deletion-confirmation')
        history._confirm_button(ctx, 'confirmAction', 'Delete 1 file')
        final = ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': detail['id']})).get('remoteStatus')
                                 in ('removed', 'already_missing', 'failed', 'unknown', 'changed') else None),
                         'UI deletion settled', timeout=60)
        ctx.expect(final['remoteStatus'] in ('removed', 'already_missing') and not final['linkAvailable'], 'history reports removal')
        ctx.note('deleteOperationId=' + final['deleteOperationId'])
        _object(scope, service, detail, absent=True)
        _frame(ctx, 'deleted-object-no-longer-resolves')
