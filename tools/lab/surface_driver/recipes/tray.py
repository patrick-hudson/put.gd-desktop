"""Real-input tray/notification/shortcut recipes; never activate a D-Bus action.

Desktop node selectors live in profile fragments. AT-SPI is an observation
channel only: clicks use its live screen rectangle, never doAction().
Synthetic clipboard owners and settings rollback come from core fixtures.
The local scenarios mirror tray_progress.py's regular-file archive obstruction.
"""
from __future__ import annotations

from contextlib import contextmanager
import json
import glob
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


def _frame(ctx, label, **options):
    if ctx.destination == 'local' and ctx.placement in ('boat', 'guest') and ctx.profile.get('guest'):
        options['desktop_safe'] = 'Disposable guest; only synthetic fixture content and loopback publications; no live account or credentials'
    return ctx.frame(label, **options)


def _focus(ctx, window):
    return ctx.focus(window, raise_window=ctx.profile.get('session') != 'wayland')


def _profile(ctx):
    return ctx.profile.get('tray', {})


def _shell(ctx, script):
    result = ctx.shell(script)
    ctx.expect(result.get('returncode') == 0, 'synthetic tray setup command succeeded')
    return result.get('stdout', '').strip()


def _menu_parent_state(ctx, node):
    parents = [a for a in node.get('ancestors', []) if a.get('role') == 'menu item']
    if not parents or not node.get('pid'):
        ctx.blocked('Named shell menu label lacks its live owned menu-item parent', 670)
    rect = parents[-1].get('rect')
    if not isinstance(rect, (list, tuple)) or len(rect) != 4 or min(rect[2:]) <= 0:
        ctx.blocked('Shell menu parent has no positive live rectangle', 670)
    rows = ctx.accessible({'name': '*', 'role': 'menu item', 'pid': node['pid'],
                           'application': _profile(ctx)['icon']['application']})
    rows = [r for r in rows if r.get('rect') == rect and r.get('pid') == node['pid']
            and r.get('visible') and r.get('showing')]
    if len(rows) != 1 or 'enabled' not in rows[0]:
        ctx.blocked('Shell menu parent enabled state is missing or ambiguous', 670)
    return dict(node, enabled=rows[0]['enabled'])


def _nodes(ctx, name, role=None):
    is_menu = role == 'menu item'
    menu_policy = _profile(ctx).get('menu', {}) if is_menu else {}
    role = menu_policy.get('role', role)
    query = {'name': name}
    if role:
        query['role'] = role
    if is_menu:
        query['application'] = _profile(ctx)['icon']['application']
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
    nodes = [n for n in ctx.accessible(query)
             if n.get('name') == name and n.get('visible', True)
             and n.get('showing', True) and on_screen(n)]
    if menu_policy.get('enabled_from_parent'):
        nodes = [_menu_parent_state(ctx, n) for n in nodes]
    return nodes


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


# Stable catalog row IDs from #711/#738. Desktop menus are still resolved
# through live shell nodes; catalog rectangles never authorize desktop input.
CONTROLS = {
    'trayOutputSummary': 'Local only', 'trayChangeOutput': 'Change output…',
    **LABELS, **{'trayOutput' + a: label + ': Local only' for a, label in LABELS.items()},
    'trayCancelUpload': 'Cancel upload', 'edit-sources': 'Edit image', 'trayMenuAction-3deabb1a41ce13d8': 'Edit clipboard image',
    'trayMenuAction-672f6057ec40f3b4': 'Edit an image file…', 'history': 'History',
    'destinations': 'Destinations', 'preferences': 'Settings',
    'trayMenuAction-362db4ee39b59aea': 'About put.gd',
    'trayMenuAction-33617f924b4aa7fb': 'Quit put.gd',
}


def _press(ctx, name, role=None, button='left'):
    name = CONTROLS.get(name, name)
    node = _node(ctx, name, role)
    ctx.expect(node.get('enabled', True), 'desktop control is enabled: ' + name)
    # Preserve shell/application ownership through the independent hit-test.
    query = {'name': name, 'role': node['role']}
    if node.get('pid'):
        query['pid'] = node['pid']
    elif node.get('application'):
        query['application'] = node['application']
    else:
        ctx.blocked('Desktop node lacks an ownership identity: ' + name, 670)
    if _profile(ctx).get('hover_before_click'):
        ctx.hover_accessible(query)
        _shell(ctx, "python3 -c 'import time; time.sleep(.25)'\n")
    ctx.click_accessible(query, button=button)
    return node


def _ready(ctx):
    return ctx.wait(lambda: (s if (s := ctx.call('ui.snapshot')).get('operationsReady') else None),
                    'resident journal is ready', timeout=30)


def _begin(ctx, *, show_ui=True):
    _ready(ctx)
    ctx.fixture('app-profile')
    if show_ui:
        ctx.view('ui.show', {'view': 'settings'})
    snapshot = _ready(ctx)
    if not snapshot['diagnostics'].get('trayAvailable'):
        ctx.blocked('Resident reports no desktop tray host', 670)
    return snapshot


def _page(ctx, name):
    def selected():
        node = _node(ctx, name, 'page tab')
        states = node.get('states', {})
        if 'selected' in node:
            return node['selected']
        if 'selected' not in states and 'checked' not in states:
            ctx.blocked('AT-SPI needs the selected state to prove page navigation', 670)
        return bool(states.get('selected') or states.get('checked'))
    ctx.wait(selected, 'selected main page: ' + name)


