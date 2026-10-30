"""Real-input tray/notification/shortcut recipes; never activate a D-Bus action.

Desktop node selectors live in profile fragments. AT-SPI is an observation
channel only: clicks use its live screen rectangle, never doAction(). Synthetic
clipboard data comes from the existing independent desktop fixture's key path.
The local scenarios mirror tray_progress.py's regular-file archive obstruction.
"""
from __future__ import annotations

from contextlib import contextmanager
import json
import shlex

from tools.lab.surface_driver.api import RecipeFailed, recipe


TERMINAL = {'completed', 'failed', 'partial', 'interrupted', 'cancelled'}
LABELS = {
    'region': 'Capture region', 'window': 'Capture window',
    'desktop': 'Capture desktop', 'edit': 'Capture region & edit',
    'record': 'Record screen', 'clipboard': 'Upload clipboard',
}
CHORDS = {
    'region': 'Ctrl+Shift+1', 'window': 'Ctrl+Shift+2',
    'desktop': 'Ctrl+Shift+3', 'edit': 'Ctrl+Shift+4',
    'record': 'Ctrl+Shift+5', 'clipboard': 'Ctrl+Shift+6',
}
FIXTURE_TITLE = 'put.gd independent synthetic paint / paste fixture'


def _profile(ctx):
    return ctx.profile.get('tray', {})


def _shell(ctx, script):
    result = ctx.shell(script)
    ctx.expect(result.get('returncode') == 0, 'synthetic tray setup command succeeded')
    return result.get('stdout', '').strip()


def _nodes(ctx, name, role=None):
    menu_policy = _profile(ctx).get('menu', {}) if role == 'menu item' else {}
    role = menu_policy.get('role', role)
    query = {'name': name}
    if role:
        query['role'] = role
    def on_screen(node):
        rect = node.get('rect', node.get('bounds'))
        if not rect:
            return False
        if isinstance(rect, dict):
            x, y, w, h = (rect[k] for k in ('x', 'y', 'width', 'height'))
        else:
            x, y, w, h = rect
        return (x >= 0 and y >= 0 and w > 0 and h > 0
                and not (menu_policy.get('reject_origin') and x == 0 and y == 0))
    return [n for n in ctx.accessible(query)
            if n.get('name') == name and n.get('visible', True)
            and n.get('showing', True) and on_screen(n)]


def _node(ctx, name, role=None):
    def find():
        found = _nodes(ctx, name, role)
        if len(found) > 1:
            ctx.blocked('Desktop selector needs ownership/notification scoping: ' + name, 670)
        return found[0] if found else None
    return ctx.wait(find, 'visible desktop node: ' + name)


def _point(ctx, node):
    rect = node.get('rect', node.get('bounds'))
    if not rect:
        ctx.blocked('AT-SPI node has no screen rectangle; real input cannot be targeted')
    if isinstance(rect, dict):
        x, y, w, h = (rect[k] for k in ('x', 'y', 'width', 'height'))
    else:
        x, y, w, h = rect
    ctx.expect(w > 0 and h > 0 and x >= 0 and y >= 0, 'desktop node is on screen')
    return round(x + w / 2), round(y + h / 2)


def _press(ctx, name, role=None, button='left'):
    node = _node(ctx, name, role)
    ctx.expect(node.get('enabled', True), 'desktop control is enabled: ' + name)
    ctx.click_at(*_point(ctx, node), button=button)
    return node


def _begin(ctx):
    ctx.view('ui.show', {'view': 'settings'})
    snapshot = ctx.call('ui.snapshot')
    ctx.expect(snapshot.get('operationsReady'), 'resident journal is ready')
    if not snapshot['diagnostics'].get('trayAvailable'):
        ctx.blocked('Resident reports no desktop tray host', _profile(ctx).get('fixture_issue'))
    if _profile(ctx).get('preflight_blocked'):
        ctx.blocked(_profile(ctx)['preflight_blocked'], _profile(ctx).get('fixture_issue'))
    return snapshot


def _page(ctx, name):
    def selected():
        node = _node(ctx, name, 'page tab')
        if 'selected' not in node:
            ctx.blocked('AT-SPI needs the selected state to prove page navigation', 670)
        return node['selected']
    ctx.wait(selected, 'selected main page: ' + name)


