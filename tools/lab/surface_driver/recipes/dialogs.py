"""Real chooser input, with independent portal and fallback recipe variants.

Backend fixtures launch a fresh private resident/helper family while preserving
native desktop buses. The production chooser-failure fixture deliberately blocks
until a real isolated Failed-result boundary exists (#671).

Shell results use Context.shell's {returncode, stdout, stderr} contract. Call,
view and MCP results are resident result objects, without transport envelopes.
Fallback catalog captures are requested through the profile; absent captures
are explicitly sent to review. Never compare a portal frame to a Qt catalog.
"""
from __future__ import annotations

import json
import re
import shlex

from tools.lab.surface_driver.api import recipe


BACKENDS = ('portal', 'fallback')
SCENARIOS = (
    'edit-image', 'history-save-as', 'output-folder', 'diagnostics-folder',
    'editor-save-copy', 'review-save-copy', 'portal', 'fallback', 'cancel',
    'failure-strip', 'mcp.file-dialogs',
)


def _shell(ctx, script):
    result = ctx.shell(script)
    ctx.expect(result['returncode'] == 0, 'synthetic file witness succeeded')
    return result['stdout'].strip()


def _files(ctx):
    # New paths avoid overwrite prompts and never touch user objects. The run
    # retains these synthetic artifacts for collection, even after a failure.
    return json.loads(_shell(ctx, """python3 - <<'PY'
import json, pathlib, struct, tempfile, zlib
root = pathlib.Path(tempfile.mkdtemp(prefix='pgfd.', dir='/tmp'))
(root / 'Chosen folder').mkdir(mode=0o700)
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
png = b'\\x89PNG\\r\\n\\x1a\\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 24, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress((b'\\0' + b'\\x32\\x96\\xc8' * 32) * 24)) + chunk(b'IEND', b'')
(root / 'Synthetic image.png').write_bytes(png)
print(json.dumps({'root': str(root), 'image': str(root / 'Synthetic image.png'), 'folder': str(root / 'Chosen folder')}))
PY"""))


def _backend(ctx, backend):
    fixture = ctx.fixture('file-dialog-' + backend)
    if not fixture['available']:
        ctx.blocked(backend + ' chooser unavailable: ' + fixture['reason'])
    settings = ctx.profile['dialogs']['backends'][backend]
    if not settings['available']:
        ctx.blocked(settings['reason'])
    ctx.note('Requested chooser backend: ' + backend)
    return settings


def _frame(ctx, label, backend, mode=None):
    reference = None
    if backend == 'fallback' and mode:
        reference = ctx.profile['dialogs']['backends'][backend]['references'].get(mode)
        if not reference:
            ctx.review('No current fallback ' + mode + ' chooser catalog capture is registered')
    # Desktop frames include portal windows even when they cover their owner.
    # A fallback reference must crop the chooser, never the owning main window.
    ctx.frame(label, window='putgdFileDialog' if reference else None, reference=reference)


def _ready(ctx, inspect, backend, mode):
    state = ctx.wait(lambda: (s if s.get('open') and s.get('backend') else None)
                     if (s := inspect()) else None, 'chooser prepared', timeout=12)
    if state['backend'] != backend:
        ctx.blocked('Requested ' + backend + ', got ' + state['backend'] +
                    '; fallbackReason=' + str(state.get('fallbackReason')))
    if backend == 'fallback':
        expected = ctx.profile['dialogs']['backends'][backend]['expected_reason']
        if state.get('fallbackReason') != expected:
            ctx.blocked('Fallback launch did not apply the profile environment; reason=' +
                        str(state.get('fallbackReason')), issue=671)
    ctx.expect(state['mode'] == mode, 'chooser has the requested mode')
    if backend == 'portal':
        def drawn():
            current = inspect()
            if current.get('backend') != backend:
                ctx.blocked('Portal changed backend before becoming ready; fallbackReason=' +
                            str(current.get('fallbackReason')))
            return ctx.accessible({'name': current['title']})
        nodes = ctx.wait(drawn, 'desktop chooser is accessible and drawn')
        # Desktop-owned portal windows are outside the private app family.
        # Bind the input guard to the observed chooser title and process, never
        # the obscured Settings/History owner behind the modal window.
        top = [node for node in nodes if node.get('role') in ('dialog', 'file chooser', 'frame')]
        if len(top) != 1:
            ctx.blocked('Portal chooser top-level AT-SPI identity is missing or ambiguous', issue=671)
        ctx.profile.setdefault('windows', {})['portalFileDialog'] = {
            'title': '^' + re.escape(state['title']) + '$', 'owned': False,
            'accessible': {'name': state['title'], 'pid': top[0]['pid'], 'owned': False},
        }
        ctx.focus('portalFileDialog', raise_window=False)
    else:
        ctx.focus('putgdFileDialog', raise_window=False)
    _frame(ctx, 'dialog-ready', backend, mode)
    return state