def _icon(ctx):
    selector = _profile(ctx).get('icon')
    if not selector:
        ctx.blocked('Profile has no owned desktop tray icon selector', 670)
    nodes = ctx.accessible(selector)
    nodes = [n for n in nodes if n.get('visible') and n.get('showing')
             and n.get('rect') and (min(n['rect'][k] for k in ('width', 'height'))
                  if isinstance(n['rect'], dict) else min(n['rect'][2:])) > 0]
    if len(nodes) != 1:
        ctx.blocked('Desktop tray icon selector is missing or ambiguous; needs an owned panel rectangle', 670)
    return nodes[0]


def _icon_input(ctx, button='left'):
    _icon(ctx)
    if _profile(ctx).get('hover_before_click'):
        ctx.hover_accessible(_profile(ctx)['icon'])
        _shell(ctx, "python3 -c 'import time; time.sleep(.25)'\n")
    ctx.click_accessible(_profile(ctx)['icon'], button=button)


def _menu(ctx):
    _icon_input(ctx, 'right')
    return _node(ctx, 'History', 'menu item')


def _dismiss_menu(ctx, *, depth=1):
    if _profile(ctx).get('menu_prepare_background'):
        _icon_input(ctx, 'right')
    else:
        ctx.keys(*(['Esc'] * depth))


def _menu_frame(ctx, label, capture='full'):
    # Keep the full desktop, including the shell menu and panel icon. Passing
    # reference= would crop the current Qt window and hide the interaction.
    # Catalog identity is a review hint, never a desktop pixel comparator.
    reference = _profile(ctx).get('references', {}).get(capture)
    _frame(ctx, label)
    if reference:
        ctx.note('desktop catalog reference=' + reference)


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
def _local(ctx, *, failure=False, notifications=True, notification_only=False, desktop_menu=False):
    """Core owns profile restoration and helper cleanup, including timeouts."""
    _begin(ctx, show_ui=not ((notification_only and _profile(ctx).get('notification_prepare_cli'))
                             or (desktop_menu and _profile(ctx).get('menu_prepare_background'))))
    profile = ctx.fixture('app-profile')
    root = profile.root
    archive = profile.output_dir
    if failure:
        archive = root + '/tray-blocked'
        _shell(ctx, 'umask 077\nprintf synthetic > ' + shlex.quote(archive) + '\n')
    routes = {action: {'destination': 'local', 'profileId': None, 'shortcut': chord}
              for action, chord in CHORDS.items() if action != 'record'}
    routes['record'] = {'shortcut': CHORDS['record']}
    _patch(ctx, {'actions': routes, 'captureDefaults': {'destination': 'local', 'profileId': None,
                                     'publicationMode': 'default'},
                 'archive': {'root': archive, 'keepUploads': True,
                             'folders': {k: None for k in ('images', 'videos', 'text', 'other')}},
                 'preferences': {'notifications': notifications}})
    yield root


def _clipboard(ctx, image=False):
    if image:
        ctx.blocked('Core has no native image clipboard owner descriptor; text fixture cannot prove image editing', 670)
    fixture = ctx.fixture('events.clipboard')
    ctx.wait(lambda: ctx.clipboard_text() == fixture.text, 'synthetic clipboard ownership')


def _local_input(ctx, *, notification_only=False):
    _clipboard(ctx)
    previous = {o['id'] for o in _recent(ctx)}
    if notification_only and _profile(ctx).get('notification_prepare_cli'):
        # State preparation only. Notification actions still require a drawn
        # banner, guarded real input and exact ActionInvoked correlation.
        ctx.call('action.invoke', {'action': 'clipboard'})
        return previous
    # Emit the toast through real input without spending its short draw window
    # navigating or framing a tray menu. The native binding is checked first.
    binding = ctx.call('ui.snapshot')['diagnostics'].get('shortcuts', {}).get('bindings', {}).get('clipboard', {})
    ctx.expect(binding.get('bound'), 'clipboard shortcut is natively bound')
    ctx.keys(CHORDS['clipboard'])
    return previous


def _local_upload(ctx, *, notification_only=False):
    return _settled(ctx, _new_operation(ctx, _local_input(ctx, notification_only=notification_only), 'clipboard'))


def _notice(ctx, summary, body=None, exclude=()):
    def find():
        matches = [n for n in ctx.notifications() if isinstance(n.get('id'), int) and n['id'] not in exclude
                   and n.get('summary') == summary and (body is None or body in n.get('body', ''))]
        return matches[-1] if matches else None
    return ctx.wait(find, 'desktop notification: ' + summary)


@recipe('tray.activate', desktop_drawn=True)
def activate(ctx):
    _begin(ctx)
    _frame(ctx, 'before')
    if _profile(ctx).get('activation_clicks', 1) == 2:
        _icon(ctx)
        ctx.double_click_accessible(_profile(ctx)['icon'])
    else:
        _icon_input(ctx)
    state = ctx.call('ui.history', {'action': 'inspect'})
    ctx.expect(isinstance(state, dict) and bool(state), 'History inspector answers after tray activation')
    _page(ctx, 'History')
    _frame(ctx, 'after')


@recipe('tray.context-menu', desktop_drawn=True)
def context_menu(ctx):
    background = _profile(ctx).get('menu_prepare_background', False)
    _begin(ctx, show_ui=not background)
    _frame(ctx, 'before')
    opened = False
    try:
        _menu(ctx)
        opened = True
        for name in ('Capture region', 'History', 'Destinations', 'Settings', 'About put.gd', 'Quit put.gd'):
            _node(ctx, name, 'menu item')
        _menu_frame(ctx, 'after')
    finally:
        if opened:
            if background:
                _icon_input(ctx, 'right')
            else:
                ctx.keys('Esc')