def _icon(ctx):
    selector = _profile(ctx).get('icon')
    if not selector:
        ctx.blocked('Profile has no unambiguous desktop tray icon selector')
    def find():
        nodes = _nodes(ctx, selector['name'], selector.get('role'))
        if len(nodes) > 1:
            ctx.blocked('Tray selector needs a profile role; candidate roles: ' +
                        ', '.join(sorted({n.get('role', 'unknown') for n in nodes})), 670)
        return nodes[0] if nodes else None
    try:
        return ctx.wait(find, 'desktop tray icon selector')
    except RecipeFailed:
        ctx.blocked('Desktop does not expose the configured tray icon selector through AT-SPI', 670)


def _menu(ctx):
    ctx.click_at(*_point(ctx, _icon(ctx)), button='right')
    return _node(ctx, 'History', 'menu item')


def _menu_frame(ctx, label, capture='full'):
    # Use the capture name only once the catalog lane supplies it in a profile.
    # Desktop-rendered menus have no meaningful pixel equality with Qt QMenu.
    reference = _profile(ctx).get('references', {}).get(capture)
    ctx.frame(label, reference=reference)


def _patch(ctx, patch):
    current = ctx.call('settings.get')
    return ctx.call('settings.patch', {
        'patch': patch, 'expectedRevision': current['metadata']['acceptedRevision'],
    })


def _recent(ctx):
    return ctx.call('operation.recent')['operations']


def _new_operation(ctx, previous, action):
    def find():
        matches = [o for o in _recent(ctx) if o['id'] not in previous and o['action'] == action]
        ctx.expect(len(matches) <= 1, 'one operation admitted for one real input')
        return matches[0] if matches else None
    operation = ctx.wait(find, 'new ' + action + ' operation')
    ctx.note('operation=' + operation['id'] + ' action=' + action)
    return operation


def _settled(ctx, operation):
    settled = ctx.wait(lambda: (o if (o := ctx.call('operation.inspect', {'id': operation['id']}))
                               ['state'] in TERMINAL else None), 'operation settled', timeout=30)
    ctx.note('operation=' + settled['id'] + ' state=' + settled['state'] + ' artifacts=' +
             ','.join(i.get('artifact', {}).get('id', '') for i in settled.get('items', [])))
    return settled


@contextmanager
def _local(ctx, *, failure=False, notifications=True):
    """Private synthetic local routing, restored even after a failed assertion."""
    snapshot = _begin(ctx)
    old = snapshot['settings']['document']
    ctx.expect(not snapshot['diagnostics'].get('activeOperations'), 'isolated resident has no active work')
    root = _shell(ctx, 'umask 077\nmktemp -d /tmp/pgtray.XXXXXX\n')
    ctx.expect(root.startswith('/tmp/pgtray.') and '\n' not in root, 'fresh private synthetic root')
    archive = root + '/archive'
    if failure:
        _shell(ctx, 'umask 077\nprintf synthetic > ' + shlex.quote(archive) + '\n')
    routes = {action: {'destination': 'local', 'profileId': None, 'shortcut': chord}
              for action, chord in CHORDS.items()}
    pid = None
    try:
        _patch(ctx, {'actions': routes, 'captureDefaults': {'destination': 'local', 'profileId': None},
                     'archive': {'root': archive, 'keepUploads': True,
                                 'folders': {k: None for k in ('images', 'videos', 'text', 'other')}},
                     'preferences': {'notifications': notifications}})
        pid = _shell(ctx, 'command -v putgd-desktop-fixture >/dev/null\n'
                     'putgd-desktop-fixture --output ' + shlex.quote(root + '/fixture') +
                     ' >' + shlex.quote(root + '/fixture.log') + ' 2>&1 </dev/null &\nprintf "%s\\n" "$!"\n')
        ctx.expect(pid.isdecimal() and int(pid) > 1, 'owned clipboard fixture PID')
        _node(ctx, FIXTURE_TITLE, 'frame')
        yield root
    finally:
        try:
            current = ctx.call('settings.get')
            ctx.call('settings.save', {'document': old, 'expectedRevision': current['metadata']['acceptedRevision']})
        finally:
            if pid and pid.isdecimal() and int(pid) > 1:
                # Only the exact child launched above; keep its output for evidence.
                _shell(ctx, 'kill -- ' + pid + '\n')


