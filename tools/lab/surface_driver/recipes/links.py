"""Links lane. Synthetic uploads in disposable guests only.

The upload fixture supplies history_id, name, link (memory only) and optionally
expect={kind,name,preview,url_host,text}. ``notification_upload`` must be a fresh
upload fixture, so the desktop notification is still actionable. ``release``
supplies an expectation for the running build's actual GitHub release page.
All desktop-specific selectors belong in profile fragments.
"""
from __future__ import annotations

from ..api import recipe
from ..browser import default_browser, open_and_verify

_DESTINATIONS = ('local', 's3', 'proton')


def _value(fixture, key, default=None):
    return fixture.get(key, default) if isinstance(fixture, dict) else getattr(fixture, key, default)


def _guest(ctx):
    if ctx.placement not in ('guest', 'boat'):
        ctx.blocked('Links recipes require a disposable guest; physical desktops are excluded')
    # boat placement alone is not proof of a nested guest. The driver must
    # explicitly attest the guest identity in its profile.
    if ctx.placement == 'boat' and not ctx.profile.get('guest'):
        ctx.blocked('Boat links tests require a nested guest identity')


def _upload_fixture(ctx, name):
    upload = ctx.fixture(name)
    link = _value(upload, 'link')
    if not isinstance(link, str) or not link.startswith(('http://', 'https://')) or not _value(upload, 'name'):
        ctx.blocked('Links need an HTTP upload fixture with a filename; a local-save row has no browser page', issue=691)
    return upload


def _expect(ctx, upload):
    return _value(upload, 'expect') or {
        'kind': 'proton' if ctx.destination == 'proton' else 's3' if ctx.destination == 's3' else 'local',
        'name': _value(upload, 'name'), 'preview': True}


def _history(ctx, upload, details=True):
    ctx.view('ui.show', {'view': 'history'})
    ctx.view('ui.history', {'action': 'select', 'ids': [_value(upload, 'history_id')]})
    if details:
        ctx.view('ui.history', {'action': 'openDetails'})
    else:
        ctx.view('ui.history', {'action': 'closeDetails'})


def _accessible_click(ctx, query):
    nodes = ctx.wait(lambda: ctx.accessible(query), 'visible desktop control', timeout=10)
    visible = [node for node in nodes if node.get('visible', True) and node.get('enabled', True)
               and (node.get('bounds') or (node.get('rect') and node['rect'][2] > 0 and node['rect'][3] > 0))]
    ctx.expect(len(visible) == 1, 'one unambiguous visible desktop control')
    bounds = visible[0].get('bounds')
    if bounds is None and visible[0].get('rect'):
        bounds = dict(zip(('x', 'y', 'width', 'height'), visible[0]['rect']))
    if not isinstance(bounds, dict) or not all(key in bounds for key in ('x', 'y', 'width', 'height')):
        ctx.blocked('AT-SPI control needs screen bounds for real input')
    ctx.expect(bounds['width'] > 0 and bounds['height'] > 0, 'desktop control has positive bounds')
    ctx.click_at(round(bounds['x'] + bounds['width'] / 2), round(bounds['y'] + bounds['height'] / 2))


def _paste(ctx):
    # Firefox is already focused by default_browser's blank operation. Paste
    # directly from the desktop clipboard: no URL appears in type_text/logs.
    ctx.keys('Ctrl+L', 'Ctrl+V', 'Return')


@recipe('notification.open-in-browser', needs=('notification_upload',),
        destinations=_DESTINATIONS, desktop_drawn=True)
def notification_open(ctx):
    _guest(ctx)
    with default_browser(ctx, 'firefox'):
        upload = _upload_fixture(ctx, 'notification_upload')
        query = ctx.profile.get('links', {}).get('notification_query', {'name': 'Upload complete'})
        open_and_verify(ctx, lambda: _accessible_click(ctx, query), _expect(ctx, upload))


@recipe('notification.link-copy-result', needs=('notification_upload',),
        destinations=_DESTINATIONS, desktop_drawn=True)
def notification_copy_result(ctx):
    _guest(ctx)
    with default_browser(ctx, 'firefox'):
        upload = _upload_fixture(ctx, 'notification_upload')
        notices = ctx.wait(lambda: [n for n in ctx.notifications()
                                   if 'Link copied to the clipboard.' in n.get('body', '')],
                           'upload notification reports the link copied')
        ctx.expect(bool(notices), 'desktop notification reports successful copy')
        ctx.expect(ctx.clipboard_text() == _value(upload, 'link'), 'desktop clipboard contains this upload link')
        open_and_verify(ctx, lambda: _paste(ctx), _expect(ctx, upload))


@recipe('history.link-field', needs=('upload',), destinations=_DESTINATIONS, desktop_drawn=True)
def history_link_field(ctx):
    _guest(ctx)
    upload = _upload_fixture(ctx, 'upload')
    with default_browser(ctx, 'firefox'):
        _history(ctx, upload)
        # The read-only field itself is selectable with actual keyboard input.
        ctx.click('historyLink', window='history')
        ctx.keys('Ctrl+A', 'Ctrl+C')
        ctx.expect(ctx.clipboard_text() == _value(upload, 'link'), 'stored link field copies exactly')
        open_and_verify(ctx, lambda: _paste(ctx), _expect(ctx, upload))


@recipe('history.copy-link', needs=('upload',), destinations=_DESTINATIONS, desktop_drawn=True)
def history_copy_link(ctx):
    _guest(ctx)
    upload = _upload_fixture(ctx, 'upload')
    with default_browser(ctx, 'firefox'):
        _history(ctx, upload)
        ctx.click('copyHistoryLink', window='history')
        ctx.wait(lambda: ctx.clipboard_text() == _value(upload, 'link'), 'History Copy reaches desktop clipboard')
        open_and_verify(ctx, lambda: _paste(ctx), _expect(ctx, upload))


@recipe('history.open-link', needs=('upload',), destinations=_DESTINATIONS,
        desktop_drawn=True, timeout=240)
def history_open_link(ctx):
    _guest(ctx)
    upload = _upload_fixture(ctx, 'upload')
    with default_browser(ctx, 'firefox'):
        def inspector():
            _history(ctx, upload)
            ctx.click('openHistoryLink', window='history')
        open_and_verify(ctx, inspector, _expect(ctx, upload))
        ctx.note('History inspector Open verified')
        def row_menu():
            _history(ctx, upload, details=False)
            ctx.click('historyList', window='history')
            ctx.view('ui.history', {'action': 'select', 'ids': [_value(upload, 'history_id')]})
            ctx.keys('Shift+F10')
            query = ctx.profile.get('links', {}).get('row_open_query', {'name': 'Open in browser', 'role': 'menu item'})
            _accessible_click(ctx, query)
        open_and_verify(ctx, row_menu, _expect(ctx, upload))
        ctx.note('History row menu Open verified')
        open_and_verify(ctx, lambda: ctx.mcp('open_history_link', {'id': _value(upload, 'history_id')}),
                        _expect(ctx, upload))
        ctx.note('MCP open_history_link verified')


@recipe('shell.version-link', needs=('release',), destinations=('local',), desktop_drawn=True)
def shell_version_link(ctx):
    _guest(ctx)
    release = ctx.fixture('release')
    with default_browser(ctx, 'firefox'):
        def trigger():
            ctx.view('ui.show', {'view': 'history'})
            ctx.click('appVersionLink', window='history')
        open_and_verify(ctx, trigger, _value(release, 'expect'))