def _navigation(interaction, label, target):
    @recipe(interaction, desktop_drawn=True)
    def navigate(ctx):
        _begin(ctx, show_ui=not _profile(ctx).get('menu_prepare_background'))
        if target == 'settings':
            ctx.view('ui.show', {'view': 'history'})
        about_focused = False
        try:
            _menu(ctx)
            _menu_frame(ctx, 'before')
            _press(ctx, next((k for k,v in CONTROLS.items() if v == label), label), 'menu item')
            if target == 'history':
                ctx.expect(bool(ctx.call('ui.history', {'action': 'inspect'})), 'History inspector answers')
                _page(ctx, 'History')
            elif target in ('settings', 'destinations'):
                _page(ctx, target.title())
            elif target == 'capture':
                _page(ctx, 'Settings')
                ctx.wait(lambda: ctx.call('ui.viewState').get('settings', {}).get('section') == 1,
                         'Settings Capture section')
            elif target == 'editFile':
                state = ctx.call('ui.fileDialog', {'dialog': 'editImage', 'action': 'inspect'})
                ctx.expect(state.get('open'), 'edit image file chooser is open')
            if target == 'about':
                _node(ctx, 'About put.gd', 'dialog')
                _focus(ctx, 'trayAbout')
                about_focused = True
            _frame(ctx, 'after')
        finally:
            if about_focused:
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
    with _local(ctx, desktop_menu=True):
        _menu(ctx)
        _menu_frame(ctx, 'before')
        try:
            _press(ctx, 'trayOutputSummary', 'menu item')
            for action, label in LABELS.items():
                node = _node(ctx, CONTROLS['trayOutput' + action], 'menu item')
                if 'enabled' not in node:
                    ctx.blocked('AT-SPI needs the enabled state to prove informational output rows are disabled', 670)
                ctx.expect(node.get('enabled') is False, 'output row is informational: ' + action)
            _menu_frame(ctx, 'after', 'output')
        finally:
            _dismiss_menu(ctx, depth=2)


@recipe('tray.menu-edit-file', desktop_drawn=True)
def edit_file(ctx):
    _begin(ctx, show_ui=not _profile(ctx).get('menu_prepare_background'))
    _menu(ctx)
    _press(ctx, 'edit-sources', 'menu item')
    _menu_frame(ctx, 'before')
    _press(ctx, 'trayMenuAction-672f6057ec40f3b4', 'menu item')
    focused = False
    try:
        state = ctx.call('ui.fileDialog', {'dialog': 'editImage', 'action': 'inspect'})
        ctx.expect(state.get('open'), 'real tray input opened the image chooser')
        _focus(ctx, 'fileDialog')
        focused = True
        _frame(ctx, 'after')
    finally:
        if focused:
            ctx.keys('Esc')


def _action_recipe(interaction, action, shortcut=False, menu_open=False):
    @recipe(interaction, desktop_drawn=True, timeout=300 if action == 'record' and shortcut else 120)
    def action_recipe(ctx):
        with _local(ctx, desktop_menu=not shortcut):
            if shortcut:
                diagnostics = ctx.call('ui.snapshot')['diagnostics']
                status = diagnostics.get('shortcuts', {})
                if not status.get('available'):
                    ctx.blocked('Resident reports no global shortcut channel: ' +
                                _profile(ctx).get('shortcut_reason', 'desktop channel unavailable'),
                                _profile(ctx).get('shortcut_issue'))
                binding = status.get('bindings', {}).get(action, {})
                ctx.note('native shortcut channel=' + str(status.get('channel')) + ' binding=' + json.dumps(binding, sort_keys=True))
                if not binding.get('bound'):
                    helper = diagnostics.get('capabilities', {}).get('source', {}).get('helper', {})
                    ctx.note('native binding=' + json.dumps(binding))
                    if helper.get('outdated'):
                        ctx.blocked('Guest GNOME helper is outdated (' + str(helper.get('extensionVersion')) +
                                    ' vs packaged ' + str(helper.get('packagedExtensionVersion')) +
                                    ') and omits binding: ' + action, 670)
                ctx.expect(binding.get('bound'), 'test shortcut bound: ' + action)
            if action == 'window':
                ctx.fixture('selector-desktop')
                _focus(ctx, 'trayFixture')
            if action == 'clipboard':
                _clipboard(ctx)
            previous = {o['id'] for o in _recent(ctx)}
            if not shortcut or menu_open:
                _menu(ctx)
            _menu_frame(ctx, 'before')
            if shortcut:
                ctx.keys(CHORDS[action])
            else:
                _press(ctx, action, 'menu item')
            operation = _new_operation(ctx, previous, action)
            if action in ('region', 'edit', 'record'):
                ctx.expect(operation['state'] not in {'failed', 'interrupted'}, 'selector opened without an error')
                if menu_open:
                    ctx.expect(not _nodes(ctx, 'Quit put.gd', 'menu item'), 'shortcut closed tray menu')
                _frame(ctx, 'after')
                # If targeting is refused, fixture teardown owns selector
                # cleanup; never retry keyboard input through a failed guard.
                _focus(ctx, 'recsel' if action == 'record' else 'region')
                ctx.keys('Esc')
                # A real cancellation also establishes that the shortcut did
                # not merely admit work which failed before showing a selector.
                settled = _settled(ctx, operation)
                ctx.expect(settled['state'] == 'cancelled', 'selector cancelled through real Escape input')
                if action == 'record' and shortcut:
                    _record_toggle(ctx)
            else:
                settled = _settled(ctx, operation)
                ctx.expect(settled['state'] == 'completed' and settled.get('items'), 'local action completed')
                ctx.expect(all(i.get('uploadStatus') == 'local_only' and i.get('local', {}).get('status') == 'saved'
                               for i in settled['items']), 'every output saved locally without publication')
                _frame(ctx, 'after')
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
        _press(ctx, 'edit-sources', 'menu item')
        _menu_frame(ctx, 'before')
        _press(ctx, 'trayMenuAction-3deabb1a41ce13d8', 'menu item')
        operation = _new_operation(ctx, previous, 'edit')
        operation = ctx.wait(lambda: (o if (o := ctx.call('operation.inspect', {'id': operation['id']}))
                                      ['state'] == 'editing' else None), 'clipboard image editor')
        try:
            state = ctx.call('editor.inspect', {'id': operation['id']})
            ctx.expect(state.get('phase') == 'editing' and (state.get('width'), state.get('height')) == (96, 64),
                       'real-input operation opened the known synthetic 96×64 clipboard image')
            _frame(ctx, 'after')
        finally:
            ctx.call('editor.cancel', {'id': operation['id']})