def _clipboard(ctx, image=False):
    _press(ctx, FIXTURE_TITLE, 'frame')
    ctx.keys('Ctrl+Alt+Shift+I' if image else 'Ctrl+Alt+Shift+T')
    if not image:
        ctx.wait(lambda: ctx.clipboard_text() == 'putgd synthetic clipboard text', 'synthetic clipboard ownership')


def _local_upload(ctx):
    _clipboard(ctx)
    previous = {o['id'] for o in _recent(ctx)}
    _menu(ctx)
    _menu_frame(ctx, 'before')
    _press(ctx, LABELS['clipboard'], 'menu item')
    return _settled(ctx, _new_operation(ctx, previous, 'clipboard'))


def _notice(ctx, summary, body=None, exclude=()):
    def find():
        matches = [n for n in ctx.notifications() if n.get('id') not in exclude
                   and n.get('summary') == summary and (body is None or body in n.get('body', ''))]
        return matches[-1] if matches else None
    return ctx.wait(find, 'desktop notification: ' + summary)


@recipe('tray.activate', desktop_drawn=True)
def activate(ctx):
    _begin(ctx)
    ctx.frame('before')
    ctx.click_at(*_point(ctx, _icon(ctx)))
    state = ctx.call('ui.history', {'action': 'inspect'})
    ctx.expect(isinstance(state, dict) and bool(state), 'History inspector answers after tray activation')
    _page(ctx, 'History')
    ctx.frame('after')


@recipe('tray.context-menu', desktop_drawn=True)
def context_menu(ctx):
    _begin(ctx)
    ctx.frame('before')
    try:
        _menu(ctx)
        for name in ('Capture region', 'History', 'Destinations', 'Settings', 'About put.gd', 'Quit put.gd'):
            _node(ctx, name, 'menu item')
        _menu_frame(ctx, 'after')
    finally:
        ctx.keys('Esc')


def _navigation(interaction, label, target):
    @recipe(interaction, desktop_drawn=True)
    def navigate(ctx):
        _begin(ctx)
        if target == 'settings':
            ctx.view('ui.show', {'view': 'history'})
        try:
            _menu(ctx)
            _menu_frame(ctx, 'before')
            _press(ctx, label, 'menu item')
            if target == 'history':
                ctx.expect(bool(ctx.call('ui.history', {'action': 'inspect'})), 'History inspector answers')
                _page(ctx, 'History')
            elif target in ('settings', 'destinations'):
                _page(ctx, target.title())
            elif target == 'capture':
                ctx.wait(lambda: ctx.call('ui.viewState').get('settings', {}).get('section') == 1,
                         'Settings Capture section')
            elif target == 'editFile':
                state = ctx.call('ui.fileDialog', {'dialog': 'editImage', 'action': 'inspect'})
                ctx.expect(state.get('open'), 'edit image file chooser is open')
            if target == 'about':
                _node(ctx, 'About put.gd', 'dialog')
            ctx.frame('after')
        finally:
            ctx.keys('Esc')
    navigate.__name__ = interaction.replace('.', '_').replace('-', '_')
    return navigate


for _id, _label, _target in (
    ('tray.menu-history', 'History', 'history'),
    ('tray.menu-settings', 'Settings', 'settings'),
    ('tray.menu-destinations', 'Destinations', 'destinations'),
    ('tray.menu-about', 'About put.gd', 'about'),
    ('tray.menu-change-output', 'Change output…', 'capture'),
):
    globals()[_id.replace('.', '_').replace('-', '_')] = _navigation(_id, _label, _target)


@recipe('tray.menu-output-submenu', desktop_drawn=True)
def output_submenu(ctx):
    with _local(ctx):
        _menu(ctx)
        _menu_frame(ctx, 'before')
        try:
            _press(ctx, 'Local only', 'menu item')
            for action, label in LABELS.items():
                node = _node(ctx, label + ': Local only', 'menu item')
                if 'enabled' not in node:
                    ctx.blocked('AT-SPI needs the enabled state to prove informational output rows are disabled', 670)
                ctx.expect(node.get('enabled') is False, 'output row is informational: ' + action)
            _menu_frame(ctx, 'after', 'output')
        finally:
            ctx.keys('Esc', 'Esc')


