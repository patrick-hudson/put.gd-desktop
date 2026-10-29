"""Real-input Edit & upload recipes; no desktop-specific input code lives here.

The 640x320 synthetic image fixture ports the inspect/pixel invariants from
edit_acceptance.py. CLI/MCP commands are interactions in their own recipes,
never substitutes for the mouse/keyboard actions being tested.
"""
from __future__ import annotations

import base64
from contextlib import contextmanager
import json
import math
import shlex

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, recipe

CORE_ISSUE = 663
IMAGE = ('image',)
REFERENCE = '20-editor-redact'


def _result(reply):
    if 'ok' in reply:
        if not reply['ok']:
            raise AssertionError(f"editor command rejected: {reply.get('error', {}).get('code')}")
        return reply['result']
    return reply


def _inspect(ctx, op):
    return _result(ctx.call('editor.inspect', {'id': op}))


def _view(ctx, op, **params):
    return _result(ctx.view('editor.view', {'id': op, **params}))


def _frame(ctx, label, reference=REFERENCE):
    return ctx.frame(label, window='editor', reference=reference)


@contextmanager
def _session(ctx, opener=None):
    image = ctx.fixture('image')
    path = image['path'] if isinstance(image, dict) else image
    params = {'source': 'file', 'path': path}
    opened = _result(opener(params) if opener else ctx.call('editor.open', params))
    op = opened['operationId']
    try:
        def ready():
            reply = ctx.call('editor.inspect', {'id': op})
            if reply.get('ok') is False and reply.get('error', {}).get('code') == 'editor_starting':
                return None
            return _result(reply)
        state = ctx.wait(ready, 'editor ready on synthetic image')
        ctx.expect(state['phase'] == 'editing' and not state['shapes'], 'fresh private editor has no marks')
        ctx.expect([state['width'], state['height']] == [640, 320], 'four-quadrant fixture is 640x320')
        _view(ctx, op, action='fit')
        ctx.expect(_inspect(ctx, op)['view']['fitted'], 'editor opens fitted')
        _frame(ctx, 'before')
        yield op
    finally:
        # Submitted/cancelled operations have no live editor. Inspect the durable
        # operation first so cleanup cannot hide a failed assertion or cancel output.
        operation = _result(ctx.call('operation.inspect', {'id': op}))
        if operation['state'] == 'editing':
            ctx.call('editor.cancel', {'id': op})


def _tool(ctx, op, name, key=None):
    if key:
        ctx.keys(key)
    else:
        ctx.click('text:' + name.title(), window='editor')
    ctx.expect(_inspect(ctx, op)['view']['tool'] == name, f'{name} tool selected by real input')


def _point(ctx, op, x, y):
    """Image pixel centre -> canvas screen point, including current pan/zoom."""
    try:
        canvas = ctx.geometry('editor.canvas')
    except (NotImplementedError, KeyError, ValueError):
        ctx.blocked('Core must expose editor.canvas screen geometry for image-coordinate drags', issue=CORE_ISSUE)
    state = _inspect(ctx, op)
    view = state['view']
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
        _frame(ctx, 'after', '26i-editor-text-anchor')


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
        ctx.click('removeMark', window='editor')
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
        ctx.drag(_point(ctx, op, 470,80), _point(ctx, op,800,500))
        for s in _inspect(ctx, op)['shapes']:
            x,y,w,h = s['rect']
            ctx.expect(x >= 0 and y >= 0 and x+w <= 640 and y+h <= 320, 'pointer edits stay inside image')
        _frame(ctx, 'after', '26-editor-selected')


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