@recipe('notification.saved-locally', desktop_drawn=True)
def saved_locally(ctx):
    with _local(ctx, notification_only=True):
        op = _local_upload(ctx, notification_only=True)
        ctx.expect(op['state'] == 'completed' and op['items'], 'local clipboard operation completed')
        ctx.expect(all(i['local']['status'] == 'saved' and i['uploadStatus'] == 'local_only' for i in op['items']),
                   'saved-local toast agrees with operation inspection')
        _drawn_notice(ctx, _notice(ctx, 'Saved locally', 'Saved locally'))
        _frame(ctx, 'after')


@recipe('tray.icon-attention', desktop_drawn=True)
def local_failure_attention(ctx):
    fixture = ctx.fixture('tray-attention')
    original = fixture.operation_id
    _menu(ctx)
    _node(ctx, 'Local save needs attention', 'menu item')
    _frame(ctx, 'latched-attention')
    fixture.retry()
    retried = _settled(ctx, fixture.state)
    ctx.expect(retried['id'] == original and all(i['local']['status'] == 'saved' for i in retried['items']), 'retry retains operation and saves each item')
    _node(ctx, 'Local save needs attention', 'menu item')
    _press(ctx, 'Local save needs attention', 'menu item')
    ctx.wait(lambda: ctx.call('ui.viewState').get('settings', {}).get('section') == 1, 'attention opens local-copy settings')
    _menu(ctx)
    try:
        ctx.expect(not _nodes(ctx, 'Local save needs attention', 'menu item'), 'attention click clears latch')
        _frame(ctx, 'after')
        ctx.review('Compare the panel danger glyph and badge with idle')
    finally:
        _dismiss_menu(ctx)


@recipe('notification.local-save-failed', desktop_drawn=True)
def local_save_failed_notice(ctx):
    with _local(ctx, failure=True, notification_only=True):
        previous = _local_input(ctx, notification_only=True)
        notice = _notice(ctx, 'Local save failed')
        _drawn_notice(ctx, notice)
        op = _settled(ctx, _new_operation(ctx, previous, 'clipboard'))
        ctx.expect(op['state'] == 'failed' and op['items'], 'genuine local obstruction failed the operation')
        ctx.expect(all(i['uploadStatus'] == 'local_only' and i['local']['status'] == 'failed' for i in op['items']), 'failed local copies were not published')
        ctx.expect('save-diagnostics' in notice.get('actions', []), 'native failure banner offers diagnostics')
        _frame(ctx, 'after')


@recipe('tray.menu-attention', desktop_drawn=True)
def attention_with_busy_latch(ctx):
    ctx.fixture('tray-proton-busy')


@recipe('notification.preference-off', desktop_drawn=True)
def notifications_off(ctx):
    with _local(ctx, notifications=False, notification_only=True):
        # A Notify call can be observed before its method return supplies the
        # notification ID. Preserve the monitor timestamp in the identity.
        identity = lambda n: (n.get('id'), n.get('at'), n.get('summary'), n.get('body'))
        old_ids = {identity(n) for n in ctx.notifications()}
        op = _local_upload(ctx, notification_only=True)
        ctx.expect(op['state'] == 'completed', 'operation still completes with notifications disabled')
        # Observe a bounded quiet window as well as the terminal resident state.
        _shell(ctx, "python3 -c 'import time; time.sleep(1)'\n")
        ctx.call('ui.snapshot')
        ctx.expect(not [n for n in ctx.notifications() if identity(n) not in old_ids
                        and n.get('summary') == 'Saved locally'], 'operation toast suppressed')
        _frame(ctx, 'after')


def _action(ctx, notice, action):
    ctx.notification_action(notice['id'], action)
    current = next(n for n in ctx.notifications() if n.get('id') == notice['id'])
    ctx.expect(any(e.get('action') == action for e in current.get('action_events', [])),
               'ActionInvoked belongs to the exact notification ID and action')
    ctx.note('notification action=' + action + ' evidence=' + json.dumps({k: current.get(k) for k in
             ('id', 'sender', 'server', 'at', 'action_events', 'closed')}))
    return current


def _host_opened(ctx, target, after=0):
    # Read only our private persistent log; do not export unrelated log records.
    script = """python3 - <<'PYLOG'
import json,os
from datetime import datetime
from pathlib import Path
rows=[]
for path in Path(os.environ['XDG_STATE_HOME']).rglob('putgd-*.jsonl'):
 for line in path.read_text().splitlines():
  try: row=json.loads(line)
  except ValueError: continue
  if row.get('message')=='Opened on the host' and row.get('fields',{}).get('target')==TARGET and datetime.fromisoformat(row['time'].replace('Z','+00:00')).timestamp()>=AFTER:
   rows.append(row)
print('opened' if rows else '')
PYLOG
""".replace('TARGET', repr(target)).replace('AFTER', repr(after))
    ctx.wait(lambda: _shell(ctx, script) == 'opened', 'accepted host open: ' + target)