@recipe('tray.menu-edit-file', desktop_drawn=True)
def edit_file(ctx):
    _begin(ctx)
    _menu(ctx)
    _press(ctx, 'Edit image', 'menu item')
    _menu_frame(ctx, 'before')
    _press(ctx, 'Edit an image file…', 'menu item')
    try:
        state = ctx.call('ui.fileDialog', {'dialog': 'editImage', 'action': 'inspect'})
        ctx.expect(state.get('open'), 'real tray input opened the image chooser')
        ctx.frame('after')
    finally:
        ctx.keys('Esc')


def _action_recipe(interaction, action, shortcut=False, menu_open=False):
    @recipe(interaction, desktop_drawn=True)
    def action_recipe(ctx):
        with _local(ctx):
            if shortcut:
                status = ctx.call('diagnostics').get('shortcuts', {})
                if not status.get('available'):
                    ctx.blocked('Resident reports no global shortcut channel: ' +
                                _profile(ctx).get('shortcut_reason', 'desktop channel unavailable'),
                                _profile(ctx).get('shortcut_issue'))
                binding = status.get('bindings', {}).get(action, {})
                ctx.expect(binding.get('bound'), 'test shortcut bound: ' + action)
            if action == 'clipboard':
                _clipboard(ctx)
            previous = {o['id'] for o in _recent(ctx)}
            if not shortcut or menu_open:
                _menu(ctx)
            _menu_frame(ctx, 'before')
            if shortcut:
                ctx.keys(CHORDS[action])
            else:
                _press(ctx, LABELS[action], 'menu item')
            operation = _new_operation(ctx, previous, action)
            try:
                if action in ('region', 'edit', 'record'):
                    ctx.expect(operation['state'] not in {'failed', 'interrupted'}, 'selector opened without an error')
                    if menu_open:
                        ctx.expect(not _nodes(ctx, 'Quit put.gd', 'menu item'), 'shortcut closed tray menu')
                    ctx.frame('after')
                    ctx.keys('Esc')
                    # A real cancellation also establishes that the shortcut did
                    # not merely admit work which failed before showing a selector.
                    settled = _settled(ctx, operation)
                    ctx.expect(settled['state'] == 'cancelled', 'selector cancelled through real Escape input')
                else:
                    settled = _settled(ctx, operation)
                    ctx.expect(settled['state'] == 'completed' and settled.get('items'), 'local action completed')
                    ctx.expect(all(i.get('uploadStatus') == 'local_only' and i.get('local', {}).get('status') == 'saved'
                                   for i in settled['items']), 'every output saved locally without publication')
                    ctx.frame('after')
            finally:
                ctx.keys('Esc')
    action_recipe.__name__ = interaction.replace('.', '_').replace('-', '_')
    return action_recipe


for _action in LABELS:
    _id = 'tray.menu-' + {'edit': 'capture-region-edit', 'record': 'record',
                         'clipboard': 'upload-clipboard'}.get(_action, 'capture-' + _action)
    globals()[_id.replace('.', '_').replace('-', '_')] = _action_recipe(_id, _action)
    _id = 'shortcut.' + _action
    globals()[_id.replace('.', '_').replace('-', '_')] = _action_recipe(_id, _action, shortcut=True)

shortcut_closes_tray_menu = _action_recipe('shortcut.closes-tray-menu', 'region', shortcut=True, menu_open=True)


@recipe('tray.menu-edit-clipboard', desktop_drawn=True)
def edit_clipboard(ctx):
    with _local(ctx):
        _clipboard(ctx, image=True)
        previous = {o['id'] for o in _recent(ctx)}
        _menu(ctx)
        _press(ctx, 'Edit image', 'menu item')
        _menu_frame(ctx, 'before')
        _press(ctx, 'Edit clipboard image', 'menu item')
        operation = _new_operation(ctx, previous, 'edit')
        operation = ctx.wait(lambda: (o if (o := ctx.call('operation.inspect', {'id': operation['id']}))
                                      ['state'] == 'editing' else None), 'clipboard image editor')
        try:
            state = ctx.call('editor.inspect', {'id': operation['id']})
            ctx.expect(state.get('phase') == 'editing' and (state.get('width'), state.get('height')) == (96, 64),
                       'real-input operation opened the known synthetic 96×64 clipboard image')
            ctx.frame('after')
        finally:
            ctx.call('editor.cancel', {'id': operation['id']})


