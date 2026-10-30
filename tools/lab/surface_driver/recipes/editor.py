"""Real-input Edit & upload recipes; no desktop-specific input code lives here.

The 640x320 synthetic image fixture ports the inspect/pixel invariants from
edit_acceptance.py. CLI/MCP commands are interactions in their own recipes,
never substitutes for the mouse/keyboard actions being tested.
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
import json
import inspect
import math
import shlex
import sys

from tools.lab.editor_pixels import redactions_black
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, recipe

CORE_ISSUE = 663
IMAGE = ('image',)
REFERENCE = '20-editor-redact'


def _result(reply):
    if 'ok' in reply:
        if not reply['ok']:
            raise RecipeFailed(f"editor command rejected: {reply.get('error', {}).get('code')}")
        return reply['result']
    return reply


def _inspect(ctx, op):
    return _result(ctx.call('editor.inspect', {'id': op}))


def _view(ctx, op, **params):
    return _result(ctx.view('editor.view', {'id': op, **params}))


def _frame(ctx, label, reference=REFERENCE):
    frame = ctx.frame(label, window='editor', reference=reference)
    ctx.use_catalog('editor', reference)
    return frame


def _resize(ctx, width, height):
    rect = ctx.geometry('editor')
    if [rect['width'],rect['height']] != [width,height]:
        # Ui::WindowEdges owns a 10px corner on every custom-framed window.
        # Dragging it is real compositor input, not a geometry mutation.
        start=(rect['x']+rect['width']-5,rect['y']+rect['height']-5)
        end=(rect['x']+width-5,rect['y']+height-5)
        ctx.drag(start,end)
        def sized():
            r=ctx.geometry('editor')
            return r if [r['width'],r['height']]==[width,height] else None
        try:
            ctx.wait(sized,'editor resized to matching catalog dimensions')
        except RecipeFailed:
            observed=ctx.geometry('editor')
            ctx.note(f'editor resize requested {width}x{height}; observed {observed["width"]}x{observed["height"]}')
            if abs(observed['width']-width)>4 or abs(observed['height']-height)>4:
                raise
            # A compositor can round the initial system-resize grab by a few
            # pixels. Make one real corrective drag; keep exact final bounds.
            start=(observed['x']+observed['width']-5,observed['y']+observed['height']-5)
            end=(observed['x']+width-5,observed['y']+height-5)
            ctx.drag(start,end)
            ctx.wait(sized,'editor correction reaches exact catalog dimensions')



@contextmanager
def _session(ctx, opener=None, recent_colours=None):
    image = ctx.fixture('image')
    path = image['path'] if isinstance(image, dict) else image
    if recent_colours:
        current = ctx.call('settings.get')
        ctx.call('settings.patch', {'expectedRevision':current['metadata']['observedRevision'],
                                   'patch':{'preferences':{'editorRecentColors':recent_colours}}})
    params = {'source': 'file', 'path': path}
    opened = _result(opener(params) if opener else ctx.call('editor.open', params))
    op = opened['operationId']
    ctx.note('editor operationId=' + op)
    try:
        def ready():
            # call() raises for resident errors; readiness needs the raw CLI
            # envelope to distinguish a transient helper start from failure.
            response = ctx.cli('editor.inspect', json.dumps({'id':op}))
            reply = json.loads(response['stdout'])
            if reply.get('ok') is False and reply.get('error', {}).get('code') == 'editor_starting':
                return None
            return _result(reply)
        state = ctx.wait(ready, 'editor ready on synthetic image')
        ctx.expect(state['phase'] == 'editing' and not state['shapes'], 'fresh private editor has no marks')
        ctx.expect([state['width'], state['height']] == [640, 320], 'four-quadrant fixture is 640x320')
        ctx.call('editor.raise',{'id':op})
        ctx.focus('editor', raise_window=False)
        _view(ctx, op, action='fit')
        ctx.expect(_inspect(ctx, op)['view']['fitted'], 'editor opens fitted')
        _frame(ctx, 'before')
        yield op
    except BaseException:
        try:
            ctx.frame('failure-before-cleanup')
        except Exception as error:
            ctx.note('failure frame unavailable: ' + type(error).__name__)
        raise
    finally:
        # Submitted/cancelled operations have no live editor. Inspect the durable
        # operation first so cleanup cannot hide a failed assertion or cancel output.
        failed = sys.exc_info()[0] is not None
        try:
            operation = _result(ctx.call('operation.inspect', {'id': op}))
            if operation['state'] == 'editing':
                ctx.call('editor.cancel', {'id': op})
                ctx.wait(lambda: _result(ctx.call('operation.inspect', {'id':op}))['state'] != 'editing', 'editor cleanup cancelled')
        except Exception as error:
            ctx.note('editor cleanup unavailable: ' + type(error).__name__)
            if not failed:
                raise RecipeFailed('editor cleanup did not complete') from error



def _tool(ctx, op, name, key=None):
    if key:
        ctx.keys(key)
    else:
        ctx.click('text:' + ('Select and move' if name=='select' else name.title()), window='editor')
    ctx.expect(_inspect(ctx, op)['view']['tool'] == name, f'{name} tool selected by real input')


def _place(ctx, op, kind, text=None):
    """Place through the actual button; Insert only works with canvas focus."""
    before = {shape['id'] for shape in _inspect(ctx, op)['shapes']}
    ctx.click('placeMark', window='editor')
    if kind == 'text':
        ctx.wait(lambda: _inspect(ctx, op)['view']['editingText'], 'text placement starts editing')
        ctx.type_text(text or 'font check')
        ctx.keys('Return')
    def placed():
        state = _inspect(ctx, op)
        selected = state['view']['selectedShapeId']
        return next((shape for shape in state['shapes'] if shape['id'] == selected
                     and shape['id'] not in before and shape['kind'] == kind), None)
    return ctx.wait(placed, 'new selected '+kind+' mark from Place button')['id']


def _shape(ctx, op, identity):
    shape = next((shape for shape in _inspect(ctx, op)['shapes'] if shape['id'] == identity), None)
    ctx.expect(shape is not None, 'placed mark remains in the document')
    return shape


def _point(ctx, op, x, y):
    """Image pixel centre -> canvas screen point, including current pan/zoom."""
    state = _inspect(ctx, op)
    view = state['view']
    canvas = ctx.geometry('editor.canvas')
    ctx.expect([canvas['width'], canvas['height']] == view['canvasSize'], 'geometry identifies the live editor canvas')
    scale = view['zoomPercent'] / 100
    vx, vy, vw, vh = view['visibleRect']
    # visibleRect is rounded; tolerate one image pixel in geometry assertions.
    ox = max(0, (canvas['width'] - vw * scale) / 2)
    oy = max(0, (canvas['height'] - vh * scale) / 2)
    return (math.floor(canvas['x'] + ox + (x - vx + .5) * scale),
            math.floor(canvas['y'] + oy + (y - vy + .5) * scale))


def _draw(ctx, op, kind='redact', key=None):
    _tool(ctx, op, kind, key)
    ctx.drag(_point(ctx, op, 340, 20), _point(ctx, op, 600, 140))
    state = ctx.wait(lambda: (lambda s: s if len(s['shapes']) == 1 else None)(_inspect(ctx, op)), 'one mouse-drawn mark')
    shape = state['shapes'][0]
    ctx.expect(shape['kind'] == kind, 'drag creates the chosen kind')
    slack = math.ceil(100 / state['view']['zoomPercent']) + 1
    if kind == 'arrow':
        ctx.expect(all(abs(a-b) <= slack for a,b in zip(shape['from']+shape['to'], [340,20,600,140])), 'arrow endpoints follow mouse drag')
    else:
        ctx.expect(all(abs(a-b) <= slack for a,b in zip(shape['rect'], [340,20,260,120])), 'mouse mark image geometry')
    ctx.expect(state['canUndo'], 'drawing adds undo history')
    return shape


@recipe('editor.opens-fit', 'editor.tool-redact', 'editor.mouse-draw', needs=IMAGE)
def redact(ctx):
    with _session(ctx) as op:
        state = _inspect(ctx, op)
        ctx.expect(state['view']['visibleRect'] == [0,0,640,320] and state['view']['propertiesWidth'] > 0,
                   'Fit shows the whole image beside properties')
        _draw(ctx, op)
        _frame(ctx, 'after')


@recipe('editor.tool-arrow', 'editor.tool-rectangle', 'editor.tool-highlight', 'editor.tool-select', needs=IMAGE)
def annotation_tools(ctx):
    with _session(ctx) as op:
        for kind, key in [('arrow','A'), ('rectangle','R'), ('highlight','H')]:
            # Each mouse draw begins from an empty document, then repeats the
            # tool choice through its documented key.
            _draw(ctx, op, kind)
            _frame(ctx, kind, '22-editor-arrow-style' if kind == 'arrow' else '26-editor-selected')
            ctx.keys('Ctrl+Z')
            _tool(ctx, op, kind, key)
            ctx.expect(not _inspect(ctx, op)['shapes'], 'undo restores the empty document')
        _tool(ctx, op, 'select')
        _tool(ctx, op, 'select', 'V')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.tool-blur', 'editor.blur-warning', needs=IMAGE)
def blur(ctx):
    with _session(ctx) as op:
        _draw(ctx, op, 'blur')
        _frame(ctx, 'warning', '21-editor-blur-warning')
        # The warning is a visible control, not an inferred acknowledgement.
        ctx.click('text:Choose Redact tool', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['tool'] == 'redact', 'blur warning switches to irreversible redaction')
        _tool(ctx, op, 'blur', 'B')
        _frame(ctx, 'after', '21-editor-blur-warning')


@recipe('editor.tool-text', 'editor.text-typing', 'editor.key-edit-text', needs=IMAGE)
def text_entry(ctx):
    with _session(ctx) as op:
        _tool(ctx, op, 'text')
        _tool(ctx, op, 'text', 'T')
        ctx.click_at(*_point(ctx, op, 20, 200))
        ctx.type_text('xtract')  # Tool letters remain literal while typing.
        _frame(ctx, 'text-anchor', '26i-editor-text-anchor')
        ctx.keys('Return')
        state = _inspect(ctx, op)
        ctx.expect(len(state['shapes']) == 1 and state['shapes'][0]['text'] == 'xtract', 'typed tool letters committed as text')
        ctx.expect(state['phase'] == 'editing', 'plain Return never submits')
        ctx.keys('Return')
        ctx.expect(_inspect(ctx, op)['view']['editingText'], 'Return edits selected label')
        ctx.keys('Ctrl+A')
        ctx.type_text('cancelled draft')
        ctx.keys('Esc')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['text'] == 'xtract', 'Escape discards text draft')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.undo', 'editor.redo', 'editor.place-mark', 'editor.remove-mark', needs=IMAGE)
def mark_history(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        first = _inspect(ctx, op)['shapes']
        ctx.expect(len(first) == 1, 'Place mark creates a redaction')
        ctx.click('text:Undo (Ctrl+Z)', window='editor')
        ctx.expect(not _inspect(ctx, op)['shapes'] and _inspect(ctx, op)['canRedo'], 'Undo control records redo')
        ctx.click('text:Redo (Ctrl+Shift+Z)', window='editor')
        ctx.expect(_inspect(ctx, op)['shapes'] == first, 'Redo control restores mark')
        for redo in ('Ctrl+Shift+Z', 'Ctrl+Y'):
            ctx.keys('Ctrl+Z')
            ctx.expect(not _inspect(ctx, op)['shapes'], 'keyboard Undo removes mark')
            ctx.keys(redo)
            ctx.expect(_inspect(ctx, op)['shapes'] == first, 'keyboard Redo restores mark')
        _tool(ctx, op, 'select', 'V')
        x,y,w,h = first[0]['rect']
        ctx.click_at(*_point(ctx,op,x+w/2,y+h/2))
        ctx.click('text:Remove mark', window='editor')
        ctx.expect(not _inspect(ctx, op)['shapes'], 'Remove selected mark control')
        for remove in ('Delete','Backspace'):
            _tool(ctx, op, 'redact', 'X')
            ctx.keys('Insert')
            ctx.expect(len(_inspect(ctx, op)['shapes']) == 1, 'Insert places a mark')
            ctx.keys(remove)
            ctx.expect(not _inspect(ctx, op)['shapes'], 'keyboard removes selected mark')
        _frame(ctx, 'after')


@recipe('editor.mouse-move-mark', 'editor.key-nudge', 'editor.key-resize', 'editor.key-next-mark',
        'editor.key-deselect', 'editor.marks-stay-on-image', needs=IMAGE)
def selection_geometry(ctx):
    with _session(ctx) as op:
        shape = _draw(ctx, op)
        rx, ry, rw, rh = shape['rect']
        _tool(ctx, op, 'rectangle', 'R')
        ctx.drag(_point(ctx, op, 470,80), _point(ctx, op,500,100))
        moved = _inspect(ctx, op)['shapes']
        slack = math.ceil(100 / _inspect(ctx, op)['view']['zoomPercent']) + 1
        ctx.expect(len(moved) == 1 and moved[0]['rect'][2:] == [rw,rh] and
                   abs(moved[0]['rect'][0]-rx-30) <= slack and abs(moved[0]['rect'][1]-ry-20) <= slack,
                   'drawing tool moves existing mark without creating one')
        ctx.keys('Ctrl+Z')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['rect'] == shape['rect'], 'move undo restores geometry')
        ctx.keys('Right','Down','Ctrl+Right','Ctrl+Down')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['rect'] == [rx+11,ry+11,rw,rh], '1px and 10px nudges')
        ctx.keys('Shift+Right','Shift+Down')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['rect'] == [rx+11,ry+11,rw+1,rh+1], 'Shift arrows resize selected mark')
        _tool(ctx, op, 'highlight', 'H')
        ctx.keys('Insert')
        ids = [s['id'] for s in _inspect(ctx, op)['shapes']]
        ctx.keys(']')
        ctx.expect(_inspect(ctx, op)['view']['selectedShapeId'] == ids[0], 'next mark wraps')
        ctx.keys('[')
        ctx.expect(_inspect(ctx, op)['view']['selectedShapeId'] == ids[-1], 'previous mark wraps')
        ctx.keys('Esc')
        ctx.expect(_inspect(ctx, op)['view']['selectedShapeId'] is None, 'Escape deselects')
        # Pointer clamp: move the existing redaction far beyond the bottom/right.
        canvas = ctx.geometry('editor.canvas')
        ctx.drag(_point(ctx, op, 470,80), (canvas['x']+canvas['width']-3,canvas['y']+canvas['height']-3))
        for s in _inspect(ctx, op)['shapes']:
            x,y,w,h = s['rect']
            ctx.expect(x >= 0 and y >= 0 and x+w <= 640 and y+h <= 320, 'pointer edits stay inside image')
        _tool(ctx,op,'arrow','A')
        ctx.keys('Insert')
        arrow = _inspect(ctx,op)['shapes'][-1]
        ctx.keys('Shift+Right','Ctrl+Shift+Down')
        changed = _inspect(ctx,op)['shapes'][-1]
        ctx.expect(changed['from']==arrow['from'] and changed['to']==[arrow['to'][0]+1,arrow['to'][1]+10], 'resize keys move only arrow head')
        _tool(ctx,op,'text','T')
        ctx.keys('Insert'); ctx.type_text('resize'); ctx.keys('Return')
        pixels=_inspect(ctx,op)['shapes'][-1]['fontPixels']
        ctx.keys('Shift+Right','Ctrl+Shift+Up')
        ctx.expect(_inspect(ctx,op)['shapes'][-1]['fontPixels']==pixels-9,'text resize keys change font by 1 and 10 pixels')
        _frame(ctx, 'after', '26-editor-selected')
        x,y,w,h = _inspect(ctx,op)['view']['geometry']
        font = _inspect(ctx,op)['shapes'][-1]['fontPixels']
        ctx.drag(_point(ctx,op,x+w-1,y+h-1), _point(ctx,op,x+w+20,y+h+10))
        ctx.expect(_inspect(ctx,op)['shapes'][-1]['fontPixels'] > font, 'text corner drag scales the label')
        _frame(ctx, 'text-scaled', '26-editor-selected')


@recipe('editor.tool-crop', 'editor.apply-crop', 'editor.cancel-crop', needs=IMAGE)
def crop(ctx):
    with _session(ctx) as op:
        _tool(ctx, op, 'crop')
        ctx.drag(_point(ctx, op,40,40), _point(ctx, op,600,280))
        pending = _inspect(ctx, op)
        ctx.expect(pending['view']['cropPending'] and pending['crop'] == [0,0,640,320], 'crop draft does not change output')
        _frame(ctx, 'draft', '23-editor-crop')
        ctx.click('cancelCrop', window='editor')
        ctx.expect(not _inspect(ctx, op)['view']['cropPending'], 'Cancel crop clears draft')
        _tool(ctx, op, 'crop', 'C')
        ctx.drag(_point(ctx, op,40,40), _point(ctx, op,600,280))
        draft = _inspect(ctx, op)['view']['geometry']
        ctx.click('applyCrop', window='editor')
        ctx.expect(_inspect(ctx, op)['crop'] == draft, 'Apply crop commits draft geometry')
        ctx.keys('Ctrl+Z')
        _tool(ctx, op, 'crop', 'C')
        ctx.drag(_point(ctx, op,40,40), _point(ctx, op,600,280))
        draft = _inspect(ctx, op)['view']['geometry']
        ctx.keys('Return')
        ctx.expect(_inspect(ctx, op)['crop'] == draft, 'Return applies crop without submitting')
        _frame(ctx, 'after', '23-editor-crop')


def _click_editor_text(ctx, name, roles):
    # AT-SPI may expose a same-label tooltip/text child. Resolve one enabled
    # owned interactive node, then use the editor's guarded pointer API.
    owner = ctx.geometry('editor')
    nodes = ctx.accessible({'owned':True, 'name':name, 'visible':True, 'showing':True})
    nodes = [n for n in nodes if n.get('enabled') and n.get('role') in roles and
             not n.get('popup') and n.get('rect') and min(n['rect'][2:]) > 0 and
             (not owner.get('pid') or n.get('pid')==owner['pid'])]
    if len(nodes) != 1:
        ctx.blocked('Editor control '+name+' needs one owned enabled '+str(roles)+' node, without tooltip duplicates', issue=CORE_ISSUE)
    x,y,w,h = nodes[0]['rect']
    x,y = x+w/2,y+h/2
    if not (owner['x'] <= x < owner['x']+owner['width'] and owner['y'] <= y < owner['y']+owner['height']):
        ctx.blocked('Editor control '+name+' is outside its owned window', issue=CORE_ISSUE)
    scale = owner.get('scale', 1)
    ctx.click_at(round((x-owner.get('screen_x',0))*scale),
                 round((y-owner.get('screen_y',0))*scale),head=owner.get('head',0))


def _click_keyboard_help(ctx):
    _click_editor_text(ctx, 'Keyboard help', ('push button','toggle button','check box'))


@recipe('editor.geometry', needs=IMAGE)
def properties(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        ctx.click('text:Position and size', window='editor')
        ctx.wait(lambda: _inspect(ctx, op)['view']['geometryDetails']['open'], 'geometry disclosure opens')
        for control, value in [('X position','30'), ('Y position','40'), ('Width','100'), ('Height','80')]:
            ctx.click('text:'+control+' in image pixels', window='editor')
            ctx.keys('Ctrl+A'); ctx.type_text(value); ctx.keys('Return')
        ctx.wait(lambda: _inspect(ctx, op)['shapes'][0]['rect'] == [30,40,100,80], 'real steppers update selected geometry')
        _frame(ctx, 'geometry', '26h-editor-geometry-expanded')


@recipe('editor.keyboard-help', needs=IMAGE)
def keyboard_help(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        ctx.click('text:Position and size', window='editor')
        ctx.wait(lambda: _inspect(ctx, op)['view']['geometryDetails']['open'], 'geometry disclosure opens')
        _frame(ctx, 'geometry-expanded', '26h-editor-geometry-expanded')
        # Keep the disclosure inside the actual scroll viewport (#789).
        ctx.click('text:Position and size', window='editor')
        ctx.wait(lambda: not _inspect(ctx, op)['view']['geometryDetails']['open'], 'geometry disclosure collapses')
        _click_keyboard_help(ctx)
        ctx.wait(lambda: _inspect(ctx, op)['view']['keyboardHelp']['open'], 'keyboard help expands')
        _frame(ctx, 'help', '26d-editor-keyboard-help')
        _click_keyboard_help(ctx)
        ctx.wait(lambda: not _inspect(ctx, op)['view']['keyboardHelp']['open'], 'keyboard help collapses')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.destination', needs=IMAGE)
def destination(ctx):
    with _session(ctx) as op:
        text = _inspect(ctx, op)['view']['destination']['text']
        ctx.expect(bool(text), 'destination identifies the isolated local output')
        # Qt 6.4 exposes the revealable label as "push button"; Qt 6.10
        # reports the same QAccessible::Button as "button".
        _click_editor_text(ctx, text, ('push button', 'button'))
        ctx.expect(_inspect(ctx, op)['view']['destination']['text']==text, 'destination remains visible after real pointer input')
        _frame(ctx, 'after')


@recipe('editor.zoom', needs=IMAGE)
def zoom_pan(ctx):
    with _session(ctx) as op:
        ctx.click('text:Zoom', window='editor')
        _frame(ctx, 'menu', '26f-editor-zoom-menu')
        ctx.keys('Esc')
        # Verify the editor has regained native focus after menu dismissal.
        # Keep the Zoom button's child focus so Ctrl+1 still proves the
        # documented window shortcut, rather than requiring canvas focus.
        ctx.focus('editor', raise_window=False)
        ctx.keys('Ctrl+1')
        ctx.wait(lambda: _inspect(ctx, op)['view']['zoomPercent'] == 100, 'actual size shortcut')
        ctx.keys('Ctrl+=')
        ctx.wait(lambda: _inspect(ctx, op)['view']['zoomPercent'] > 100, 'zoom in shortcut')
        ctx.keys('Ctrl+-','Ctrl+0')
        ctx.wait(lambda: _inspect(ctx, op)['view']['fitted'], 'Fit shortcut')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.key-pan-alt', needs=IMAGE)
def key_pan(ctx):
    with _session(ctx) as op:
        _tool(ctx, op, 'select', 'V')
        _view(ctx, op, action='zoom', percent=400)
        before = _inspect(ctx, op)['view']['visibleRect']
        ctx.keys('Alt+Left','Alt+Shift+Up')
        ctx.wait(lambda: _inspect(ctx, op)['view']['visibleRect'] != before, 'Alt arrows pan canvas')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.mouse-pan', needs=IMAGE)
def mouse_pan(ctx):
    with _session(ctx) as op:
        _tool(ctx, op, 'select', 'V')
        _view(ctx, op, action='zoom', percent=400)
        before = _inspect(ctx, op)['view']['visibleRect']
        centre = _point(ctx,op,320,160)
        ctx.drag(centre, (centre[0]+60, centre[1]+30), button='middle')
        ctx.wait(lambda: _inspect(ctx, op)['view']['visibleRect'] != before, 'middle drag pans canvas')
        before = _inspect(ctx, op)['view']['visibleRect']
        ctx.scroll(*centre, clicks=-2)
        ctx.wait(lambda: _inspect(ctx, op)['view']['visibleRect'] != before, 'wheel pans canvas')
        _frame(ctx, 'after', '26-editor-selected')


def _mapped_window(ctx, role, description):
    def mapped():
        try:
            return ctx.geometry(role)
        except RecipeBlocked:
            return None
    return ctx.wait(mapped, description)


def _discard_ready(ctx):
    _mapped_window(ctx, 'editorDiscard', 'discard confirmation mapped by the window manager')
    ctx.focus('editorDiscard', raise_window=False)


@recipe('editor.cancel', 'confirm.editor-discard', needs=IMAGE)
def discard(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        shapes = _inspect(ctx, op)['shapes']
        ctx.click('editorCancel', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['discardConfirmation']['open'], 'dirty Cancel asks before discard')
        _discard_ready(ctx)
        ctx.frame('question', window='editorDiscard', reference='19-editor-discard-confirm')
        ctx.keys('Esc')
        ctx.expect(_inspect(ctx, op)['shapes'] == shapes and not _inspect(ctx, op)['view']['discardConfirmation']['open'], 'Escape keeps edits')
        ctx.click('editorCancel', window='editor')
        _discard_ready(ctx)
        ctx.click('text:Keep editing', window='editorDiscard')
        ctx.expect(_inspect(ctx, op)['shapes'] == shapes, 'Keep editing preserves marks')
        _frame(ctx, 'after')
        ctx.click('editorCancel', window='editor')
        _discard_ready(ctx)
        ctx.click('confirmDiscard', window='editorDiscard')
        operation = ctx.wait(lambda: (lambda o: o if o['state']=='cancelled' else None)(_result(ctx.call('operation.inspect', {'id':op}))), 'discard completes')
        ctx.expect(not operation['items'], 'discard stores no artifact')


@recipe('editor.upload', needs=IMAGE, destinations=('local',))
def upload(ctx):
    with _session(ctx) as op:
        _draw(ctx, op)
        final = _inspect(ctx, op)
        _frame(ctx, 'after')
        ctx.click('editorUpload', window='editor')
        operation = ctx.wait(lambda: (lambda o: o if o['state'] in ('completed','failed','cancelled','partial') else None)(_result(ctx.call('operation.inspect', {'id':op}))), 'local sanitized output', timeout=60)
        ctx.expect(operation['state']=='completed' and len(operation['items'])==1, 'explicit Upload completes one item')
        item = operation['items'][0]
        ctx.expect(item['local']['status']=='saved' and item['artifact']['mime']=='image/png', 'output is a saved sanitized PNG')
        # Same independent pixel oracle as edit_acceptance.py; no preview pixels
        # or source bytes enter receipts. Decode the actual saved PNG independently.
        path = shlex.quote(item['local']['path'])
        masks = json.dumps([s['rect'] for s in final['shapes'] if s['kind']=='redact'])
        # Reuse the acceptance driver's full-mask oracle on independent PNG bytes.
        mask_check = inspect.getsource(redactions_black)
        script = f'''set -eu
python3 - {path} <<'ORACLE'
import sys
from PIL import Image
{mask_check}
with Image.open(sys.argv[1]) as saved:
 image=saved.convert("RGB")
 w,h=image.size
 pixels=image.tobytes()
assert (w,h)==(640,320)
assert redactions_black(w,pixels,{masks})
for x,y,rgb in [(100,60,[220,30,40]),(230,170,[35,70,220]),(330,290,[235,190,20])]:
 i=(y*w+x)*3
 assert pixels[i:i+3]==bytes(rgb)
ORACLE'''
        result = ctx.shell(script)
        ctx.expect(result['returncode']==0, 'every mask pixel is black and untouched fixture pixels match')


@recipe('cli.editor', needs=IMAGE)
def cli_editor(ctx):
    def cli(method, params):
        reply = ctx.cli(method, json.dumps(params))
        ctx.expect(reply['returncode']==0, method+' CLI succeeds')
        return _result(json.loads(reply['stdout']))
    with _session(ctx, lambda params: cli('editor.open', params)) as op:
        def command(method, **params):
            return cli(method, {'id':op, **params})
        command('editor.raise')
        before = command('editor.inspect')
        commands = [{'op':'add','shape':{'kind':'redact','rect':[10,10,60,30]}},
                    {'op':'add','shape':{'kind':'blur','rect':[340,180,120,100],'strength':24}},
                    {'op':'add','shape':{'kind':'arrow','from':[100,300],'to':[200,230],'color':'#ffffff','width':6}},
                    {'op':'add','shape':{'kind':'highlight','rect':[500,290,120,20]}},
                    {'op':'add','shape':{'kind':'rectangle','rect':[480,175,140,100],'color':'#111317','width':4}},
                    {'op':'add','shape':{'kind':'text','at':[20,200],'text':'ok'}}]
        applied = command('editor.apply', commands=commands)
        ctx.expect(len(applied['created'])==6 and not before['shapes'], 'CLI all six shape kinds appear live')
        rejected = ctx.cli('editor.apply', json.dumps({'id':op,'commands':[commands[0], {'op':'remove','shapeId':9999}]}))
        error = json.loads(rejected['stdout'])
        ctx.expect(rejected['returncode']!=0 and error.get('error',{}).get('code')=='edit_shape_missing' and len(_inspect(ctx,op)['shapes'])==6, 'CLI batch rejects atomically')
        preview = command('editor.preview', maxEdge=640)
        ctx.expect(preview['width']==640 and base64.b64decode(preview['png']).startswith(b'\x89PNG\r\n\x1a\n'), 'CLI preview is PNG')
        command('editor.view', action='tool', tool='select')
        ctx.expect(_inspect(ctx,op)['view']['tool']=='select', 'CLI view updates visible editor')
        _frame(ctx, 'after', '26-editor-selected')
        dialog = command('editor.fileDialog', action='inspect')
        ctx.expect(not dialog['open'], 'CLI file-dialog inspection reports closed')
        path = _copy_path(ctx)
        saved = command('editor.saveCopy', path=path)
        ctx.expect(saved['saved'] and _inspect(ctx,op)['phase']=='editing', 'CLI saves private copy without submitting')
        command('editor.submit')
        _completed(ctx,op)
    with _session(ctx, lambda params: cli('editor.open', params)) as op:
        cli('editor.cancel', {'id':op})
        operation = _result(ctx.call('operation.inspect', {'id':op}))
        ctx.expect(operation['state']=='cancelled' and not operation['items'], 'CLI cancellation stores nothing')


# These declarations intentionally stay blocked: the contract cannot express
# their real input or fixture yet. No review-only declaration pretends to prove it.
def _blocked_recipe(name, ids, reason):
    @recipe(*ids, needs=IMAGE)
    def pending(ctx):
        with _session(ctx):
            _frame(ctx, 'after')
            ctx.blocked(reason, issue=CORE_ISSUE)
    pending.__name__ = name
    globals()[name] = pending


@recipe('editor.double-click-text', needs=IMAGE)
def double_click(ctx):
    with _session(ctx) as op:
        # Put text across the canvas centre, where double_click resolves the
        # actual canvas bounds in one gesture (no second SSH probe between taps).
        ctx.call('editor.apply', {'id':op, 'commands':[{'op':'add','shape':{
            'kind':'text','at':[300,150],'text':'double click','fontPixels':32}}]})
        _tool(ctx, op, 'select', 'V')
        ctx.double_click('text:Image being edited', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['editingText'], 'double click edits the existing label')
        ctx.keys('Ctrl+A'); ctx.type_text('edited twice'); ctx.keys('Return')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['text'] == 'edited twice', 'double-click draft commits')
        _frame(ctx, 'after', '26-editor-selected')


_blocked_recipe('preparation_failure', ('editor.preparation-failed',), 'Needs deterministic output-limit failure fixture; normal small image cannot exercise this banner')



@recipe('editor.colours', needs=IMAGE)
def palette(ctx):
    with _session(ctx, recent_colours=['#123456', '#654321']) as op:
        _tool(ctx, op, 'rectangle')
        mark = _place(ctx, op, 'rectangle')
        ctx.use_catalog('editor', '22-editor-arrow-style')
        colours = [('Red','#dc2626'),('Orange','#f59e0b'),('Yellow','#facc15'),('Green','#22c55e'),
                   ('Blue','#3b82f6'),('White','#ffffff'),('Black','#111317')]
        for name, colour in colours:
            ctx.click('colorSwatch'+name, window='editor')
            state = _inspect(ctx,op)
            ctx.expect(state['view']['style']['color']==colour and _shape(ctx, op, mark)['color']==colour, 'palette updates style and selected shape')
        _frame(ctx,'after','22-editor-arrow-style')
        for colour in ('#123456', '#654321'):
            ctx.click('recentColorSwatch_'+colour[1:], window='editor')
            state = _inspect(ctx, op)
            ctx.expect(state['view']['style']['color']==colour and _shape(ctx, op, mark)['color']==colour,
                       'recent-colour swatch updates selected shape')
        _frame(ctx, 'recent-colours', '22-editor-arrow-style')


@recipe('editor.line-width', needs=IMAGE)
def style_controls(ctx):
    with _session(ctx) as op:
        _tool(ctx,op,'rectangle')
        mark = _place(ctx, op, 'rectangle')
        ctx.use_catalog('editor', '22-editor-arrow-style')
        ctx.click('lineWidth', window='editor')
        # Return closes the popup with focus on the combo. Continue through
        # its native keyboard controls instead of resolving a closing popup.
        for keys, width in [(('Home','Return'),2),(('Down',),4),(('End',),8)]:
            ctx.keys(*keys)
            state = _inspect(ctx,op)
            ctx.expect(state['view']['style']['width']==width and _shape(ctx, op, mark)['width']==width, 'line width updates style and shape')
        ctx.note('line width 2/4/8 read back on the placed mark')
        _frame(ctx,'width','22-editor-arrow-style')
        _tool(ctx, op, 'blur')
        blur = _place(ctx, op, 'blur')
        ctx.use_catalog('editor', '21-editor-blur-warning')
        ctx.click('blurStrengthValue',window='editor'); ctx.keys('Ctrl+A'); ctx.type_text('24'); ctx.keys('Return')
        ctx.expect(_shape(ctx, op, blur)['strength']==24,'blur strength control changes mark')
        _tool(ctx,op,'text')
        label = _place(ctx, op, 'text', 'font check')
        ctx.use_catalog('editor', '26e-editor-text-stepper-1060')
        ctx.click('textSize',window='editor'); ctx.keys('Ctrl+A'); ctx.type_text('32'); ctx.keys('Return')
        ctx.expect(_shape(ctx, op, label)['fontPixels']==32,'font size control changes label')
        ctx.click('textFont',window='editor'); ctx.keys('Home','Return')
        ctx.expect(bool(_shape(ctx, op, label)['font']),'font family choice changes label')
        _frame(ctx,'after','26e-editor-text-stepper-1060')


@recipe('editor.custom-colour', needs=IMAGE)
def custom_colour(ctx):
    with _session(ctx) as op:
        _tool(ctx,op,'rectangle','R')
        ctx.click('text:Custom color',window='editor')
        ctx.frame('picker',window='editorColour')
        ctx.focus('editorColour', raise_window=False)
        ctx.keys('Esc')
        ctx.focus('editor', raise_window=False)
        ctx.expect(_inspect(ctx,op)['phase']=='editing','custom picker cancel keeps editor private')
        _frame(ctx,'after','22-editor-arrow-style')
        ctx.blocked('Custom picker opened/cancelled; choosing a custom colour and recent swatch still needs native QColorDialog selectors',issue=CORE_ISSUE)


@recipe('editor.affected-text', needs=IMAGE)
def affected_text(ctx):
    with _session(ctx) as op:
        # Seed the warning condition; only the warning action is under test.
        applied = _result(ctx.call('editor.apply',{'id':op,'commands':[
            {'op':'add','shape':{'kind':'text','at':[620,300],'text':'Label beyond the edge'}}]}))
        text_id = applied['created'][0]
        ctx.expect(_inspect(ctx,op)['view']['textOutsideImage'],'fixture exposes clipped text warning')
        _frame(ctx,'warning','26b-editor-text-edge')
        ctx.click('selectAffectedText',window='editor')
        ctx.expect(_inspect(ctx,op)['view']['selectedShapeId']==text_id,'warning selects affected annotation')
        _frame(ctx,'after','26j-editor-affected-text')


def _copy_path(ctx):
    result = ctx.shell('umask 077; mktemp -d /tmp/pg-edit.XXXXXXXX')
    ctx.expect(result['returncode']==0, 'private export directory created')
    directory = result['stdout'].strip()
    ctx.expect(directory.startswith('/tmp/pg-edit.') and '\n' not in directory, 'export path is a new private test directory')
    return directory + '/copy.png'


def _completed(ctx, op):
    operation = ctx.wait(lambda: (lambda o: o if o['state'] in ('completed','failed','cancelled','partial') else None)(
        _result(ctx.call('operation.inspect',{'id':op}))), 'agent local output completes', timeout=60)
    ctx.expect(operation['state']=='completed' and len(operation['items'])==1 and operation['items'][0]['local']['status']=='saved',
               'explicit agent submit saves exactly one output')
    return operation


def _mcp(ctx, tool, arguments):
    try:
        reply = ctx.mcp(tool, arguments)
    except NotImplementedError:
        ctx.blocked('Core MCP backend not installed/configured for this target',issue=CORE_ISSUE)
    # Context returns the tool's JSON envelope; official SDK CallToolResult
    # structuredContent is accepted as well, as in mcp_editor_e2e.py.
    return reply.get('structuredContent') or reply


def _mcp_open(ctx, params):
    opened = _result(_mcp(ctx, 'open_editor', params))
    def ready():
        reply = _mcp(ctx, 'get_editor', {'id':opened['operationId']})
        if reply.get('ok') is False and reply.get('error',{}).get('code')=='editor_starting':
            return None
        return _result(reply)
    # The SDK acceptance driver also awaits get_editor readiness. Observe the
    # opaque editor state before the first editor frame reaches the guard.
    ctx.wait(ready, 'MCP editor ready before editor evidence', timeout=30)
    return opened


@recipe('mcp.editor', 'mcp.editor-output', needs=IMAGE)
def mcp_editor(ctx):
    def open_editor(params): return _mcp_open(ctx, params)
    with _session(ctx, open_editor) as op:
        def tool(name, **args): return _result(_mcp(ctx,name,{'id':op, **args}))
        tool('raise_editor')
        state = tool('get_editor')
        ctx.expect(state['phase']=='editing' and not state['shapes'],'MCP reads fresh editor')
        applied = tool('edit_image',commands=[
            {'op':'add','shape':{'kind':'redact','rect':[10,10,60,30]}},
            {'op':'add','shape':{'kind':'blur','rect':[340,180,120,100],'strength':24}},
            {'op':'add','shape':{'kind':'arrow','from':[100,300],'to':[200,230],'color':'#ffffff','width':6}},
            {'op':'add','shape':{'kind':'highlight','rect':[500,290,120,20]}},
            {'op':'add','shape':{'kind':'rectangle','rect':[480,175,140,100],'color':'#111317','width':4}},
            {'op':'add','shape':{'kind':'text','at':[20,200],'text':'MCP'}}])
        ctx.expect(len(applied['created'])==6 and len(_inspect(ctx,op)['shapes'])==6,'MCP applies every shape kind live')
        rejected = _mcp(ctx,'edit_image',{'id':op,'commands':[{'op':'remove','shapeId':9999}]})
        ctx.expect(rejected.get('ok') is False and len(_inspect(ctx,op)['shapes'])==6,'invalid MCP edit rejected without mutation')
        tool('drive_editor',action='tool',tool='select')
        ctx.expect(_inspect(ctx,op)['view']['tool']=='select','MCP drive_editor visible selection tool')
        preview = _mcp(ctx,'preview_editor',{'id':op,'max_edge':640})
        # Same official SDK image + metadata contract as mcp_editor_e2e.py.
        content = preview.get('content', [])
        ctx.expect(len(content)==2 and content[0]['type']=='image' and content[0]['mimeType']=='image/png',
                   'MCP preview transports one PNG image and metadata')
        png = base64.b64decode(content[0]['data'])
        meta = _result(json.loads(content[1]['text']))
        ctx.expect(png.startswith(b'\x89PNG\r\n\x1a\n') and [meta['sourceWidth'],meta['sourceHeight']]==[640,320],
                   'MCP preview metadata matches the synthetic image')
        tool('editor_file_dialog',action='open')
        ctx.expect(tool('editor_file_dialog',action='inspect')['open'],'MCP opens real save chooser')
        tool('editor_file_dialog',action='cancel')
        ctx.wait(lambda: not tool('editor_file_dialog',action='inspect')['open'],'MCP cancels save chooser')
        path=_copy_path(ctx)
        saved=tool('save_editor_copy',path=path)
        ctx.expect(saved['saved'] and _inspect(ctx,op)['phase']=='editing','MCP saves private PNG without submitting')
        overwrite=_mcp(ctx,'save_editor_copy',{'id':op,'path':path})
        ctx.expect(overwrite.get('ok') is False,'MCP refuses overwrite by default')
        _frame(ctx,'after','27-editor-saved')
        tool('submit_editor')
        _completed(ctx,op)
    with _session(ctx,open_editor) as op:
        _result(_mcp(ctx,'cancel_editor',{'id':op}))
        operation=_result(ctx.call('operation.inspect',{'id':op}))
        ctx.expect(operation['state']=='cancelled' and not operation['items'],'MCP cancel stores nothing')



@recipe('editor.save-copy', needs=IMAGE+('file-dialog-fallback',))
def save_copy(ctx):
    from tools.lab.surface_driver.recipes.dialogs import _backend, _ready, _input
    _backend(ctx, 'fallback')
    with _session(ctx) as op:
        inspect_dialog = lambda: _result(ctx.call('editor.fileDialog', {'id':op,'action':'inspect'}))
        ctx.click('editorSaveCopy', window='editor')
        _ready(ctx, inspect_dialog, 'fallback', 'save')
        path = _copy_path(ctx)
        _input(ctx, 'fallback', path, 'save', inspect_dialog)
        ctx.wait(lambda: (last if (last := _inspect(ctx,op)['view']['lastSave']) and
                         last.get('status') == 'saved' and last.get('path') == path else None),
                 'chooser saves exactly the private selected copy')
        ctx.focus('editor', raise_window=False)
        ctx.keys('Ctrl+S')
        _ready(ctx, inspect_dialog, 'fallback', 'save')
        ctx.keys('Esc')
        ctx.wait(lambda: not inspect_dialog()['open'], 'shortcut chooser cancels')
        ctx.focus('editor', raise_window=False)
        ctx.expect(_inspect(ctx,op)['phase']=='editing', 'save and chooser cancel keep the private editor open')
        ctx.expect(not _result(ctx.call('operation.inspect',{'id':op}))['items'], 'saving a copy never submits')
        _frame(ctx,'after','27-editor-saved')


@recipe('editor.save-feedback', needs=IMAGE)
def save_feedback(ctx):
    with _session(ctx) as op:
        path=_copy_path(ctx)
        saved=_result(ctx.call('editor.saveCopy',{'id':op,'path':path}))
        ctx.expect(saved['saved'],'private copy fixture saved')
        view=_inspect(ctx,op)['view']
        ctx.expect(view['saveFeedbackVisible'] and view['lastSave']['current'],'current saved copy banner appears')
        _frame(ctx,'saved','27-editor-saved')
        # The saved-path row uses its real overflow menu at compact widths.
        # The run observer resolves its runtime QMenu; no catalog coordinates.
        overflow = ctx.accessible({'owned':True,'name':'Saved copy actions','visible':True,'showing':True})
        if overflow:
            ctx.click('editorSavedMore',window='editor')
            ctx.click('text:Copy path',window='editorSavedMenu')
        else:
            ctx.click('copySavedPath',window='editor')
        ctx.wait(lambda: ctx.clipboard_text()==path and _inspect(ctx,op)['view']['lastCopiedPath']==path,'Copy path uses exact saved path')
        ctx.click('dismissEditorSavedCopy',window='editor')
        ctx.expect(not _inspect(ctx,op)['view']['saveFeedbackVisible'],'dismiss hides feedback')
        # A missing private subdirectory deterministically triggers save failure.
        failure=ctx.cli('editor.saveCopy',json.dumps({'id':op,'path':path+'/missing.png'}))
        ctx.expect(failure['returncode']!=0,'invalid private target fails without upload')
        ctx.expect(_inspect(ctx,op)['view']['saveFeedbackVisible'],'write failure presents recovery banner')
        _frame(ctx,'write-failure','28-editor-save-failed')
        ctx.click('editorSaveDetails',window='editor')
        ctx.expect(_inspect(ctx,op)['view']['saveDetailsOpen'],'Details reveals save failure')
        _frame(ctx,'details','28a-editor-save-path')
        ctx.click('editorSaveDetails',window='editor')
        ctx.expect(not _inspect(ctx,op)['view']['saveDetailsOpen'],'Details toggles closed')
        ctx.click('chooseAnotherLocation',window='editor')
        ctx.expect(_result(ctx.call('editor.fileDialog',{'id':op,'action':'inspect'}))['open'],'recovery opens another location chooser')
        ctx.focus('fileDialog', raise_window=False)
        ctx.keys('Esc')
        ctx.focus('editor', raise_window=False)
        ctx.wait(lambda: not _result(ctx.call('editor.fileDialog',{'id':op,'action':'inspect'}))['open'],'recovery chooser cancelled')
        _frame(ctx,'after','28-editor-save-failed')
        ctx.blocked('Saved banner/copy/overflow/dismiss/failure/details/recovery checked; Show in folder needs a bounded file-manager/window fixture',issue=CORE_ISSUE)


@recipe('editor.compact',needs=IMAGE)
def compact(ctx):
    with _session(ctx) as op:
        _resize(ctx,1060,820)
        view=_inspect(ctx,op)['view']
        ctx.expect(view['fitted'] and view['propertiesWidth']>0,'compact resize retains Fit and properties')
        _frame(ctx,'compact','26c-editor-1060')
        _resize(ctx,1180,820)
        _frame(ctx,'restored','26-editor-selected')


@recipe('editor.window-close', needs=IMAGE)
def window_chrome(ctx):
    with _session(ctx) as op:
        before=ctx.geometry('editor')
        ctx.click('windowMaximize',window='editor')
        ctx.wait(lambda: ctx.geometry('editor')['width']>before['width'],'maximize enlarges editor')
        ctx.expect(_inspect(ctx,op)['phase']=='editing','maximize preserves private edit')
        # Resolve the same stable chrome object live at its maximized position.
        ctx.click('windowMaximize',window='editor')
        ctx.wait(lambda: ctx.geometry('editor')['width']==before['width'],'restore original window')
        ctx.click('windowMinimize',window='editor')
        def minimized():
            try: state = ctx.geometry('editor').get('states', {})
            except RecipeBlocked: return True
            # X11 may retain geometry for an iconified client; the WM state
            # proves minimize even while the old rectangle remains queryable.
            return state.get('minimized') or state.get('iconified')
        ctx.wait(minimized,'minimize hides live editor window')
        ctx.call('editor.raise',{'id':op})
        _mapped_window(ctx, 'editor', 'raise restores minimized editor')
        ctx.click('placeMark',window='editor')
        ctx.click('windowClose',window='editor')
        ctx.expect(_inspect(ctx,op)['view']['discardConfirmation']['open'],'window Close asks before discarding')
        _discard_ready(ctx)
        ctx.frame('close-question',window='editorDiscard',reference='19-editor-discard-confirm')
        ctx.keys('Esc')
        ctx.focus('editor', raise_window=False)
        ctx.expect(_inspect(ctx,op)['phase']=='editing','Escape keeps editor after window Close')
        _frame(ctx,'after','26-editor-selected')


@recipe('editor.mouse-square', 'editor.key-pan-space', 'editor.mouse-zoom', needs=IMAGE, timeout=240)
def held_modifiers(ctx):
    with _session(ctx) as op:
        for kind in ('redact','blur','rectangle','highlight'):
            _tool(ctx, op, kind)
            with ctx.held_keys('Shift'):
                ctx.drag(_point(ctx, op, 80, 40), _point(ctx, op, 200, 100))
            state = _inspect(ctx, op)
            ctx.expect(len(state['shapes']) == 1 and state['shapes'][0]['kind']==kind and
                       abs(state['shapes'][0]['rect'][2]-state['shapes'][0]['rect'][3]) <= 1,
                       'Shift drag constrains '+kind+' square')
            _frame(ctx, 'square-'+kind, '26-editor-selected')
            if kind!='highlight': ctx.keys('Delete')
        _tool(ctx, op, 'rectangle', 'R')
        # Ctrl must draw through an existing mark rather than select/move it.
        with ctx.held_keys('Ctrl'):
            ctx.hover(*_point(ctx, op, 100, 60))
            _frame(ctx, 'ctrl-cursor', '26-editor-selected')
            ctx.drag(_point(ctx, op, 100, 60), _point(ctx, op, 180, 90))
        ctx.expect(len(_inspect(ctx, op)['shapes']) == 2, 'Ctrl drag draws over the existing mark')
        centre = _point(ctx, op, 320, 160)
        before = _inspect(ctx, op)['view']['zoomPercent']
        with ctx.held_keys('Ctrl'):
            ctx.scroll(*centre, clicks=2)
        ctx.expect(_inspect(ctx, op)['view']['zoomPercent'] > before, 'Ctrl wheel zooms at pointer')
        _view(ctx, op, action='zoom', percent=400)
        before = _inspect(ctx, op)['view']['visibleRect']
        centre = _point(ctx, op, 320, 160)
        with ctx.held_keys('Space'):
            ctx.drag(centre, (centre[0]+60, centre[1]+30))
        ctx.expect(_inspect(ctx, op)['view']['visibleRect'] != before, 'Space drag pans without drawing')
        ctx.expect(len(_inspect(ctx, op)['shapes']) == 2, 'held gestures preserve mark count')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.key-ctrl-cursor', needs=IMAGE)
def ctrl_cursor(ctx):
    with _session(ctx) as op:
        _draw(ctx, op, 'rectangle')
        ctx.hover(*_point(ctx, op, 470,80))
        _frame(ctx, 'move-cursor', '26-editor-selected')
        with ctx.held_keys('Ctrl'):
            ctx.hover(*_point(ctx, op, 470,80))
            _frame(ctx, 'draw-over-cursor', '26-editor-selected')
        ctx.blocked('Ctrl hover delivered, but QMP screendumps omit the cursor glyph; a core cursor-shape witness is still required', issue=CORE_ISSUE)