def _diagnostics(ctx, *, reveal=False, denied=False, resident=False):
    with _local(ctx, failure=True, notification_only=True) as root:
        folder = root + '/diagnostics'
        _shell(ctx, 'mkdir -m ' + ('500' if denied else '700') + ' -- ' + shlex.quote(folder))
        _patch(ctx, {'preferences': {'diagnostics': {'debugLogging': False, 'packageFolder': folder}}})
        previous = _local_input(ctx, notification_only=True)
        notice = _notice(ctx, 'Local save failed')
        if resident:
            expected = _profile(ctx)['resident_notifications']
            ctx.expect(bool(notice.get('hints', {}).get('resident')) == expected,
                       'resident hint agrees with desktop policy')
            ctx.expect(notice.get('timeout') == (0 if expected else 10000),
                       'action banner timeout agrees with desktop policy')
        _action(ctx, notice, 'save-diagnostics')
        result = _notice(ctx, 'put.gd — diagnostics not saved' if denied else 'put.gd — diagnostics saved')
        if denied:
            _drawn_notice(ctx, result)
        else:
            # Consume the one banner slot immediately, before inspecting the
            # journal/package or taking a frame. Both shells serialize banners.
            _action(ctx, result, 'show-in-folder')
            if reveal:
                _host_opened(ctx, 'diagnostics-folder', result.get('at', 0))
        op = _settled(ctx, _new_operation(ctx, previous, 'clipboard'))
        ctx.expect(op['state'] == 'failed', 'local obstruction produced the diagnostic source operation')
        if denied:
            ctx.expect(not _shell(ctx, 'find ' + shlex.quote(folder) + " -maxdepth 1 -name '*.zip' -print"),
                       'failed diagnostics produced no package')
        else:
            ctx.expect('Nothing was uploaded.' in result.get('body', ''), 'package was saved without publication')
            name = _shell(ctx, "python3 - <<'PYZIP'\nimport zipfile\nfrom pathlib import Path\nfiles=list(Path(" +
                          repr(folder) + ").glob('*.zip'))\nassert len(files)==1\nwith zipfile.ZipFile(files[0]) as z: assert z.testzip() is None\nprint(files[0].name)\nPYZIP")
            ctx.expect(name.endswith('.zip'), 'one valid diagnostics package exists in the selected private folder')
        if resident:
            ctx.wait(lambda: next((n.get('closed') for n in ctx.notifications() if n.get('id') == notice['id']), None),
                     'used notification closed after the action')
        _frame(ctx, 'after')


@recipe('notification.save-diagnostics', 'notification.diagnostics-saved', desktop_drawn=True)
def save_diagnostics(ctx):
    _diagnostics(ctx)


@recipe('notification.show-in-folder', desktop_drawn=True)
def show_in_folder(ctx):
    _diagnostics(ctx, reveal=True)


@recipe('notification.resident-close', desktop_drawn=True)
def resident_close(ctx):
    _diagnostics(ctx, resident=True)


@recipe('notification.diagnostics-not-saved', desktop_drawn=True)
def diagnostics_not_saved(ctx):
    _diagnostics(ctx, denied=True)


@recipe('notification.recording-stopped', desktop_drawn=True)
def recording_stopped(ctx):
    fixture = ctx.fixture('recording-budget-stop')
    notice = _notice(ctx, 'put.gd — recording stopped', body='Largest file')
    _drawn_notice(ctx, notice)
    state = ctx.call('recording.inspect', {'id': fixture.recording_id})
    ctx.expect(state['state'] == 'reviewing' and state.get('stopReason') == 'budget',
               'budget stop retains the exact recording for private review')
    _frame(ctx, 'after')


@recipe('notification.incomplete-uploads', desktop_drawn=True, timeout=300)
def incomplete_uploads(ctx):
    fixture = ctx.fixture('upload-incomplete')
    notice = _notice(ctx, 'put.gd — incomplete uploads left')
    _drawn_notice(ctx, notice)
    state = ctx.call('operation.inspect', {'id': fixture.operationId})
    ctx.expect(state['state'] in ('failed', 'partial'), 'multipart failure retained its exact operation')
    _frame(ctx, 'after')


@recipe('tray.scroll-noop', desktop_drawn=True)
def scroll_noop(ctx):
    _begin(ctx)
    _page(ctx, 'Settings')
    before = {o['id'] for o in _recent(ctx)}
    point = _point(ctx, _icon(ctx))
    _frame(ctx, 'before')
    _icon_input(ctx, button='middle')
    ctx.scroll_accessible(_profile(ctx)['icon'], clicks=2)
    ctx.scroll_accessible(_profile(ctx)['icon'], clicks=-2)
    _shell(ctx, "python3 -c 'import time; time.sleep(1)'\n")
    ctx.expect({o['id'] for o in _recent(ctx)} == before, 'middle click/scroll admit no operation')
    ctx.expect(not _nodes(ctx, 'Quit put.gd', 'menu item'), 'middle click/scroll open no tray menu')
    _page(ctx, 'Settings')
    _frame(ctx, 'after')


@recipe('tray.icon-idle', desktop_drawn=True)
def idle(ctx):
    _begin(ctx, show_ui=not _profile(ctx).get('menu_prepare_background'))
    # app-profile starts a fresh private resident; there is no prior attention
    # latch to clear. Observe all menu labels together instead of repeatedly
    # walking the shell tree for names which should be absent.
    _frame(ctx, 'before')
    ctx.expect(all(o['state'] in TERMINAL for o in _recent(ctx)), 'resident has no active operation')
    _menu(ctx)
    try:
        policy = _profile(ctx).get('menu', {})
        rows = ctx.accessible({'name': '*', 'role': policy.get('role', 'menu item'),
                               'application': _profile(ctx)['icon']['application'],
                               'visible': True, 'showing': True})
        names = set()
        for node in rows:
            rect = node.get('rect')
            if isinstance(rect, dict):
                rect = [rect[k] for k in ('x', 'y', 'width', 'height')]
            if (node.get('visible') and node.get('showing') and rect and min(rect[:2]) >= 0
                    and min(rect[2:]) > 0):
                names.add(node.get('name'))
        ctx.expect('Cancel upload' not in names, 'idle menu has no cancel-upload row')
        ctx.expect(not names.intersection({'Local save needs attention', 'Destination needs attention',
                   'Capture needs attention', 'Action needs attention'}), 'idle menu has no attention latch')
        _frame(ctx, 'after')
        ctx.review('Saved panel frame needs glyph/tint review; menu metadata cannot establish icon pixels')
    finally:
        _dismiss_menu(ctx)