@recipe('notification.saved-locally', desktop_drawn=True)
def saved_locally(ctx):
    with _local(ctx):
        op = _local_upload(ctx)
        ctx.expect(op['state'] == 'completed' and op['items'], 'local clipboard operation completed')
        ctx.expect(all(i['local']['status'] == 'saved' and i['uploadStatus'] == 'local_only' for i in op['items']),
                   'saved-local toast agrees with operation inspection')
        _notice(ctx, 'Saved locally', 'Saved locally')
        ctx.frame('after')


@recipe('notification.local-save-failed', 'tray.icon-attention', desktop_drawn=True)
def local_failure_attention(ctx):
    with _local(ctx, failure=True) as root:
        op = _local_upload(ctx)
        ctx.expect(op['state'] == 'failed' and op['items'], 'regular file obstruction caused local save failure')
        ctx.expect(all(i['uploadStatus'] == 'local_only' and i['local']['status'] == 'failed' for i in op['items']),
                   'attention is authorized by observed local-only failure (tray_progress.py)')
        _notice(ctx, 'Local save failed')
        ctx.frame('failure-notification')
        _menu(ctx)
        _node(ctx, 'Local save needs attention', 'menu item')
        _menu_frame(ctx, 'latched-attention')
        # Repair only the obstruction created above; retry keeps original identity.
        _shell(ctx, 'rm -- ' + shlex.quote(root + '/archive') + '\nmkdir -m 700 -- ' +
               shlex.quote(root + '/archive') + '\n')
        ctx.call('operation.retryLocalSave', {'id': op['id']})
        retried = _settled(ctx, op)
        ctx.expect(retried['id'] == op['id'] and all(i['local']['status'] == 'saved' for i in retried['items']),
                   'local retry retains operation identity and saves each item')
        _node(ctx, 'Local save needs attention', 'menu item')
        _press(ctx, 'Local save needs attention', 'menu item')
        # Unlike the failed-upload branch, local-save attention goes directly
        # to Settings → Capture → Local copies via actionRecovery.
        ctx.wait(lambda: ctx.call('ui.viewState').get('settings', {}).get('section') == 1,
                 'local-save attention opens Capture settings')
        _menu(ctx)
        try:
            ctx.expect(not _nodes(ctx, 'Local save needs attention', 'menu item'), 'attention click clears the latch')
            ctx.frame('after')
            ctx.review('Compare the saved panel danger glyph/! badge with idle; AT-SPI exposes labels, not icon pixels')
        finally:
            ctx.keys('Esc')


@recipe('tray.menu-attention', desktop_drawn=True)
def attention_with_busy_latch(ctx):
    local_failure_attention(ctx)
    ctx.blocked('Local-save attention/retry branch exercised; Proton busy-latch branch needs an offline '
                'proton_account_busy fixture and original-ID recovery without a live Proton session', 670)


@recipe('notification.preference-off', desktop_drawn=True)
def notifications_off(ctx):
    with _local(ctx, notifications=False):
        # A Notify call can be observed before its method return supplies the
        # notification ID. Preserve the monitor timestamp in the identity.
        identity = lambda n: (n.get('id'), n.get('at'), n.get('summary'), n.get('body'))
        old_ids = {identity(n) for n in ctx.notifications()}
        op = _local_upload(ctx)
        ctx.expect(op['state'] == 'completed', 'operation still completes with notifications disabled')
        # Observe a bounded quiet window as well as the terminal resident state.
        _shell(ctx, "python3 -c 'import time; time.sleep(1)'\n")
        ctx.call('ui.snapshot')
        ctx.expect(not [n for n in ctx.notifications() if identity(n) not in old_ids
                        and n.get('summary') == 'Saved locally'], 'operation toast suppressed')
        ctx.frame('after')


