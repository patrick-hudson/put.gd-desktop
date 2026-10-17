#!/usr/bin/env python3
"""Cinnamon graphical acceptance for Edit & upload (#51), driven through QMP.

Run from the source root on the QEMU host after a passing `lab.py run --keep`:
    python3 tools/lab/edit_acceptance.py RUN_DIR
For a physical desktop, run it through tools/lab/physical.py.
Uses the runner's own helpers: real hotkey, region drag, mouse/keyboard input
in the editor, production CLI agent commands, QMP witnesses and the
independent fixture paste. Local output only; no network destination.
"""
import base64, hashlib, json, sys, time
from pathlib import Path
sys.path.insert(0, 'tools/lab')
import lab

run = Path(sys.argv[1]).resolve()
out = run / 'editor-acceptance'
out.mkdir(exist_ok=True)
result = {'schemaVersion': 1, 'scenario': 'edit-region-independent-paste', 'ok': False, 'checks': {}, 'witnesses': []}
started = time.monotonic()

def park_pointer():
    # Physical VNC frames include the cursor (QMP screendumps do not), so move it
    # off the fixture and editor before a frame whose pixels are matched.
    lab.pointer(run, width - 40, 40, width, height); time.sleep(.3)


def shot(name):
    witness = lab.screenshot(run, 'edit-' + name)
    result['witnesses'].append(witness)
    return witness

def check(name, ok, detail=None):
    result['checks'][name] = {'ok': bool(ok), **({'detail': detail} if detail is not None else {})}
    if not ok:
        raise lab.LabError(f'check failed: {name}: {detail}')