@recipe('tray.tooltip', desktop_drawn=True)
def tooltip(ctx):
    _begin(ctx, show_ui=not _profile(ctx).get('menu_prepare_background'))
    _icon(ctx)
    ctx.hover_accessible(_profile(ctx)['icon'])
    rows = ctx.tray()
    ctx.expect(len(rows) == 1, 'one owned status notifier')
    ctx.note('SNI tooltip=' + json.dumps(rows[0]['tooltip']))
    policy = {'role': 'tool tip', 'application': _profile(ctx)['icon']['application'],
              'visible': True, 'showing': True}
    try:
        ctx.wait(lambda: ctx.accessible(policy), 'drawn shell tooltip', timeout=5)
    except RecipeFailed:
        ctx.blocked('SNI tooltip metadata is available, but the drawn tooltip is absent from shell accessibility', 670)
    _frame(ctx, 'after')


@recipe('tray.menu-shortcut-suffix', desktop_drawn=True)
def shortcut_suffix(ctx):
    with _local(ctx, desktop_menu=True):
        _menu(ctx)
        try:
            for action, chord in CHORDS.items():
                node = _node(ctx, LABELS[action], 'menu item')
                accelerator = node.get('accelerator')
                if not accelerator:
                    ctx.blocked('Shell menu exposes no accelerator metadata for ' + action, 670)
                ctx.expect(accelerator == chord, 'menu accelerator agrees with accepted shortcut: ' + action)
            _menu_frame(ctx, 'after')
        finally:
            _dismiss_menu(ctx)


@recipe('tray.icon-uploading', 'tray.menu-upload-status', desktop_drawn=True)
def uploading(ctx):
    fixture = ctx.fixture('tray-offline')
    ctx.view('ui.show', {'view': 'history'})
    operation = ctx.call('operation.inspect', {'id': fixture.operation_id})
    ctx.expect(any(i.get('uploadStatus') == 'uploading' for i in operation.get('items', [])),
               'loopback fixture is still uploading')
    states = ctx.tray()
    ctx.expect(len(states) == 1 and bool(states[0]['icon_pixmaps']), 'one owned SNI icon has pixels')
    ctx.note('upload operation=' + fixture.operation_id + ' icon=' + json.dumps(states[0]['icon_pixmaps']))
    _menu(ctx)
    try:
        _node(ctx, 'Cancel upload', 'menu item')
        status = ctx.accessible({'name': 'Uploading*', 'application': _profile(ctx)['icon']['application']})
        ctx.expect(bool(status), 'native upload status row is exposed')
        _menu_frame(ctx, 'after', 'uploading')
        ctx.review('Compare native panel progress glyph and status row against the uploading catalog')
    finally:
        ctx.keys('Esc')


@recipe('notification.upload-complete', desktop_drawn=True)
def upload_complete(ctx):
    _ready(ctx)
    fixture = ctx.fixture('upload')
    notice = _notice(ctx, 'Upload complete')
    # Click immediately: the server serializes short-lived banners.
    _action(ctx, notice, 'default')
    ctx.expect(fixture.state['state'] == 'completed', 'loopback publication completed')
    _host_opened(ctx, 'notification-link', notice.get('at', 0))
    _frame(ctx, 'after')


def _drawn_notice(ctx, notice, *, by_body=False):
    policy = ctx.profile.get('notifications', {}).get('accessible')
    if not policy:
        ctx.blocked('Notification profile has no shell ownership selector', 670)
    def drawn():
        name = (glob.escape(__import__('html').unescape(notice.get('body', ''))) if by_body else
                ctx.profile.get('notifications', {}).get('title_name_glob', '{summary}').replace('{summary}', glob.escape(notice['summary'])))
        ctx.expect(bool(name), 'notification has an observable body or title')
        def on_screen(node):
            rect = node.get('rect')
            if isinstance(rect, dict):
                rect = [rect[k] for k in ('x', 'y', 'width', 'height')]
            return rect and min(rect[:2]) >= 0 and min(rect[2:]) > 0
        for field in ('text', 'name') if by_body else ('name',):
            nodes = ctx.accessible({**policy, field: name, 'visible': True, 'showing': True})
            if any(on_screen(n) for n in nodes):
                return True
        return False
    ctx.wait(drawn, 'drawn shell popup: ' + notice['summary'],
             timeout=ctx.profile.get('notifications', {}).get('draw_seconds', 3))


def _icon_signature(ctx):
    rows = ctx.tray()
    ctx.expect(len(rows) == 1 and rows[0].get('icon_pixmaps'), 'one private SNI with icon pixels')
    return json.dumps(rows[0]['icon_pixmaps'], sort_keys=True)


def _record_toggle(ctx):
    active = ctx.fixture('tray-active-recording')
    _focus(ctx, 'trayFixture')
    ctx.keys(CHORDS['record'])
    ctx.wait(lambda: active.inspect().get('state') == 'reviewing', 'record shortcut stops the same recording', timeout=40)
    previous = {o['id'] for o in _recent(ctx)}
    other = ctx.fixture('settings-focus-loss')
    _focus(ctx, other.role)
    ctx.keys(CHORDS['record'])
    # Validate activation without raising the review window ourselves.
    ctx.focus('review', raise_window=False)
    ctx.expect(active.inspect()['state'] == 'reviewing', 'toggle raises existing review')
    ctx.expect({o['id'] for o in _recent(ctx)} == previous, 'raise admits no new operation')
    _frame(ctx, 'raised-review')