def _save_diagnostics(ctx, root):
    folder = root + '/diagnostics'
    _shell(ctx, 'mkdir -m 700 -- ' + shlex.quote(folder) + '\n')
    _patch(ctx, {'preferences': {'diagnostics': {'packageFolder': folder}}})
    op = _local_upload(ctx)
    ctx.expect(op['state'] == 'failed', 'diagnostics target is an inspected failed operation')
    notice = _notice(ctx, 'Local save failed')
    ctx.expect('save-diagnostics' in json.dumps(notice.get('actions', [])), 'failed-operation notification offers diagnostics')
    ctx.frame('before-diagnostics-button')
    _press(ctx, 'Save diagnostics for this upload')
    saved = _notice(ctx, 'put.gd — diagnostics saved', 'Nothing was uploaded.')
    ctx.expect('show-in-folder' in json.dumps(saved.get('actions', [])), 'saved-package notification offers folder action')
    # Read only our new output directory; never collect private user logs here.
    script = ('python3 - <<\'PY\'\nimport pathlib, zipfile\np = pathlib.Path(' + repr(folder) + ')\n'
              'files = list(p.glob("putgd-diagnostics-*.zip"))\n'
              'assert len(files) == 1 and files[0].stat().st_size > 0\n'
              'with zipfile.ZipFile(files[0]) as z: assert z.testzip() is None\n'
              'print(files[0].name)\nPY\n')
    filename = _shell(ctx, script)
    ctx.note('diagnostics target operation=' + op['id'] + ' package=' + filename)
    ctx.frame('after-diagnostics-button')
    return notice, saved, filename


@recipe('notification.save-diagnostics', 'notification.diagnostics-saved', desktop_drawn=True)
def save_diagnostics(ctx):
    with _local(ctx, failure=True) as root:
        _save_diagnostics(ctx, root)


@recipe('notification.show-in-folder', desktop_drawn=True)
def show_in_folder(ctx):
    with _local(ctx, failure=True) as root:
        _, _, filename = _save_diagnostics(ctx, root)
        _press(ctx, 'Show in folder')
        _node(ctx, filename)
        ctx.frame('after')


@recipe('notification.resident-close', desktop_drawn=True)
def resident_close(ctx):
    _begin(ctx)
    if not _profile(ctx).get('resident_notifications'):
        ctx.frame('before')
        ctx.frame('after')
        ctx.blocked('This notification server retains notifications in its list and uses a finite popup timeout')
    with _local(ctx, failure=True) as root:
        _save_diagnostics(ctx, root)
        ctx.wait(lambda: not _nodes(ctx, 'Save diagnostics for this upload'), 'used resident notification closed')
        ctx.frame('after')


@recipe('notification.diagnostics-not-saved', desktop_drawn=True)
def diagnostics_not_saved(ctx):
    with _local(ctx, failure=True) as root:
        folder = root + '/read-only-diagnostics'
        _shell(ctx, 'mkdir -m 500 -- ' + shlex.quote(folder) + '\n')
        try:
            _patch(ctx, {'preferences': {'diagnostics': {'packageFolder': folder}}})
            op = _local_upload(ctx)
            ctx.expect(op['state'] == 'failed', 'failed operation exists before diagnostics click')
            _notice(ctx, 'Local save failed')
            ctx.frame('before-diagnostics-button')
            _press(ctx, 'Save diagnostics for this upload')
            _notice(ctx, 'put.gd — diagnostics not saved')
            ctx.frame('after')
        finally:
            _shell(ctx, 'chmod 700 -- ' + shlex.quote(folder) + '\n')


@recipe('tray.scroll-noop', desktop_drawn=True)
def scroll_noop(ctx):
    _begin(ctx)
    _page(ctx, 'Settings')
    before = {o['id'] for o in _recent(ctx)}
    point = _point(ctx, _icon(ctx))
    ctx.frame('before')
    ctx.click_at(*point, button='middle')
    ctx.scroll(*point, clicks=2)
    ctx.scroll(*point, clicks=-2)
    _shell(ctx, "python3 -c 'import time; time.sleep(1)'\n")
    ctx.expect({o['id'] for o in _recent(ctx)} == before, 'middle click/scroll admit no operation')
    ctx.expect(not _nodes(ctx, 'Quit put.gd', 'menu item'), 'middle click/scroll open no tray menu')
    _page(ctx, 'Settings')
    ctx.frame('after')