def wait(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise lab.LabError('timeout: ' + what)
        time.sleep(.2)

def cli_envelope(method, params):
    # The runner's cli() hides error codes behind the CLI's nonzero exit.
    body = 'putgd-cli ' + lab.shlex.quote(method) + ' ' + lab.shlex.quote(json.dumps(params)) + ' || true\n'
    return json.loads(lab.guest_session(run, body))

def editor(method, params=None):
    return lab.cli(run, 'editor.' + method, params or {})

def editor_ready(op_id):
    # Before the window is up the resident answers editor_starting, or
    # editor_unavailable while the native selector is still acquiring.
    reply = cli_envelope('editor.inspect', {'id': op_id})
    if reply['ok']:
        return reply['result']
    if reply['error']['code'] in ('editor_starting', 'editor_unavailable'):
        state = lab.cli(run, 'operation.inspect', {'id': op_id})['state']
        if state in ('accepted', 'acquiring', 'editing'):
            return None
    raise lab.LabError('editor did not open: ' + str(reply['error']))

try:
    ready = lab.read_json(run / 'ready.json')
    paint = dict(ready['paint'])
    profile = lab.load_profile(lab.state(run)['profile'])
    screen = ready['display_layout'][0]
    width, height = screen['width'], screen['height']
    if ready['session'] == 'wayland':
        # Wayland hides client origins: locate the fixture in an independent frame.
        park_pointer()
        located = lab.locate_fixture_paint(lab.screenshot(run, 'edit-locate-fixture')['path'])
        if not located:
            raise lab.LabError('fixture pattern not uniquely visible before the edit')
        paint.update(x=located['x'], y=located['y'])
    hotkey = ('Ctrl+Shift+F8', ['ctrl', 'shift', 'f8']) if profile['name'].startswith('kde') else ('Ctrl+Shift+5', ['ctrl', 'shift', '5'])
    result['display'] = {'session': ready['session'], 'desktop': ready['desktop'], 'qt': ready['qt_version'], 'layout': ready['display_layout']}
    result['runningApp'] = lab.verify_running_app(run)

    # Bind the unbound-by-default Edit & upload shortcut; route stays local.
    settings = lab.cli(run, 'settings.get')
    lab.cli(run, 'settings.patch', {'patch': {'actions': {'edit': {'destination': 'local', 'profileId': None, 'shortcut': hotkey[0]}}},
                                    'expectedRevision': settings['metadata']['observedRevision']})
    binding = wait(lambda: (lambda b: b if b.get('bound') else None)(
        lab.cli(run, 'capabilities')['source']['shortcuts']['bindings'].get('edit', {})), 15, 'edit shortcut bound')
    check('editShortcutBound', binding.get('bound') is True, binding)
    before_ids = {op['id'] for op in lab.cli(run, 'operation.recent')['operations']}
    shot('00-before')

    # Real hotkey and region drag over the whole four-colour paint.
    lab.key(run, hotkey[1])
    time.sleep(.6)
    x0, y0, x1, y1 = paint['x'], paint['y'], paint['x'] + paint['width'], paint['y'] + paint['height']
    lab.pointer(run, x0, y0, width, height, True); time.sleep(.25)
    lab.pointer(run, x1, y1, width, height); time.sleep(.25)
    shot('01-region-drag')
    lab.pointer(run, x1, y1, width, height, False)
    op_id = wait(lambda: next((op['id'] for op in lab.cli(run, 'operation.recent')['operations'] if op['id'] not in before_ids), None), 20, 'edit operation')
    result['operationId'] = op_id
    state = wait(lambda: editor_ready(op_id), 30, 'editor window')
    check('editorOpenedWithSelection', (state['width'], state['height']) == (paint['width'], paint['height']), [state['width'], state['height']])
    check('operationEditingNoItems', (lambda o: o['action'] == 'edit' and o['state'] == 'editing' and not o['items'])(lab.cli(run, 'operation.inspect', {'id': op_id})))
    time.sleep(1.0)
    park_pointer()
    open_frame = shot('02-editor-open')

    # Locate the image on screen from the independent frame (unique full pattern).
    located = lab.locate_fixture_paint(open_frame['path'])
    check('editorImageLocatedUniquely', located is not None, 'pattern missing, obscured or ambiguous')
    origin = (located['x'], located['y'])
    result['editorImageOrigin'] = located
    check('editorShowsImageAt100Percent', (located['width'], located['height']) == (640, 320), located)

    def at(ix, iy):
        return origin[0] + ix, origin[1] + iy

    # Real mouse: Redact is the default tool; drag over the green quadrant.
    sx, sy = at(340, 20); ex, ey = at(600, 140)
    lab.pointer(run, sx, sy, width, height, True); time.sleep(.15)
    lab.pointer(run, (sx + ex) // 2, (sy + ey) // 2, width, height); time.sleep(.1)
    lab.pointer(run, ex, ey, width, height); time.sleep(.15)
    lab.pointer(run, ex, ey, width, height, False); time.sleep(.4)
    state = editor('inspect', {'id': op_id})
    mouse_redactions = [s for s in state['shapes'] if s['kind'] == 'redact']
    check('mouseDrawnRedaction', len(mouse_redactions) == 1, state['shapes'])
    rx, ry, rw, rh = mouse_redactions[0]['rect']
    check('mouseRedactionGeometry', abs(rx - 340) <= 1 and abs(ry - 20) <= 1 and abs(rx + rw - 600) <= 2 and abs(ry + rh - 140) <= 2, mouse_redactions[0]['rect'])

    # Real keyboard: Text tool, click, type, Enter (must not upload), undo/redo.
    lab.key(run, ['t']); time.sleep(.3)
    tx, ty = at(20, 200)
    lab.pointer(run, tx, ty, width, height, True); lab.pointer(run, tx, ty, width, height, False); time.sleep(.4)
    for letter in 'ok':
        lab.key(run, [letter]); time.sleep(.05)
    lab.key(run, ['ret']); time.sleep(.5)
    state = editor('inspect', {'id': op_id})
    texts = [s for s in state['shapes'] if s['kind'] == 'text']
    check('keyboardText', len(texts) == 1 and texts[0]['text'] == 'ok', state['shapes'])
    check('plainEnterDidNotSubmit', state['phase'] == 'editing', state['phase'])
    lab.key(run, ['ctrl', 'z']); time.sleep(.4)
    check('keyboardUndo', not [s for s in editor('inspect', {'id': op_id})['shapes'] if s['kind'] == 'text'])
    lab.key(run, ['ctrl', 'shift', 'z']); time.sleep(.4)
    check('keyboardRedo', len([s for s in editor('inspect', {'id': op_id})['shapes'] if s['kind'] == 'text']) == 1)
    shot('03-mouse-and-keyboard-edits')

    # Agent commands through the production CLI, shown live in the window.
    applied = editor('apply', {'id': op_id, 'commands': [
        {'op': 'add', 'shape': {'kind': 'redact', 'rect': [10, 10, 60, 30]}},
        {'op': 'add', 'shape': {'kind': 'blur', 'rect': [340, 180, 120, 100], 'strength': 24}},
        {'op': 'add', 'shape': {'kind': 'arrow', 'from': [100, 300], 'to': [200, 230], 'color': '#ffffff', 'width': 6}},
        {'op': 'add', 'shape': {'kind': 'highlight', 'rect': [500, 290, 120, 20]}},
        {'op': 'add', 'shape': {'kind': 'rectangle', 'rect': [480, 175, 140, 100], 'color': '#111317', 'width': 4}}]})
    check('agentApply', len(applied['created']) == 5)
    rejected = cli_envelope('editor.apply', {'id': op_id, 'commands': [{'op': 'add', 'shape': {'kind': 'redact', 'rect': [1, 1, 5, 5]}}, {'op': 'remove', 'shapeId': 9999}]})
    check('agentAtomicRejection', not rejected['ok'] and rejected['error']['code'] == 'edit_shape_missing'
          and len(editor('inspect', {'id': op_id})['shapes']) == len(applied['state']['shapes']), rejected['error'])
    preview = editor('preview', {'id': op_id, 'maxEdge': 640})
    (out / 'agent-preview.png').write_bytes(base64.b64decode(preview['png']))
    check('agentPreview', preview['width'] == 640 and preview['sourceWidth'] == 640)
    time.sleep(.6)
    shot('04-agent-edits')
    final_state = editor('inspect', {'id': op_id})
    result['finalEditorState'] = final_state

    # Real keyboard submit: Ctrl+Enter.
    lab.key(run, ['ctrl', 'ret'])
    op = wait(lambda: (lambda o: o if o['state'] in ('completed', 'failed', 'cancelled', 'partial') and o.get('copyStatus') != 'pending' else None)(
        lab.cli(run, 'operation.inspect', {'id': op_id})), 60, 'edit operation completion')
    result['operation'] = {'state': op['state'], 'copyStatus': op.get('copyStatus'), 'items': [
        {'uploadStatus': i['uploadStatus'], 'local': i['local']['status'], 'mime': i['artifact']['mime'], 'bytes': i['artifact']['bytes'], 'sha256': i['artifact']['sha256']} for i in op['items']]}
    check('operationCompleted', op['state'] == 'completed' and len(op['items']) == 1 and op['items'][0]['local']['status'] == 'saved', result['operation'])
    item = op['items'][0]
    check('artifactIsPng', item['artifact']['mime'] == 'image/png')
    time.sleep(.8)
    shot('05-after-submit')
    lab.guest_session(run, 'putgd-desktop-fixture --convert ' + lab.shlex.quote(item['local']['path']) + ' "$HOME/putgd-lab/fixture/edit-artifact.ppm"\n')
    lab.get(run, 'putgd-lab/fixture/edit-artifact.ppm', out / 'artifact.ppm')
    aw, ah, pixels = lab.ppm(out / 'artifact.ppm')
    check('artifactSize', (aw, ah) == (640, 320), [aw, ah])
    def rgb(x, y):
        offset = (y * aw + x) * 3
        return list(pixels[offset:offset + 3])
    redactions = [s['rect'] for s in final_state['shapes'] if s['kind'] == 'redact']
    black = all(rgb(x, y) == [0, 0, 0] for (rx, ry, rw, rh) in redactions for y in range(ry, ry + rh) for x in range(rx, rx + rw))
    check('everyRedactedPixelBlack', black, redactions)
    # Independent oracle: untouched quadrant interiors keep the fixture colours.
    oracle = {'width': 640, 'height': 320, 'areas': [
        {'x': 100, 'y': 60, 'width': 200, 'height': 90, 'rgb': [220, 30, 40]},
        {'x': 330, 'y': 145, 'width': 280, 'height': 12, 'rgb': [20, 180, 70]},
        {'x': 230, 'y': 170, 'width': 80, 'height': 50, 'rgb': [35, 70, 220]},
        {'x': 330, 'y': 290, 'width': 100, 'height': 25, 'rgb': [235, 190, 20]}]}
    check('untouchedPixelsMatchFixture', lab.compare_ppm(out / 'artifact.ppm', oracle)['ok'], lab.compare_ppm(out / 'artifact.ppm', oracle))
    check('noUnredactedSourceInsideMasks', not any(rgb(x, y) in ([20, 180, 70], [220, 30, 40]) for (rx, ry, rw, rh) in redactions for y in range(ry, ry + rh, 7) for x in range(rx, rx + rw, 7)))

    # Independent paste of the local image result into the fixture.
    fx, fy = paint['x'] + 30, paint['y'] + 30
    lab.pointer(run, fx, fy, width, height, True); lab.pointer(run, fx, fy, width, height, False); time.sleep(.4)
    lab.guest_session(run, 'rm -f ~/putgd-lab/fixture/paste.json ~/putgd-lab/fixture/paste.ppm ~/putgd-lab/fixture/paste.png\n')
    lab.key(run, ['ctrl', 'v']); time.sleep(.8)
    lab.get(run, 'putgd-lab/fixture/paste.json', out / 'paste.json')
    paste = lab.read_json(out / 'paste.json')
    result['pasteMetadata'] = paste
    lab.guest_session(run, 'putgd-desktop-fixture --convert "$HOME/putgd-lab/fixture/paste.png" "$HOME/putgd-lab/fixture/paste.ppm"\n')
    lab.get(run, 'putgd-lab/fixture/paste.ppm', out / 'paste.ppm')
    check('independentPasteMatchesArtifact', lab.ppm(out / 'paste.ppm') == lab.ppm(out / 'artifact.ppm'),
          {'paste': hashlib.sha256((out / 'paste.ppm').read_bytes()).hexdigest(), 'artifact': hashlib.sha256((out / 'artifact.ppm').read_bytes()).hexdigest()})
    shot('06-paste')
    result['ok'] = True
except BaseException as error:
    result['error'] = str(error)
    try:
        shot('99-failure')
    except Exception:
        pass
    # Leave no editor open: the next run's selection would land inside it.
    try:
        if result.get('operationId') and lab.cli(run, 'operation.inspect', {'id': result['operationId']})['state'] == 'editing':
            lab.cli(run, 'editor.cancel', {'id': result['operationId']})
    except Exception:
        pass
finally:
    result['elapsedSeconds'] = round(time.monotonic() - started, 2)
    lab.write_json(out / 'results.json', result)
    print(json.dumps({k: result.get(k) for k in ('ok', 'error', 'operationId', 'elapsedSeconds')}))
    print(json.dumps({k: v['ok'] for k, v in result['checks'].items()}))