def _input(ctx, backend, path, mode, inspect):
    if backend == 'fallback':
        ctx.click('fileNameEdit', window='putgdFileDialog')
        ctx.keys('Ctrl+A')
    else:
        settings = ctx.profile['dialogs']['backends'][backend]
        ctx.keys(*settings['location_keys'])
    ctx.type_text(path)
    if backend == 'portal':
        # A title-only AT-SPI witness is insufficient: inspect the typed path
        # before Return can navigate, accept, or trigger an overwrite prompt.
        def contains_path():
            identity = ctx.profile['windows']['portalFileDialog']['accessible']
            nodes = ctx.accessible({'text': path, 'window': identity['name'],
                                    'pid': identity['pid'], 'visible': True, 'showing': True})
            matches = [node for node in nodes if path in
                       (node.get('text'), node.get('value'))]
            if nodes and not matches and not any('text' in node or 'value' in node for node in nodes):
                ctx.blocked('AT-SPI probe exposes names only; cannot prove typed chooser path', issue=671)
            return matches
        ctx.wait(contains_path, 'desktop chooser contains the typed path')
    ctx.keys('Return')
    if backend == 'portal' and mode == 'folder' and inspect()['open']:
        ctx.keys(*ctx.profile['dialogs']['backends'][backend]['folder_accept_keys'])


def _exercise(ctx, backend, mode, request, consumer, path, *, mcp=False):
    def command(action):
        method, params = request
        if mcp:
            envelope = ctx.mcp('drive_file_dialog', dict(params, action=action))
            ctx.expect(envelope.get('ok') is True, 'MCP file-dialog request succeeded')
            return envelope['result']
        return ctx.call(method, dict(params, action=action))

    before = consumer()
    _frame(ctx, 'before', backend)
    command('open')
    try:
        _ready(ctx, lambda: command('inspect'), backend, mode)
        ctx.keys('Escape')
        cancelled = ctx.wait(lambda: (s if not s['open'] else None)
                             if (s := command('inspect')) else None, 'chooser cancelled')
        ctx.expect(cancelled['last']['status'] == 'cancelled', 'Escape reports cancellation')
        ctx.expect(consumer() == before, 'cancel leaves the owning surface unchanged')
        _frame(ctx, 'after-cancel', backend)
        if mcp:
            command('cancel')
        command('open')
        _ready(ctx, lambda: command('inspect'), backend, mode)
        _input(ctx, backend, path, mode, lambda: command('inspect'))
        chosen = ctx.wait(lambda: (s if not s['open'] else None)
                          if (s := command('inspect')) else None, 'chooser accepted')
        ctx.expect(chosen['last']['status'] == 'chosen', 'resident reports a choice')
        ctx.expect(chosen['last']['backend'] == backend, 'resident confirms the actual backend')
        ctx.expect(chosen['last']['path'] == path, 'resident received the exact typed path')
    except Exception:
        _frame(ctx, 'dialog-failure', backend)
        raise
    finally:
        # Review disables mutating controls during export. Do not send a
        # redundant cancel after a successful choice has started that export.
        if command('inspect')['open']:
            command('cancel')


def _settings(ctx):
    return ctx.call('settings.get')['document']


def _ui_request(dialog, **params):
    return 'ui.fileDialog', dict(dialog=dialog, **params)