@recipe('tray.icon-idle', desktop_drawn=True)
def idle(ctx):
    _begin(ctx)
    _menu(ctx)
    for label in ('Local save needs attention', 'Destination needs attention',
                  'Capture needs attention', 'Action needs attention'):
        if _nodes(ctx, label, 'menu item'):
            _press(ctx, label, 'menu item')
            break
    ctx.keys('Esc')
    ctx.view('ui.show', {'view': 'settings'})
    ctx.frame('before')
    ctx.expect(all(o['state'] in TERMINAL for o in _recent(ctx)), 'resident has no active operation')
    _icon(ctx)
    _menu(ctx)
    try:
        ctx.expect(not _nodes(ctx, 'Cancel upload', 'menu item'), 'idle menu has no cancel-upload row')
        ctx.expect(not any(_nodes(ctx, label, 'menu item') for label in (
            'Local save needs attention', 'Destination needs attention',
            'Capture needs attention', 'Action needs attention')), 'idle menu has no attention latch')
        ctx.frame('after')
        ctx.review('Saved panel frame needs glyph/tint review; tooltip hover and SNI pixel observation are not in Context')
    finally:
        ctx.keys('Esc')


# Explicit declarations are independent rows, so unavailable scenarios never
# silently disappear from a lane run. Issue numbers are filled by the follow-up
# tracker; no provider session is borrowed to cover an offline-only campaign.
BLOCKED = {
    'tray.tooltip': 'Needs real pointer hover plus read-only tooltip observation; Context exposes clicks but no hover',
    'tray.icon-uploading': 'Needs an offline provider fixture with controlled storage progress lasting over 200 ms',
    'tray.icon-complete': 'Needs an offline successful upload and sub-600-ms icon sampling; local saves have no upload fill',
    'tray.icon-recording': 'Needs a recording fixture and icon-pixel observation for active-dot and paused-bars evidence',
    'tray.menu-upload-status': 'Needs an offline controlled upload fixture for real status/progress menu rows',
    'tray.menu-cancel-upload': 'Needs offline one/two active uploads to exercise both immediate cancel and confirmation',
    'tray.menu-update-available': 'Needs a stable-tagged local build with isolated update-result fixture; no live update query',
    'tray.menu-quit': 'Needs core resident restart ownership after real Quit; do not terminate the shared run mid-lane',
    'tray.menu-shortcut-suffix': 'Needs AT-SPI accelerator metadata or read-only menu snapshot tied to current shortcut bindings',
    'tray.qt-fallback': 'Needs a graphical-resident launch profile; the production Qt Core resident uses NativeTray',
    'shortcut.over-open-menu': 'Needs an independently owned other-client popup fixture with keyboard-grab witness',
    'shortcut.ignored-while-recording': 'Needs current-build shortcut recorder controls plus a bounded no-operation observation window',
    'shortcut.paused-while-settings-records': 'Needs native shortcut-grab observation during recorder focus and restoration',
    'shortcut.locked-session': 'Needs core-owned visible session lock/unlock lifecycle and protected login handling',
    'shortcut.layout-change': 'Needs isolated keyboard-layout change/restore and native rebinding witness',
    'notification.upload-complete': 'Needs an offline successful upload fixture; local-only completion is Saved locally',
    'notification.completed-with-problem': 'Needs offline mixed-success batch fixture with inspected per-item outcomes',
    'notification.upload-failed': 'Needs offline failed/interrupted upload fixture with retained local-copy variants',
    'notification.action-rejected': 'Needs deterministic pre-admission rejection fixture without configuration recovery stealing popup focus',
    'notification.recording-stopped': 'Needs controlled recording byte-budget fixture to trigger resident budgetStopped signal',
    'notification.incomplete-uploads': 'Needs retained incomplete-upload journal fixture to trigger cleanup notice safely',
    'notification.in-flight-limit': 'Needs a delayed-reply notification server fixture; eight outstanding Notify calls are not eight visible toasts',
}


def _blocked_recipe(interaction, reason):
    @recipe(interaction, desktop_drawn=True)
    def unavailable(ctx):
        _begin(ctx)
        ctx.frame('before')
        ctx.note(reason)
        ctx.frame('after')
        ctx.blocked(reason, _profile(ctx).get('fixture_issue'))
    unavailable.__name__ = interaction.replace('.', '_').replace('-', '_')
    return unavailable


for _id, _reason in BLOCKED.items():
    globals()[_id.replace('.', '_').replace('-', '_')] = _blocked_recipe(_id, _reason)