@recipe('editor.geometry', 'editor.keyboard-help', 'editor.destination', needs=IMAGE)
def properties(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        ctx.click('editorGeometryDisclosure', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['geometryDetails']['open'], 'geometry disclosure opens')
        for control, value in [('geometryX','30'), ('geometryY','40'), ('geometryWidth','100'), ('geometryHeight','80')]:
            ctx.click(control, window='editor')
            ctx.keys('Ctrl+A'); ctx.type_text(value); ctx.keys('Return')
        ctx.expect(_inspect(ctx, op)['shapes'][0]['rect'] == [30,40,100,80], 'real steppers update selected geometry')
        _frame(ctx, 'geometry', '26h-editor-geometry-expanded')
        ctx.click('editorKeyboardHelp', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['keyboardHelp']['open'], 'keyboard help expands')
        _frame(ctx, 'help', '26d-editor-keyboard-help')
        ctx.click('editorKeyboardHelp', window='editor')
        ctx.expect(not _inspect(ctx, op)['view']['keyboardHelp']['open'], 'keyboard help collapses')
        ctx.click('editorDestination', window='editor')
        ctx.expect(bool(_inspect(ctx, op)['view']['destination']['text']), 'destination identifies the isolated output')
        ctx.keys('Esc')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.zoom', 'editor.key-pan-alt', 'editor.mouse-pan', needs=IMAGE)
def zoom_pan(ctx):
    with _session(ctx) as op:
        ctx.click('editorZoom', window='editor')
        _frame(ctx, 'menu', '26f-editor-zoom-menu')
        ctx.keys('Esc','Ctrl+1')
        ctx.expect(_inspect(ctx, op)['view']['zoomPercent'] == 100, 'actual size shortcut')
        ctx.keys('Ctrl+=')
        ctx.expect(_inspect(ctx, op)['view']['zoomPercent'] > 100, 'zoom in shortcut')
        ctx.keys('Ctrl+-','Ctrl+0')
        ctx.expect(_inspect(ctx, op)['view']['fitted'], 'Fit shortcut')
        # Enlarge until pan has a measurable visible-rectangle effect.
        _view(ctx, op, action='zoom', percent=400)
        before = _inspect(ctx, op)['view']['visibleRect']
        ctx.keys('Alt+Left','Alt+Shift+Up')
        ctx.expect(_inspect(ctx, op)['view']['visibleRect'] != before, 'Alt arrows pan canvas')
        before = _inspect(ctx, op)['view']['visibleRect']
        canvas = ctx.geometry('editor.canvas')
        centre = (canvas['x']+canvas['width']//2, canvas['y']+canvas['height']//2)
        ctx.drag(centre, (centre[0]+60, centre[1]+30), button='middle')
        ctx.expect(_inspect(ctx, op)['view']['visibleRect'] != before, 'middle drag pans canvas')
        before = _inspect(ctx, op)['view']['visibleRect']
        ctx.scroll(*centre, clicks=-2)
        ctx.expect(_inspect(ctx, op)['view']['visibleRect'] != before, 'wheel pans canvas')
        _frame(ctx, 'after', '26-editor-selected')


@recipe('editor.cancel', 'confirm.editor-discard', needs=IMAGE)
def discard(ctx):
    with _session(ctx) as op:
        ctx.click('placeMark', window='editor')
        shapes = _inspect(ctx, op)['shapes']
        ctx.click('editorCancel', window='editor')
        ctx.expect(_inspect(ctx, op)['view']['discardConfirmation']['open'], 'dirty Cancel asks before discard')
        ctx.frame('question', window='confirm', reference='19-editor-discard-confirm')
        ctx.keys('Esc')
        ctx.expect(_inspect(ctx, op)['shapes'] == shapes and not _inspect(ctx, op)['view']['discardConfirmation']['open'], 'Escape keeps edits')
        ctx.click('editorCancel', window='editor')
        ctx.click('text:Keep editing', window='confirm')
        ctx.expect(_inspect(ctx, op)['shapes'] == shapes, 'Keep editing preserves marks')
        _frame(ctx, 'after')
        ctx.click('editorCancel', window='editor')
        ctx.click('confirmDiscard', window='confirm')
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
        # or source bytes enter receipts. Conversion uses the shipped fixture.
        path = shlex.quote(item['local']['path'])
        masks = json.dumps([s['rect'] for s in final['shapes'] if s['kind']=='redact'])
        script = f'''set -eu
p=$(mktemp --suffix=.ppm)
trap 'rm -f "$p"' EXIT
putgd-desktop-fixture --convert {path} "$p"
python3 - "$p" <<'ORACLE'
import sys
from pathlib import Path
import re
data=Path(sys.argv[1]).read_bytes()
match=re.match(rb'P6\\s+(?:#[^\\n]*\\n\\s*)?(\\d+)\\s+(\\d+)\\s+255[ \\t\\r\\n]',data)
assert match, '8-bit binary PPM header'
w,h=map(int,match.groups())
pixels=data[match.end():]
assert len(pixels)==w*h*3
assert (w,h)==(640,320)
for x,y,rw,rh in {masks}:
 for yy in range(y,y+rh):
  for xx in range(x,x+rw):
   i=(yy*w+xx)*3
   assert pixels[i:i+3]==bytes([0,0,0])
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


_blocked_recipe('held_modifiers', ('editor.mouse-square','editor.key-ctrl-cursor','editor.key-pan-space','editor.mouse-zoom'),
                'Context has no held-modifier drag/scroll or key-down/up; Shift-square, Ctrl-overdraw/cursor/zoom and Space-pan need real input')
_blocked_recipe('double_click', ('editor.double-click-text',), 'Context has no double-click input')
_blocked_recipe('window_chrome', ('editor.window-close','editor.compact'), 'Context has no real window resize/minimize/maximize/close input')
_blocked_recipe('save_copy', ('editor.save-copy','editor.save-feedback'), 'Needs target file-dialog fixture, output path and native dialog input support; owned dialog recipe is in the dialogs lane')
_blocked_recipe('preparation_failure', ('editor.preparation-failed',), 'Needs deterministic output-limit failure fixture; normal small image cannot exercise this banner')



def _named_or_legacy(ctx, name, fallback):
    try:
        ctx.click(name, window='editor')
    except (KeyError, RecipeBlocked, RecipeFailed):
        ctx.note(f'{name} absent from this catalog; using legacy selector {fallback} until fix/surface-objectnames lands')
        ctx.click(fallback, window='editor')


@recipe('editor.colours', 'editor.line-width', needs=IMAGE)
def style_controls(ctx):
    with _session(ctx) as op:
        _tool(ctx, op, 'rectangle', 'R')
        ctx.keys('Insert')
        colours = [('Red','#dc2626'),('Orange','#f59e0b'),('Yellow','#facc15'),('Green','#22c55e'),
                   ('Blue','#3b82f6'),('White','#ffffff'),('Black','#111317')]
        for name, colour in colours:
            _named_or_legacy(ctx, 'colorSwatch'+name, f'text-glob:{name} ({colour})')
            state = _inspect(ctx,op)
            ctx.expect(state['view']['style']['color']==colour and state['shapes'][0]['color']==colour, 'palette updates style and selected shape')
        for key, width in [('Home',2),('Down',4),('End',8)]:
            _named_or_legacy(ctx,'lineWidth','class:QComboBox')
            ctx.keys(key,'Return')
            state = _inspect(ctx,op)
            ctx.expect(state['view']['style']['width']==width and state['shapes'][0]['width']==width, 'line width updates style and shape')
        _frame(ctx,'width','22-editor-arrow-style')
        _tool(ctx, op, 'blur', 'B')
        ctx.keys('Insert')
        ctx.click('blurStrengthValue',window='editor'); ctx.keys('Ctrl+A'); ctx.type_text('24'); ctx.keys('Return')
        ctx.expect(_inspect(ctx,op)['shapes'][-1]['strength']==24,'blur strength control changes mark')
        _tool(ctx,op,'text','T')
        ctx.keys('Insert'); ctx.type_text('font check'); ctx.keys('Return')
        ctx.click('textSize',window='editor'); ctx.keys('Ctrl+A'); ctx.type_text('32'); ctx.keys('Return')
        ctx.expect(_inspect(ctx,op)['shapes'][-1]['fontPixels']==32,'font size control changes label')
        ctx.click('textFont',window='editor'); ctx.keys('Home','Return')
        ctx.expect(bool(_inspect(ctx,op)['shapes'][-1]['font']),'font family choice changes label')
        _frame(ctx,'after','26e-editor-text-stepper-1060')


@recipe('editor.custom-colour', needs=IMAGE)
def custom_colour(ctx):
    with _session(ctx) as op:
        _tool(ctx,op,'rectangle','R')
        ctx.click('customColor',window='editor')
        ctx.frame('picker',window='editor',reference='22-editor-arrow-style')
        ctx.keys('Esc')
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


@recipe('mcp.editor', 'mcp.editor-output', needs=IMAGE)
def mcp_editor(ctx):
    def open_editor(params): return _result(_mcp(ctx,'open_editor',params))
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
    ctx.blocked('MCP command/output checks passed; preview_editor image-content transport and editor_file_dialog open/cancel still need core backend contract',issue=CORE_ISSUE)