@recipe('tray.icon-recording', desktop_drawn=True, timeout=240)
def recording_icon(ctx):
    active = ctx.fixture('tray-active-recording')
    ctx.wait(lambda: active.inspect().get('phase') == 'recording', 'recording phase')
    recording = _icon_signature(ctx)
    _frame(ctx, 'recording')
    active.pause()
    ctx.wait(lambda: active.inspect().get('phase') == 'paused', 'paused recording phase')
    ctx.wait(lambda: _icon_signature(ctx) != recording, 'paused badge differs from recording dot')
    _frame(ctx, 'paused')
    active.resume()
    ctx.wait(lambda: active.inspect().get('phase') == 'recording', 'same recording resumes')
    ctx.wait(lambda: _icon_signature(ctx) == recording, 'recording dot restored')
    active.stop()
    ctx.review('Compare native dot and pause bars in the saved panel frames')


@recipe('tray.menu-cancel-upload', desktop_drawn=True, timeout=300)
def cancel_uploads(ctx):
    active = ctx.fixture('tray-two-uploads')
    _begin(ctx, show_ui=False)
    expected = set(active.operation_ids)
    ctx.expect(len(expected) == 2 and all(o['state'] not in TERMINAL for o in active.inspect()), 'two independent uploads are active')
    _menu(ctx)
    _press(ctx, 'Cancel all uploads…', 'menu item')
    state = ctx.wait(lambda: (v if (v := ctx.call('ui.history', {'action': 'inspect'})).get('cancelAllConfirmation', {}).get('open') else None), 'native cancel-all confirmation')
    ctx.expect(set(state['cancelAllConfirmation']['operationIds']) == expected, 'confirmation identifies exactly the two owned uploads')
    _frame(ctx, 'confirmation')
    _focus(ctx, 'confirm')
    ctx.click('historyConfirmCancelAll', window='confirm')
    ctx.wait(lambda: all(o['state'] == 'cancelled' for o in active.inspect()), 'both confirmed uploads cancelled', timeout=75)
    _frame(ctx, 'cancelled')


@recipe('tray.menu-update-available', desktop_drawn=True, timeout=240)
def update_available(ctx):
    ctx.fixture('tray-update-available')
    _begin(ctx, show_ui=False)
    status = ctx.call('update.status')
    label = 'Update available: ' + status['result']['latest']
    _menu(ctx)
    _press(ctx, label, 'menu item')
    ctx.wait(lambda: ctx.call('ui.viewState').get('settings', {}).get('section') == 0, 'Updates page opened')
    _frame(ctx, 'after')


@recipe('tray.menu-quit', desktop_drawn=True)
def quit_resident(ctx):
    lifecycle = ctx.fixture('resident-lifecycle')
    _begin(ctx, show_ui=False)
    _menu(ctx)
    _press(ctx, 'trayMenuAction-33617f924b4aa7fb', 'menu item')
    ctx.wait(lifecycle.stopped, 'real Quit releases the private socket')
    _frame(ctx, 'quit')
    restarted = lifecycle.launch()
    ctx.expect(restarted != lifecycle.initial, 'fresh authenticated resident PID/start identity')
    _ready(ctx)
    _frame(ctx, 'restored')


@recipe('tray.qt-fallback', desktop_drawn=True)
def qt_fallback(ctx):
    graphical = ctx.fixture('tray-qt-fallback')
    ctx.expect(graphical.graphical and graphical.native_session == ctx.profile['session'], 'real QApplication preserves native desktop')
    ctx.wait(lambda: len(ctx.tray()) == 1, 'graphical Qt tray item registered before its toast')
    with _local(ctx, failure=True, notification_only=True):
        previous = _local_input(ctx, notification_only=True)
        notice = _notice(ctx, 'Local save failed')
        ctx.expect('save-diagnostics' not in notice.get('actions', []), 'Qt fallback toast has no action buttons')
        _drawn_notice(ctx, notice)
        _settled(ctx, _new_operation(ctx, previous, 'clipboard'))
        _frame(ctx, 'toast')
        if _profile(ctx).get('activation_clicks', 1) == 2:
            _icon(ctx); ctx.double_click_accessible(_profile(ctx)['icon'])
        else:
            _icon_input(ctx)
        _page(ctx, 'History')
        _frame(ctx, 'history')


@recipe('shortcut.over-open-menu', desktop_drawn=True)
def other_client_menu(ctx):
    popup = ctx.fixture('shortcut-other-menu')
    ctx.expect(bool(popup.observe()), 'separate owned popup is visible')
    _frame(ctx, 'other-client-menu')
    if not popup.keyboard_grab_verified:
        ctx.blocked('Real other-client popup exists, but its native keyboard-grab ownership is not witnessed', 670)
    ctx.blocked('Native grabbed-popup key targeting is not exposed by this fixture', 670)


@recipe('shortcut.ignored-while-recording', desktop_drawn=True)
def recorder_ignores_shortcut(ctx):
    recorder = ctx.fixture('shortcut-settings-recorder')
    _patch(ctx, {'actions': {'clipboard': {'shortcut': CHORDS['clipboard']}}})
    ctx.wait(lambda: ctx.call('ui.snapshot')['diagnostics']['shortcuts']['bindings']['clipboard'].get('bound'),
             'native clipboard binding ready before recorder')
    before = recorder.no_operation_baseline()
    recorder.start('region')
    previous = {o['id'] for o in _recent(ctx)}
    ctx.keys(CHORDS['clipboard'])
    ctx.wait(lambda: not ((recorder.recording_status() or {}).get('text') or '').startswith('Press shortcut'), 'recorder consumed the chord')
    ctx.expect({o['id'] for o in _recent(ctx)} == previous, 'recorded chord admitted no operation')
    after = recorder.no_operation_baseline()
    ctx.expect(after == before, 'native shortcut recorder leaves operations and history unchanged')
    _frame(ctx, 'recorded')


