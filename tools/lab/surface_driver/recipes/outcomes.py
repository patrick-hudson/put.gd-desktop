"""Tier 1 Phase A: synthetic outcomes, real input, local/loopback only.

Run with --destination local --recipes outcomes. Each recipe owns its fixtures;
no live credentials, preuploaded results, or production desktop are admitted.
The frozen player's independent VNC frame is the capture oracle, taken before
invoking capture. Pixel tolerance is zero (these runs require scale 1).
"""
from contextlib import contextmanager
import hashlib
import io
import json
import re
import shlex
import time
from urllib.parse import urlsplit

from ..api import recipe
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
                             for name in ('region', 'window', 'desktop', 'edit', 'record')},
                 'archive': {'keepUploads': True}})
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
    ctx.expect(pattern_visible(image, scene.rect), 'known fullscreen pattern covers the chosen monitor without foreign UI')
    _frame(ctx, 'independent-frozen-pattern')
    return scene, image


def _capture(ctx, scope, service, kind):
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
    Image, _, _ = image_modules()
    actual = Image.open(io.BytesIO(data))
    ctx.expect(pixels_equal(actual, expected), kind + ' pixels equal independent frozen pattern (tolerance=0)')
    ctx.note('pixel oracle: ' + str(actual.size) + ', tolerance=0')
    return detail


@recipe('region.to-upload', 'region.drag', desktop_drawn=True, timeout=300)
def region_upload(ctx):
    with _lane(ctx) as (scope, service):
        _capture(ctx, scope, service, 'region')


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
        with default_browser(ctx, 'firefox'):
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
                ctx.focus('outcome-browser', raise_window=False)
                ctx.keys('Ctrl+L', 'Ctrl+V', 'Return')
            result = open_and_verify(ctx, paste,
                                     {'kind': 's3', 'name': detail['name'], 'preview': True, 'url_host': '127.0.0.1'})
            ctx.expect(result['matched'] and result['frames'], 'guarded Firefox shows uploaded image with full-desktop frame')


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
        ctx.click('confirmActionButton', window='confirmAction')
        final = ctx.wait(lambda: (d if (d := ctx.call('history.detail', {'id': detail['id']})).get('remoteStatus')
                                 in ('removed', 'already_missing', 'failed', 'unknown', 'changed') else None),
                         'UI deletion settled', timeout=60)
        ctx.expect(final['remoteStatus'] in ('removed', 'already_missing') and not final['linkAvailable'], 'history reports removal')
        ctx.note('deleteOperationId=' + final['deleteOperationId'])
        _object(scope, service, detail, absent=True)
        _frame(ctx, 'deleted-object-no-longer-resolves')