def _editor_ready(ctx, identity):
    def inspect():
        result = ctx.cli('editor.inspect', json.dumps({'id': identity}))
        response = json.loads(result['stdout'])
        return response.get('result') if response.get('ok') else None
    return ctx.wait(inspect, 'editor helper is ready', timeout=30)


def _output(ctx, backend, files, *, mcp=False, cancel_only=False):
    ctx.view('ui.show', {'view': 'settings'})
    fields = ('root',) if mcp or cancel_only else ('root', 'images', 'videos', 'text', 'other')
    for field in fields:
        if cancel_only:
            before = _settings(ctx)
            request = _ui_request('outputFolder', field=field)
            _frame(ctx, 'before', backend)
            ctx.call(request[0], dict(request[1], action='open'))
            try:
                _ready(ctx, lambda: ctx.call(request[0], dict(request[1], action='inspect')), backend, 'folder')
                ctx.keys('Escape')
                state = ctx.wait(lambda: (s if not s['open'] else None)
                                 if (s := ctx.call(request[0], dict(request[1], action='inspect'))) else None,
                                 'folder chooser cancelled')
                ctx.expect(state['last']['status'] == 'cancelled', 'resident reports cancellation')
                ctx.expect(_settings(ctx) == before, 'cancel preserves accepted settings')
            finally:
                ctx.call(request[0], dict(request[1], action='cancel'))
        else:
            _exercise(ctx, backend, 'folder', _ui_request('outputFolder', field=field),
                      lambda: _settings(ctx), files['folder'], mcp=mcp)
            def accepted():
                archive = _settings(ctx)['archive']
                value = archive['root'] if field == 'root' else archive['folders'][field]
                return value == files['folder']
            ctx.wait(accepted, 'resident autosaved chosen ' + field + ' folder')
        _frame(ctx, 'after-' + field, backend)