@recipe('shortcut.paused-while-settings-records', desktop_drawn=True)
def recorder_releases_grabs(ctx):
    recorder = ctx.fixture('shortcut-settings-recorder')
    _patch(ctx, {'actions': {name: {'shortcut': chord} for name, chord in CHORDS.items()}})
    def bindings_ready():
        bindings = recorder.status()['bindings']
        return bindings if all(bindings.get(name, {}).get('bound') for name in CHORDS) else None
    before = ctx.wait(bindings_ready, 'native bindings ready before shortcut recording')
    expected = {k for k,v in before.items() if v.get('bound')}
    ctx.expect(bool(expected), 'native bindings are present before recording')
    recorder.start('region')
    state = recorder.status()['bindings']
    ctx.expect(all(state[k].get('state') == 'recording' and state[k].get('bound') is False for k in expected), 'native grabs released during recording')
    recorder.cancel()
    ctx.wait(lambda: all(recorder.status()['bindings'][k].get('bound') for k in expected), 'all native grabs restored')
    _frame(ctx, 'restored')


@recipe('shortcut.layout-change', desktop_drawn=True)
def keyboard_layout_change(ctx):
    with _local(ctx):
        _patch(ctx, {'actions': {'clipboard': {'shortcut': 'Ctrl+Shift+Y'}}})
        _clipboard(ctx)
        layout = ctx.fixture('shortcut-keyboard-layout', layout='de')
        ctx.wait(lambda: layout.shortcuts()['bindings']['clipboard'].get('bound'), 'letter shortcut rebound on German layout')
        previous = {o['id'] for o in _recent(ctx)}
        # QMP's physical US Z key produces Y on the owned German X11 map.
        ctx.keys('Ctrl+Shift+Z')
        op = _settled(ctx, _new_operation(ctx, previous, 'clipboard'))
        ctx.expect(op['state'] == 'completed', 'rebound physical letter key invokes clipboard')
        layout.restore()
        ctx.wait(lambda: layout.shortcuts()['bindings']['clipboard'].get('bound'), 'binding restored with original map')
        _frame(ctx, 'after')


@recipe('notification.action-rejected', desktop_drawn=True)
def action_rejected(ctx):
    ctx.fixture('settings-no-output')
    _patch(ctx, {'actions': {'clipboard': {'shortcut': CHORDS['clipboard']}}})
    ctx.view('ui.show', {'view': 'settings'})
    _clipboard(ctx)
    before = {o['id'] for o in _recent(ctx)}
    ctx.wait(lambda: ctx.call('ui.snapshot')['diagnostics']['shortcuts']['bindings']['clipboard'].get('bound'),
             'native clipboard binding available')
    ctx.keys(CHORDS['clipboard'])
    notice = _notice(ctx, 'put.gd')
    # The app title also names the tray icon; require the actual error body.
    _drawn_notice(ctx, notice, by_body=True)
    ctx.expect({o['id'] for o in _recent(ctx)} == before, 'rejected action admits no operation')
    _frame(ctx, 'rejected')


@recipe('notification.upload-failed', desktop_drawn=True, timeout=240)
def upload_failed_notice(ctx):
    failed = ctx.fixture('tray-failed')
    ctx.expect(failed.state['state'] in ('failed', 'partial'), 'loopback write fault fails the owned upload')
    notice = _notice(ctx, 'Upload failed')
    ctx.expect(bool(notice.get('body')), 'failure notice includes a reason')
    _drawn_notice(ctx, notice)
    _frame(ctx, 'failed-upload')
    before = {n['id'] for n in ctx.notifications()}
    cancelled = ctx.fixture('tray-cancelled')
    ctx.expect(cancelled.state['state'] == 'cancelled', 'separate upload was cancelled')
    fresh = [n for n in ctx.notifications() if n.get('id') not in before]
    ctx.expect(not any(n.get('summary') in ('Upload failed', 'Upload interrupted') for n in fresh),
               'cancelled upload emits no failure toast')
    _frame(ctx, 'cancelled-without-failure-toast')
    ctx.blocked('Failed-upload and cancellation variants exercised; interrupted upload with retained local copy still needs a core fixture', 670)


# Explicit declarations are independent rows, so unavailable scenarios never
# silently disappear from a lane run. Issue numbers are filled by the follow-up
# tracker; no provider session is borrowed to cover an offline-only campaign.
BLOCKED = {
    'tray.icon-complete': 'upload fixture completes before control returns; needs a held completion state and sub-600-ms icon sampling',
    'shortcut.locked-session': 'Needs core-owned visible session lock/unlock lifecycle and protected login handling',
    'notification.completed-with-problem': 'Needs offline mixed-success batch fixture with inspected per-item outcomes',
    'notification.in-flight-limit': 'Needs a delayed-reply notification server fixture; eight outstanding Notify calls are not eight visible toasts',
}


def _blocked_recipe(interaction, reason):
    @recipe(interaction, desktop_drawn=True)
    def unavailable(ctx):
        _frame(ctx, 'before')
        ctx.note(reason)
        _frame(ctx, 'after')
        ctx.blocked(reason, _profile(ctx).get('fixture_issue'))
    unavailable.__name__ = interaction.replace('.', '_').replace('-', '_')
    return unavailable


for _id, _reason in BLOCKED.items():
    globals()[_id.replace('.', '_').replace('-', '_')] = _blocked_recipe(_id, _reason)
