"""Real-input cloud recipes. No provider credentials or share URLs are retained here.

`live` is a run-owned fixture, not a preuploaded history entry. Its nonsecret
contract is documented by LIVE_FIXTURE_FIELDS below and issue #669. The runner
must own the private profile, fresh remote namespace, bridge/lock, redaction and
cleanup. Boat supplies moto and stubbed Proton; those runs are fixture evidence.

Browser code is deliberately imported lazily while the links lane develops it.
No runner, input backend, provider SDK, or substitute browser is implemented here.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone
from urllib.parse import urlsplit

from tools.lab.surface_driver.api import recipe

CORE_ISSUE = 669
CLOUD = ('proton', 's3', 'b2')
LIVE_FIXTURE_FIELDS = (
    'configured', 'isolated', 'sensitive_transport', 'redacted_frames',
    'profile_id', 'profile_label', 'folder_name', 'expected_host',
    'region_start', 'region_end', 'window_focus', 'region_shortcut',
    'window_shortcut', 'image_path', 'editor_mark_start', 'editor_mark_end',
)
# These are scenario claims, not extra catalog ids. Unsupported provider cells
# block explicitly; a registry entry alone is never acceptance evidence.
SCENARIOS = {
    'upload-region': ('region.to-upload',),
    'upload-window': ('shell.command-capture-window',),
    'upload-editor': ('editor.upload',),
    'upload-recording': ('review.upload',),
    'notification-link': ('notification.open-in-browser',),
    'history-link': ('history.open-link',),
    'row-menu-link': ('history.row-menu',),
    'clipboard-link': ('history.copy-link',),
    'mcp-link': ('mcp.history',),
    'private-upload': ('editor.upload',),
    'history-sharing': ('history.proton-sharing',),
    'revoke-confirmation': ('confirm.revoke-link',),
    'account-folder': ('destinations.test',),
    'new-folder': ('destinations.proton-folder',),
    'cleanup': ('history.delete-remote', 'confirm.history-delete'),
    's3-access': ('destinations.s3-acl', 'destinations.s3-public-url'),
}


def _live(ctx):
    f = ctx.fixture('live')
    if not isinstance(f, dict) or not f.get('configured'):
        ctx.blocked('No isolated live destination configured (B2 is optional).', CORE_ISSUE)
    for flag in ('isolated', 'sensitive_transport', 'redacted_frames'):
        if f.get(flag) is not True:
            ctx.blocked('Live fixture requires verified ' + flag + '.', CORE_ISSUE)
    if ctx.destination == 'proton' and ctx.host != 'boat':
        if ctx.placement != 'guest' or ctx.host not in ('f44', 'd69'):
            ctx.blocked('This lane borrows Proton only in an f44 guest (d69 fallback).', CORE_ISSUE)
        if not f.get('proton_lock_held') or not f.get('bridge_lent'):
            ctx.blocked('Proton lock and outstanding guest-bridge lend are required.', CORE_ISSUE)
    return f


def _field(ctx, name):
    f = _live(ctx)
    if name not in f or f[name] in (None, ''):
        ctx.blocked('Live fixture missing ' + name + '.', CORE_ISSUE)
    return f[name]


def _document(ctx):
    return ctx.call('settings.get')['document']


def _profile(ctx):
    profile_id = _field(ctx, 'profile_id')
    profiles = _document(ctx)['profiles']
    ctx.expect(len(profiles) == 1, 'isolated fixture has only its campaign destination')
    matches = [p for p in profiles if p['id'] == profile_id]
    ctx.expect(len(matches) == 1, 'exact campaign destination exists')
    expected = {'proton': 'proton-drive', 's3': 's3', 'b2': 'backblaze-b2'}[ctx.destination]
    ctx.expect(matches[0]['provider'] == expected, 'destination provider matches row')
    return matches[0]


def _route(ctx, mode):
    """Fixture setup only: the interactions under test below use real input."""
    f = _live(ctx)
    profile = _profile(ctx)
    if ctx.destination != 'proton' and mode == 'private':
        ctx.blocked('S3/B2 do not implement private publication mode or share.inspect.', CORE_ISSUE)
    if ctx.destination == 'proton':
        ctx.expect(profile['settings'].get('folderName') == f['folder_name'],
                   'uploads target the fresh campaign folder')
    else:
        ctx.expect(profile['settings'].get('objectPrefix') == _field(ctx, 'object_prefix'),
                   'uploads target the fresh campaign object prefix')
    current = ctx.call('settings.get')
    patch = {'actions': {name: {'destination': 'upload', 'profileId': f['profile_id'],
                               'publicationMode': mode}
                         for name in ('region', 'window', 'edit', 'record')}}
    ctx.call('settings.patch', {'expectedRevision': current['metadata']['observedRevision'], 'patch': patch})
    accepted = _document(ctx)
    for name in patch['actions']:
        action = accepted['actions'][name]
        ctx.expect(action['profileId'] == f['profile_id'] and action['publicationMode'] == mode,
                   'accepted route for ' + name)


def _recent(ctx):
    return {o['id'] for o in ctx.call('operation.recent')['operations']}


def _when(value, predicate):
    return value if predicate(value) else None


def _operation(ctx, operation_id):
    ctx.note('operationId=' + operation_id)
    final = ctx.wait(lambda: _when(ctx.call('operation.inspect', {'id': operation_id}),
                                  lambda v: v.get('state') in ('completed', 'failed', 'cancelled', 'partial')),
                     'cloud upload final outcome', timeout=300)
    ctx.expect(final['state'] == 'completed', 'cloud operation completed successfully')
    records = ctx.wait(lambda: [r for r in ctx.call('history.list', {
        'profileId': _field(ctx, 'profile_id'), 'limit': 100})['records']
        if r.get('operationId') == operation_id], 'durable upload history', timeout=30)
    ctx.expect(len(records) == 1, 'one capture produced exactly one history file')
    detail = ctx.call('history.detail', {'id': records[0]['id']})
    ctx.expect(detail.get('uploadStatus') == 'uploaded', 'provider confirmed upload')
    ctx.expect(detail.get('name'), 'uploaded file has a name')
    ctx.note('historyId=' + detail['id'])
    _live(ctx).setdefault('owned_history_ids', []).append(detail['id'])
    return detail


def _new_operation(ctx, before):
    fresh = ctx.wait(lambda: [o for o in ctx.call('operation.recent')['operations'] if o['id'] not in before],
                     'capture operation accepted', timeout=30)
    ctx.expect(len(fresh) == 1, 'no concurrent capture in isolated profile')
    return fresh[0]['id']


def _upload(ctx, source='region', mode='share'):
    _route(ctx, mode)
    before = _recent(ctx)
    if source in ('region', 'window'):
        if source == 'window':
            ctx.click_at(*_field(ctx, 'window_focus'))
        ctx.keys(_field(ctx, source + '_shortcut'))
        if source == 'region':
            ctx.wait(lambda: ctx.geometry('region'), 'live region selector')
            ctx.drag(tuple(_field(ctx, 'region_start')), tuple(_field(ctx, 'region_end')))
        operation_id = _new_operation(ctx, before)
    elif source == 'editor':
        opened = ctx.view('editor.open', {'source': 'file', 'path': _field(ctx, 'image_path')})
        operation_id = opened['operationId']
        ctx.wait(lambda: ctx.call('operation.inspect', {'id': operation_id}).get('state') == 'editing',
                 'editor ready')
        ctx.keys('R')
        ctx.drag(tuple(_field(ctx, 'editor_mark_start')), tuple(_field(ctx, 'editor_mark_end')))
        edited = ctx.call('editor.inspect', {'id': operation_id})
        ctx.expect(edited.get('shapes'), 'real editor gesture added a mark')
        ctx.frame('editor-before-explicit-upload')
        ctx.click('editorUpload', window='editor')
    elif source == 'recording':
        prepared = ctx.call('recording.prepare', {'target': {'kind': 'region'}, 'format': 'webm',
                                                 'fps': 10, 'audio': {'system': False, 'microphone': False}})
        operation_id = prepared['operationId']
        recording_id = prepared['recordingId']
        ctx.wait(lambda: ctx.call('recording.inspect', {'id': recording_id}).get('selecting'),
                 'recording selector')
        ctx.drag(tuple(_field(ctx, 'region_start')), tuple(_field(ctx, 'region_end')))
        ctx.wait(lambda: ctx.call('recording.inspect', {'id': recording_id}).get('state') == 'ready',
                 'recording controls ready', timeout=30)
        ctx.click('record', window='bar')
        ctx.wait(lambda: ctx.call('recording.inspect', {'id': recording_id}).get('durationUs', 0) >= 2_000_000,
                 'two seconds of actual capture', timeout=30)
        ctx.click('stop', window='bar')
        ctx.wait(lambda: ctx.call('recording.inspect', {'id': recording_id}).get('state') == 'reviewing',
                 'private recording review', timeout=30)
        ctx.frame('recording-before-explicit-upload')
        ctx.click('upload', window='review')
    else:
        raise ValueError('Unknown synthetic source')
    detail = _operation(ctx, operation_id)
    if mode == 'share':
        ctx.expect(detail.get('linkAvailable') is True, 'uploaded file has a usable share link')
    else:
        ctx.expect(detail.get('linkAvailable') is False, 'private upload has no link')
        ctx.expect(detail.get('shareStatus') == 'private', 'provider confirmed private publication')
    ctx.frame(source + '-uploaded-' + mode)
    return detail


def _select(ctx, detail):
    ctx.view('ui.show', {'view': 'history'})
    ctx.call('ui.history', {'action': 'clearFilters', 'scope': 'all'})
    state = ctx.view('ui.history', {'action': 'select', 'ids': [detail['id']]})
    ctx.expect(state.get('selectedIds') == [detail['id']], 'History selected exact owned file')
    ctx.view('ui.history', {'action': 'openDetails'})


def _browser(ctx, trigger, detail, expected_host=None):
    try:
        from tools.lab.surface_driver.browser import open_and_verify
    except ImportError:
        ctx.blocked('Links lane browser.py has not landed.', CORE_ISSUE)
    result = open_and_verify(ctx, trigger, expect={
        'kind': 'proton' if ctx.destination == 'proton' else 's3',
        'name': detail['name'], 'preview': True})
    ctx.expect(result.get('matched') is True, 'browser shows expected file and decrypted preview/object')
    ctx.expect(result.get('url_host') == (expected_host or _field(ctx, 'expected_host')), 'browser reached expected provider/domain')
    ctx.expect(bool(result.get('frames')), 'browser kept full-desktop frames')
    # Do not copy title, URLs or browser-returned data into notes.


def _default_browser(ctx):
    try:
        from tools.lab.surface_driver.browser import default_browser
    except ImportError:
        ctx.blocked('Links lane browser.py has not landed.', CORE_ISSUE)
    return default_browser(ctx, 'firefox')


def _click_named(ctx, name, window):
    ctx.click('text:' + name, window=window)


def _share_action(ctx, detail, control, expected):
    before = ctx.call('history.detail', {'id': detail['id']}).get('shareOperationId')
    ctx.click(control, window='history')
    return _share_result(ctx, detail, before, expected)


def _share_result(ctx, detail, before, expected):
    updated = ctx.wait(lambda: _when(ctx.call('history.detail', {'id': detail['id']}),
                                    lambda d: d.get('shareOperationId') and d['shareOperationId'] != before),
                       'new durable share operation', timeout=30)
    operation_id = updated['shareOperationId']
    ctx.note('shareOperationId=' + operation_id)
    outcome = ctx.wait(lambda: _when(ctx.call('share.inspect', {'id': operation_id}),
                                    lambda o: o.get('state') in ('completed', 'failed', 'interrupted')),
                       'durable sharing outcome', timeout=180)
    ctx.expect(outcome.get('state') == 'completed' and outcome.get('status') == expected,
               'provider confirmed sharing status ' + expected)
    return outcome


@recipe(*SCENARIOS['upload-region'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=360)
def upload_region(ctx):
    _upload(ctx)


@recipe(*SCENARIOS['upload-window'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=360)
def upload_window(ctx):
    _upload(ctx, 'window')


@recipe(*SCENARIOS['upload-editor'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=360)
def upload_editor(ctx):
    _upload(ctx, 'editor')


@recipe(*SCENARIOS['upload-recording'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def upload_recording(ctx):
    _upload(ctx, 'recording')


def _link_entry(ctx, entry):
    _live(ctx)
    with _default_browser(ctx):
        detail = _upload(ctx)
        _select(ctx, detail)
        if entry == 'history':
            trigger = lambda: ctx.click('openHistoryLink', window='history')
        elif entry == 'row-menu':
            def trigger():
                ctx.click('historyRowMore', window='history')
                _click_named(ctx, 'Open in browser', 'historyRowMenu')
        elif entry == 'clipboard':
            def trigger():
                ctx.click('copyHistoryLink', window='history')
                # Clipboard and type_text must remain in memory in core transport.
                value = ctx.clipboard_text()
                expected = detail.get('receipt', {}).get('directUrl')
                ctx.expect(bool(expected) and value == expected, 'Copy link equals exact uploaded receipt')
                if not ctx.accessible({'role': 'frame', 'name': '*Firefox*'}):
                    ctx.blocked('Copy-link browser entry requires an opened, focused guest Firefox window.', CORE_ISSUE)
                ctx.blocked('Browser helper must focus a blank Firefox tab before clipboard URL typing; current API only promises trigger verification.',
                            CORE_ISSUE)
        elif entry == 'mcp':
            def trigger():
                result = ctx.mcp('open_history_link', {'id': detail['id']})
                ctx.expect(result.get('ok') is True, 'MCP history-open request succeeded')
                ctx.expect(result.get('result', {}).get('opened') is True, 'MCP opened exact history link')
        elif entry == 'notification':
            def trigger():
                # Names/bodies can contain a link; never retain the notification.
                notices = [n for n in ctx.notifications() if detail['name'] in n.get('body', '')]
                ctx.expect(len(notices) == 1, 'one notification for this upload')
                ctx.expect('default' in notices[0].get('actions', []), 'notification offers default action')
                _click_named(ctx, notices[0]['summary'], 'notifications')
        else:
            raise ValueError('Unknown link entry')
        _browser(ctx, trigger, detail)


@recipe(*SCENARIOS['notification-link'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def notification_link(ctx):
    _link_entry(ctx, 'notification')


@recipe(*SCENARIOS['history-link'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def history_link(ctx):
    _link_entry(ctx, 'history')


@recipe(*SCENARIOS['row-menu-link'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def row_menu_link(ctx):
    _link_entry(ctx, 'row-menu')


@recipe(*SCENARIOS['clipboard-link'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def clipboard_link(ctx):
    _link_entry(ctx, 'clipboard')


@recipe(*SCENARIOS['mcp-link'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def mcp_link(ctx):
    _link_entry(ctx, 'mcp')


@recipe(*SCENARIOS['private-upload'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=360)
def private_upload(ctx):
    _live(ctx)
    if ctx.destination != 'proton':
        ctx.blocked('S3/B2 have no private-upload/share.inspect capability.', CORE_ISSUE)
    before = ctx.clipboard_text()
    detail = _upload(ctx, 'editor', 'private')
    ctx.expect(ctx.clipboard_text() == before, 'private upload left clipboard unchanged')
    _select(ctx, detail)
    _share_action(ctx, detail, 'inspectHistoryLink', 'private')
    after = ctx.call('history.detail', {'id': detail['id']})
    ctx.expect(after.get('linkAvailable') is False and not after.get('receipt', {}).get('directUrl'),
               'share.inspect confirmed no link exists')
    ctx.frame('private-no-link')


def _sharing_upload(ctx):
    _live(ctx)
    if ctx.destination != 'proton':
        ctx.blocked('History create/inspect/revoke is Proton-only; S3/B2 use immutable object links.', CORE_ISSUE)
    expiry_days = _profile(ctx)['settings'].get('linkExpirationDays', 0)
    ctx.expect(isinstance(expiry_days, int) and 1 <= expiry_days <= 365,
               'sharing fixture requests a finite link expiry')
    detail = _upload(ctx, 'editor', 'private')
    _select(ctx, detail)
    try:
        created = _share_action(ctx, detail, 'shareHistoryLink', 'shared')
        inspected = _share_action(ctx, detail, 'inspectHistoryLink', 'shared')
        ctx.expect(created.get('expiresAt') == inspected.get('expiresAt'), 'inspection preserves confirmed expiry')
        expires = inspected.get('expiresAt')
        ctx.expect(bool(expires), 'campaign link has configured finite expiry')
        confirmed = datetime.fromisoformat(expires.replace('Z', '+00:00'))
        ctx.expect(confirmed.tzinfo is not None, 'provider expiry includes its timezone')
        expected = datetime.now(timezone.utc) + timedelta(days=expiry_days)
        ctx.expect(abs((confirmed - expected).total_seconds()) < 600,
                   'provider-confirmed expiry matches requested days within ten minutes')
        detail = ctx.call('history.detail', {'id': detail['id']})
        ctx.expect(detail.get('shareExpiresAt') == expires, 'History shows provider expiry')
        return detail
    except Exception:
        current = ctx.call('history.detail', {'id': detail['id']})
        if current.get('linkAvailable'):
            _revoke(ctx, current)
        raise


@recipe(*SCENARIOS['history-sharing'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=600)
def history_sharing(ctx):
    detail = _sharing_upload(ctx)
    try:
        with _default_browser(ctx):
            _browser(ctx, lambda: ctx.click('openHistoryLink', window='history'), detail)
    finally:
        _revoke(ctx, detail)


def _revoke(ctx, detail):
    before = detail.get('shareOperationId')
    ctx.click('revokeHistoryLink', window='history')
    ctx.frame('revoke-confirmation')
    # Escape is safe/default; proving this does not revoke the link.
    ctx.keys('Escape')
    after = ctx.call('history.detail', {'id': detail['id']})
    ctx.expect(after.get('shareOperationId') == before and after.get('linkAvailable') is True,
               'cancelled confirmation preserved share link')
    ctx.click('revokeHistoryLink', window='history')
    ctx.click('confirmDiscard', window='confirmAction')
    _share_result(ctx, detail, before, 'revoked')
    final = ctx.call('history.detail', {'id': detail['id']})
    ctx.expect(final.get('linkAvailable') is False and final.get('shareStatus') == 'revoked',
               'revoked link unavailable in History')
    ctx.frame('link-revoked')


@recipe(*SCENARIOS['revoke-confirmation'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=600)
def revoke_confirmation(ctx):
    detail = _sharing_upload(ctx)
    _revoke(ctx, detail)
    ctx.blocked('Revocation is confirmed by provider; fresh-browser negative verification needs the links API extension.',
                CORE_ISSUE)


def _destinations(ctx):
    _live(ctx)
    ctx.view('ui.show', {'view': 'destinations'})
    _click_named(ctx, _field(ctx, 'profile_label'), 'settings')
    _profile(ctx)


def _accessible(ctx, object_name):
    values = ctx.accessible({'objectName': object_name})
    if len(values) != 1 or not any(key in values[0] for key in ('text', 'enabled', 'checked')):
        ctx.blocked('Accessible objectName/state/text projection is unavailable for ' + object_name + '.',
                    CORE_ISSUE)
    return values[0]


def _setup_click(ctx, control, completion):
    ctx.click(control, window='settings')
    # Require an observed in-flight state so an old successful status cannot pass.
    ctx.wait(lambda: _accessible(ctx, control).get('enabled') is False, 'new setup action in flight')
    ctx.wait(lambda: _accessible(ctx, control).get('enabled') is True, 'setup action finished', timeout=180)
    ctx.expect(completion in _accessible(ctx, 'destinationStatus').get('text', ''),
               'fresh setup action confirmed success')
    ctx.frame('destination-' + control)


@recipe(*SCENARIOS['account-folder'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=240)
def account_folder(ctx):
    _destinations(ctx)
    _setup_click(ctx, 'testDestination', 'Connection successful.')


@recipe('destinations.proton-connect', needs=('live',), destinations=('proton',), desktop_drawn=True)
def proton_connect(ctx):
    _live(ctx)
    ctx.blocked('Connect/Reconnect starts sign-in; forbidden for the borrowed builder session. Read-only checks are separate.',
                CORE_ISSUE)


@recipe(*SCENARIOS['new-folder'], needs=('live',), destinations=('proton',), desktop_drawn=True, timeout=240)
def proton_folder(ctx):
    _destinations(ctx)
    previous = _profile(ctx)['settings']['folderUid']
    # Refresh completion has a dedicated nonsecret folder-status projection request.
    ctx.click('inspectProtonAccount', window='settings')
    ctx.wait(lambda: _accessible(ctx, 'inspectProtonAccount').get('enabled') is False, 'folder refresh in flight')
    ctx.wait(lambda: _accessible(ctx, 'inspectProtonAccount').get('enabled') is True, 'folder refresh complete', timeout=180)
    ctx.expect(_profile(ctx)['settings']['folderUid'] == previous, 'refresh preserved selected folder')
    ctx.expect('Folders loaded.' in _accessible(ctx, 'settingsStatus').get('text', ''),
               'folder refresh succeeded rather than just re-enabling controls')
    ctx.click('createProtonFolder', window='settings')
    ctx.wait(lambda: ctx.accessible({'role': 'dialog', 'name': 'New Proton folder'}), 'new folder input dialog')
    ctx.keys('Ctrl+A')
    new_name = _field(ctx, 'new_folder_name')
    ctx.type_text(new_name)
    ctx.keys('Return')
    ctx.wait(lambda: _profile(ctx)['settings'].get('folderName') == new_name, 'new folder selected and saved', timeout=180)
    ctx.expect(_profile(ctx)['settings'].get('folderUid') not in ('', previous), 'created folder has new provider identity')
    _live(ctx)['folder_name'] = new_name
    ctx.frame('proton-new-folder')


@recipe('destinations.proton-sharing', needs=('live',), destinations=('proton',), desktop_drawn=True)
def proton_sharing(ctx):
    _destinations(ctx)
    ctx.click('protonKeepPrivate', window='settings')
    ctx.wait(lambda: _profile(ctx)['settings'].get('shareByDefault') is False, 'Keep private saved')
    ctx.click('protonCreateShareLink', window='settings')
    ctx.wait(lambda: _profile(ctx)['settings'].get('shareByDefault') is True, 'Create a share link saved')
    advanced = _accessible(ctx, 'advancedSettings')
    if 'checked' not in advanced:
        ctx.blocked('Advanced disclosure checked state is unavailable.', CORE_ISSUE)
    if not advanced['checked']:
        ctx.click('advancedSettings', window='settings')
    for control, value, field in (('protonExpiration', '1', 'linkExpirationDays'),
                                  ('protonRequestTimeout', '120', 'requestTimeoutMs')):
        ctx.click(control, window='settings')
        ctx.keys('Ctrl+A')
        ctx.type_text(value)
        ctx.keys('Tab')
        expected = 120000 if field == 'requestTimeoutMs' else 1
        ctx.wait(lambda: _profile(ctx)['settings'].get(field) == expected, field + ' saved')
    ctx.click('clearProtonPassword', window='settings')
    ctx.wait(lambda: _profile(ctx)['settings'].get('sharePasswordReference') is None, 'no link password selected')
    ctx.frame('proton-sharing-settings')
    ctx.blocked('Password Store needs private masked input and nonsecret setup completion support; never type a secret via retained input.',
                CORE_ISSUE)


@recipe('destinations.b2-links', needs=('live',), destinations=('b2',), desktop_drawn=True)
def b2_links(ctx):
    _destinations(ctx)
    for key, mode in (('Home', 'public'), ('End', 'authorized')):
        ctx.click('b2LinkMode', window='settings')
        ctx.keys(key, 'Return')
        ctx.wait(lambda: _profile(ctx)['settings'].get('linkMode') == mode, 'B2 link access saved')
    ctx.click('b2LinkDays', window='settings')
    ctx.keys('Ctrl+A')
    ctx.type_text('1')
    ctx.keys('Tab')
    ctx.wait(lambda: _profile(ctx)['settings'].get('linkValidityDays') == 1, 'B2 expiry saved')
    ctx.frame('b2-link-settings')


@recipe(*SCENARIOS['cleanup'], needs=('live',), destinations=CLOUD, desktop_drawn=True, timeout=420)
def cleanup(ctx):
    _live(ctx)
    if ctx.destination == 'proton':
        ctx.blocked('Proton SDK has no guarded delete; delete.prepare refuses before opening confirmation. No cleanup upload created.',
                    CORE_ISSUE)
    owned = list(dict.fromkeys(_live(ctx).get('owned_history_ids', [])))
    if not owned:
        owned = [_upload(ctx)['id']]
    for history_id in owned:
        detail = ctx.call('history.detail', {'id': history_id})
        if detail.get('remoteStatus') not in ('removed', 'already_missing'):
            _delete_one(ctx, detail)
    ctx.blocked('Origin deletion confirmed; independent fresh browser/object-absence check requires links negative-verification API.',
                CORE_ISSUE)


def _delete_one(ctx, detail):
    _select(ctx, detail)
    ctx.click('deleteHistoryRemote', window='history')
    ctx.frame('delete-confirmation')
    # Preparation read from the dialog is the app's actual ordered selection.
    ctx.call('ui.history', {'action': 'copyDeletionDetails'})
    import json
    preparation = json.loads(ctx.clipboard_text())
    ctx.expect([t.get('historyId') for t in preparation.get('targets', [])] == [detail['id']],
               'confirmation names exact owned history ID')
    ctx.expect(bool(preparation.get('acknowledgement')), 'app prepared deletion acknowledgement')
    ctx.click('confirmActionButton', window='confirmAction')
    changed = ctx.wait(lambda: _when(ctx.call('history.detail', {'id': detail['id']}),
                                    lambda d: d.get('remoteStatus') in ('removed', 'already_missing', 'failed',
                                                                      'unknown', 'changed', 'hidden_version_retained',
                                                                      'version_retained', 'unsupported_safe_removal')),
                       'destination deletion outcome', timeout=180)
    ctx.expect(changed.get('remoteStatus') in ('removed', 'already_missing'), 'app confirmed remote deletion')
    ctx.expect(changed.get('linkAvailable') is False, 'deleted remote file has no usable history link')
    ctx.expect(bool(changed.get('deleteOperationId')), 'durable deletion operation ID retained')
    ctx.note('deleteOperationId=' + changed['deleteOperationId'])
    ctx.frame('deleted-through-history')


@recipe(*SCENARIOS['s3-access'], needs=('live',), destinations=('s3',), desktop_drawn=True, timeout=1200)
def s3_access(ctx):
    _destinations(ctx)
    profile = _profile(ctx)
    settings = profile['settings']
    original_acl = settings['aclMode']
    original_domain = settings['publicUrlBase']
    custom_domain = _field(ctx, 'custom_domain')
    ctx.expect(urlsplit(custom_domain).scheme == 'https', 'configured custom link domain uses HTTPS')
    # Existing bucket policy is never modified; external service configuration
    # must already support both modes in this isolated prefix.
    ctx.expect(_field(ctx, 'public_read_supported') is True and _field(ctx, 'bucket_policy_supported') is True,
               'fixture destination supports both approved access modes')
    try:
        for key, mode in (('End', 'public-read'), ('Home', 'omit')):
            _destinations(ctx)
            ctx.click('s3AclMode', window='settings')
            ctx.keys(key, 'Return')
            ctx.wait(lambda: _profile(ctx)['settings']['aclMode'] == mode, 'S3 access request saved')
            with _default_browser(ctx):
                detail = _upload(ctx)
                _select(ctx, detail)
                _browser(ctx, lambda: ctx.click('openHistoryLink', window='history'), detail)
        _destinations(ctx)
        ctx.click('s3PublicUrlBase', window='settings')
        ctx.keys('Ctrl+A')
        ctx.type_text(custom_domain)
        ctx.keys('Tab')
        ctx.wait(lambda: _profile(ctx)['settings']['publicUrlBase'] == custom_domain, 'custom link domain saved')
        with _default_browser(ctx):
            detail = _upload(ctx)
            _select(ctx, detail)
            # Domain verified separately without logging its URL/path/query.
            ctx.expect(urlsplit(detail['receipt']['directUrl']).hostname == urlsplit(custom_domain).hostname,
                       'uploaded receipt uses configured custom domain')
            _browser(ctx, lambda: ctx.click('openHistoryLink', window='history'), detail,
                     expected_host=urlsplit(custom_domain).hostname)
    finally:
        _destinations(ctx)
        ctx.click('s3AclMode', window='settings')
        ctx.keys('Home' if original_acl == 'omit' else 'End', 'Return')
        ctx.click('s3PublicUrlBase', window='settings')
        ctx.keys('Ctrl+A')
        if original_domain:
            ctx.type_text(original_domain)
        else:
            ctx.keys('Backspace')
        ctx.keys('Tab')
        ctx.wait(lambda: _profile(ctx)['settings']['aclMode'] == original_acl and
                 _profile(ctx)['settings']['publicUrlBase'] == original_domain, 'S3 link settings restored')