def _run(ctx, backend, scenario):
    if scenario == 'failure-strip':
        ctx.fixture('chooser-failure')
        ctx.blocked('Chooser failure fixture did not provide a production Failed result', issue=671)
    _backend(ctx, backend)
    files = _files(ctx)
    if scenario in ('output-folder', 'portal', 'fallback', 'cancel', 'mcp.file-dialogs'):
        _output(ctx, backend, files, mcp=scenario == 'mcp.file-dialogs', cancel_only=scenario == 'cancel')
    elif scenario == 'diagnostics-folder':
        ctx.view('ui.show', {'view': 'settings'})
        ctx.call('ui.diagnostics', {'action': 'openPackage', 'includeNames': False})
        ctx.call('ui.diagnostics', {'action': 'setPackage', 'upload': False})
        def consumer():
            return ctx.call('ui.diagnostics', {'action': 'inspect'})['package']['folder']
        try:
            _exercise(ctx, backend, 'folder', _ui_request('diagnosticsFolder'), consumer, files['folder'])
            ctx.wait(lambda: consumer() == files['folder'], 'diagnostics package uses chosen folder')
            _frame(ctx, 'after', backend)
        finally:
            ctx.call('ui.diagnostics', {'action': 'closePackage'})
    elif scenario == 'history-save-as':
        fixture = ctx.fixture('history')
        if not fixture.path:
            ctx.blocked('Core history fixture did not retain its original file path', issue=671)
        ctx.view('ui.show', {'view': 'history'})
        ctx.wait(lambda: not ctx.call('ui.history', {'action': 'inspect'})['busy'], 'history loaded')
        ctx.call('ui.history', {'action': 'select', 'ids': [fixture.history_id]})
        path = files['root'] + '/History copy.png'
        _exercise(ctx, backend, 'save', _ui_request('historySaveAs', id=fixture.history_id),
                  lambda: ctx.call('ui.history', {'action': 'inspect'})['savedPath'], path)
        ctx.wait(lambda: ctx.call('ui.history', {'action': 'inspect'}).get('copyJob', {}).get('status') == 'saved',
                 'resident finished saving original')
        ctx.expect(ctx.call('ui.history', {'action': 'inspect'})['savedPath'] == path, 'history saved path')
        _shell(ctx, 'cmp -- ' + shlex.quote(fixture.path) + ' ' + shlex.quote(path))
        _frame(ctx, 'after', backend)
    elif scenario == 'editor-save-copy':
        op = ctx.call('editor.open', {'source': 'file', 'path': files['image']})['operationId']
        try:
            _editor_ready(ctx, op)
            ctx.view('editor.view', {'id': op, 'action': 'fit'})
            path = files['root'] + '/Edited copy.png'
            def consumer():
                state = ctx.call('editor.inspect', {'id': op})
                return {'document': {k: v for k, v in state.items() if k != 'view'},
                        'lastSave': state['view']['lastSave']}
            _exercise(ctx, backend, 'save', ('editor.fileDialog', {'id': op}), consumer, path)
            ctx.wait(lambda: (consumer()['lastSave'] or {}).get('status') == 'saved', 'resident finished saving edit')
            ctx.expect(consumer()['lastSave']['path'] == path, 'editor saved path')
            _png(ctx, path)
            _frame(ctx, 'after', backend)
        finally:
            ctx.call('editor.cancel', {'id': op})
    elif scenario == 'review-save-copy':
        fixture = ctx.fixture('recording')
        op = fixture.id
        ctx.view('recording.review', {'id': op, 'action': 'inspect'})
        path = files['root'] + '/Recording copy.mp4'
        def consumer():
            state = ctx.call('recording.review', {'id': op, 'action': 'inspect'})
            return {'savedCopy': state['savedCopy'], 'edit': state['edit']}
        # Review exposes its chooser under its own surface method, not ui.fileDialog.
        class ReviewContext:
            def __getattr__(self, name):
                return getattr(ctx, name)
            def call(self, method, params=None):
                if method == 'recording.review.dialog':
                    actions = {'open': 'saveCopyDialog', 'inspect': 'inspect', 'cancel': 'closeFileDialog'}
                    return ctx.call('recording.review', {'id': op, 'action': actions[params['action']]})['fileDialog']
                return ctx.call(method, params)
        _exercise(ReviewContext(), backend, 'save', ('recording.review.dialog', {}), consumer, path)
        ctx.wait(lambda: consumer()['savedCopy']['path'] == path, 'review reports saved copy')
        _shell(ctx, 'test -s ' + shlex.quote(path))
        _frame(ctx, 'after', backend)
    elif scenario == 'edit-image':
        ctx.view('ui.show', {'view': 'history'})
        before_ids = {op['id'] for op in ctx.call('operation.recent')['operations']}
        _exercise(ctx, backend, 'open', _ui_request('editImage'),
                  lambda: {op['id'] for op in ctx.call('operation.recent')['operations']}, files['image'])
        def opened():
            return next((op for op in ctx.call('operation.recent')['operations']
                         if op['id'] not in before_ids and op['action'] == 'edit'), None)
        op = ctx.wait(opened, 'chosen image created an edit operation')['id']
        try:
            state = _editor_ready(ctx, op)
            ctx.expect(state['phase'] == 'editing', 'selected image is open for editing')
            _frame(ctx, 'after', backend)
        finally:
            ctx.call('editor.cancel', {'id': op})


def _png(ctx, path):
    script = "import pathlib,struct; p=pathlib.Path(" + repr(path) + ").read_bytes(); "
    script += "assert p[:8] == b'\\x89PNG\\r\\n\\x1a\\n'; assert struct.unpack('>II', p[16:24]) == (32,24)"
    _shell(ctx, 'python3 -c ' + shlex.quote(script))


def _register(scenario, backend):
    needs = ['chooser-failure'] if scenario == 'failure-strip' else ['file-dialog-' + backend]
    if scenario == 'history-save-as':
        needs.append('history')
    if scenario == 'review-save-copy':
        needs.append('recording')
    interaction = scenario if scenario.startswith('mcp.') else 'dialog.' + scenario

    @recipe(interaction, needs=needs, destinations=('local',), desktop_drawn=backend == 'portal', timeout=300)
    def run(ctx):
        _run(ctx, backend, scenario)
    run.__name__ = scenario.replace('.', '_').replace('-', '_') + '_' + backend
    return run


for _scenario in SCENARIOS:
    for _variant in BACKENDS:
        globals()[_scenario.replace('.', '_').replace('-', '_') + '_' + _variant] = _register(_scenario, _variant)
