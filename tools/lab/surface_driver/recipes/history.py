"""History and shared shell recipes; no input, runner or fixture implementation.

Fixture requests (all data is synthetic, private and reset before each recipe):
* history-row: core descriptor with id, history_id, operation_id, row.
* launcher: requires ctx.cli to dispatch leading flags to putgd (core request).
* history_rows: {image: {id, name}, category_counts: {category: count}}. The
  image name uniquely matches one row, backed by a retained PNG/local copy.
* history_pair: {ids: [id, id], bytes: int}; exactly two separate operations.
* history_paging: {total: int}; >50 separate rows, default page size 50.
* history_retry_upload / history_retry_local: {id, name, operation_id}; use the
  tray_progress.py failed-upload/local-save setup, with the fault removed.
* history_active_uploads: {operation_ids: [id, id]}; stalled loopback S3 uploads.
* history_delete: {id, name}; disposable loopback S3 receipt with an unchanged local copy.
* history_saved_copy: {id, name, path}; completed save-as in the private profile.

Each mutating fixture must be fresh per recipe, not merely cached across a run.
No live endpoint, Proton account, user history or secret is accepted here.
Missing fixtures must block in the core; do not emulate their success.
"""
from __future__ import annotations

import json
from decimal import Decimal, ROUND_HALF_UP

from tools.lab.surface_driver.api import RecipeFailed, recipe

LIST = '01-history-list'
GALLERY = '02-history-gallery'
COMPACT = '10-history-compact'


def _result(value):
    """Resident/MCP replies may be envelopes; never discard a reported failure."""
    if 'ok' in value:
        if value['ok'] is not True:
            raise RecipeFailed('resident/MCP request failed: ' + value.get('error', {}).get('code', 'unknown'))
        return value['result']
    return value


def _state(ctx):
    return _result(ctx.call('ui.history', {'action': 'inspect'}))


def _frame(ctx, label, reference=LIST, window='history'):
    ctx.frame(label, window=window, reference=reference)


def _open(ctx):
    ctx.view('ui.show', {'view': 'history'})
    ctx.view('ui.history', {'action': 'clearFilters', 'scope': 'all'})
    ctx.wait(lambda: not _state(ctx)['busy'], 'cleared History query settles before selection changes')
    ctx.view('ui.history', {'action': 'closePreview'})
    ctx.view('ui.history', {'action': 'closeDetails'})
    ctx.view('ui.history', {'action': 'clearSelection'})
    ctx.wait(lambda: not _state(ctx)['busy'], 'History query settles')
    ctx.click('historyListMode', window='history')
    ctx.expect(not _state(ctx)['gridMode'], 'known list view')


def _search(ctx, text):
    ctx.click('historySearch', window='history')
    ctx.keys('Ctrl+A')
    if text:
        ctx.type_text(text)
    else:
        ctx.keys('Backspace')
    ctx.keys('Return')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['search'] == text,
             'search is applied by real input')


def _focus_items(ctx):
    """Tab to the list and observe widget focus before sending list shortcuts."""
    query = {'owned': True, 'visible': True, 'showing': True,
             'pid': ctx.geometry('history')['pid'], 'name': 'History items and upload activity',
             'states': {'focused': True}}
    for _ in range(20):
        nodes = ctx.accessible(query)
        if len(nodes) == 1:
            ctx.note('History list keyboard focus verified through native accessibility')
            return
        if len(nodes) > 1:
            ctx.blocked('History list has ambiguous native keyboard-focus witnesses.', issue=675)
        ctx.keys('Tab')
    ctx.blocked('History list did not acquire verified native keyboard focus within 20 Tab steps.', issue=675)


def _row(ctx, fixture='history-row'):
    data = ctx.fixture(fixture)
    row = data.row if fixture == 'history-row' else data['image'] if fixture == 'history_rows' else data
    _open(ctx)
    detail = _result(ctx.call('history.detail', {'id': row['id']}))
    ctx.expect(detail['id'] == row['id'] and detail['name'] == row['name'], 'fixture has the expected identity')
    _search(ctx, row['name'])
    ctx.wait(lambda: _state(ctx)['rows'] == 1, 'unique fixture row is shown')
    _focus_items(ctx)
    ctx.keys('Home')
    ctx.wait(lambda: row['id'] in _state(ctx)['selectedIds'], 'fixture row is selected with real input')
    return row


def _gallery(ctx):
    ctx.click('historyGridMode', window='history')
    ctx.wait(lambda: _state(ctx)['gridMode'], 'gallery mode')


def _details(ctx):
    # In split-list mode details are already beside the row; gallery provides
    # an observable open/close transition at both compact and full widths.
    _gallery(ctx)
    _focus_items(ctx)
    ctx.keys('Home', 'Return')
    ctx.wait(lambda: _state(ctx)['detailsOpen'] and _state(ctx)['inspectorVisible'], 'selected details open')


def _size(ctx, byte_count, caption='Size'):
    # UI uses traditional labels with binary arithmetic. Assert the visible
    # accessible value as well as the authoritative byte count.
    if byte_count < 1024:
        label = str(byte_count) + ' B'
    else:
        unit, suffix = (1024, 'KB') if byte_count < 1024**2 else ((1024**2, 'MB') if byte_count < 1024**3 else (1024**3, 'GB'))
        precision = (1 if byte_count < 10 * 1024 else 0) if suffix == 'KB' else 2
        value = (Decimal(byte_count) / unit).quantize(Decimal(10) ** -precision, rounding=ROUND_HALF_UP)
        number = str(value)
        if suffix != 'KB' and '.' in number:
            number = number.rstrip('0').rstrip('.')
        label = number + ' ' + suffix
    expected = caption + ': ' + label
    rows = ctx.wait(lambda: ctx.accessible({'owned': True, 'visible': True, 'showing': True,
                                          'pid': ctx.geometry('history')['pid'], 'name': caption + ': *'}), 'visible binary size summary')
    ctx.expect(any(row['name'] == expected or row['name'].startswith(expected + ' · ') for row in rows),
               'visible size uses 1024 arithmetic and KB/MB/GB labels: ' + expected)


def _cli(ctx, *args):
    reply = ctx.cli(*args)
    ctx.expect(reply['returncode'] == 0, 'CLI succeeded: ' + args[0])
    return _result(json.loads(reply['stdout']))


@recipe('history.search', needs=('history-row',))
def search(ctx):
    row = ctx.fixture('history-row').row
    _open(ctx)
    _frame(ctx, 'before')
    _search(ctx, row['name'])
    ctx.expect(_state(ctx)['rows'] == 1, 'unique search finds exactly one row')
    _search(ctx, 'surface-history-no-such-row-828c6875')
    ctx.wait(lambda: _state(ctx)['rows'] == 0, 'unmatched search shows no rows')
    _frame(ctx, 'no-matches', '43-history-no-matches')
    _search(ctx, '')
    ctx.expect(_state(ctx)['total'] >= 1, 'cleared search restores rows')
    _frame(ctx, 'after')


@recipe('history.categories', needs=('history_rows',))
def categories(ctx):
    _open(ctx)
    _frame(ctx, 'before')
    # The disabled separator is skipped by keyboard navigation.
    choices = (('', ''), ('images', ''), ('videos', ''), ('text+other', ''), ('', 'failed'))
    counts = ctx.fixture('history_rows')['category_counts']
    for index, (category, upload) in enumerate(choices):
        ctx.click('historyCategories', window='history')
        ctx.keys('Home', *(['Down'] * index))
        ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['category'] == category
                 and _state(ctx)['query']['uploadStatus'] == upload, 'sidebar applies ' + str(index))
        ctx.expect(_state(ctx)['total'] == counts['failed' if upload else category], 'category total agrees with fixture')
        _frame(ctx, 'category-' + str(index))
    ctx.click('historyCategories', window='history')
    ctx.keys('Home')
    ctx.wait(lambda: _state(ctx)['query']['category'] == '' and _state(ctx)['query']['uploadStatus'] == '', 'All items resets Failed')
    _frame(ctx, 'after')


@recipe('history.clear-filters', needs=('history_rows',))
def clear_filters(ctx):
    row = _row(ctx, 'history_rows')
    _frame(ctx, 'before')
    ctx.click('historyFiltersExpanded', window='history')
    ctx.click('text:Add a history filter', window='history')
    ctx.click('text:Upload state', window='historyAddFilterMenu')
    ctx.click_accessible({'owned': True, 'visible': True, 'name': 'Upload state', 'role': 'combo box'})
    ctx.keys('Home', 'Down', 'Return')
    ctx.wait(lambda: bool(_state(ctx)['query']['uploadStatus']), 'upload filter applied')
    _frame(ctx, 'filtered', '40-history-filters-active')
    ctx.click('text:Clear filters', window='history')
    ctx.wait(lambda: not _state(ctx)['activeFilters'], 'advanced filters cleared')
    ctx.expect(_state(ctx)['query']['search'] == row['name'], 'Clear filters preserves search')
    _frame(ctx, 'after')






def _click_preview(ctx, button=False):
    # Both are exposed as buttons. The large image accepts keyboard focus;
    # its small expand glyph deliberately has Qt::NoFocus.
    ctx.click_accessible({'owned': True, 'visible': True, 'showing': True,
                          'pid': ctx.geometry('history')['pid'], 'name': 'Expand preview',
                          'states': {'focusable': not button}})


@recipe('history.preview', 'history.expand-preview', needs=('history-row',))
def preview(ctx):
    _row(ctx)
    ctx.wait(lambda: _state(ctx)['canExpandPreview'], 'retained image preview loads')
    _frame(ctx, 'before')
    _click_preview(ctx)
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'preview click expands')
    _frame(ctx, 'expanded-click', '68-history-expanded-preview', 'historyExpandedPreview')
    ctx.focus('historyExpandedPreview', raise_window=False)
    ctx.keys('Esc')
    ctx.focus('history', raise_window=False)
    ctx.wait(lambda: not _state(ctx)['previewExpanded'], 'Escape closes preview')
    ctx.keys('Space')
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'Space on focused preview expands')
    ctx.click('text:Close preview', window='historyExpandedPreview')
    ctx.wait(lambda: not _state(ctx)['previewExpanded'], 'Close preview closes dialog')
    ctx.focus('history', raise_window=False)
    _click_preview(ctx, button=True)
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'expand glyph opens preview')
    ctx.focus('historyExpandedPreview', raise_window=False)
    ctx.keys('Esc')
    ctx.focus('history', raise_window=False)
    _frame(ctx, 'after')




@recipe('history.receipt-details', needs=('history-row',))
def receipt_details(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('historyReceiptExpanded', window='history')
    ctx.click('text:Advanced original receipt details', window='history')
    ctx.keys('Ctrl+A', 'Ctrl+C')
    detail = json.loads(ctx.clipboard_text())
    ctx.expect(detail['id'] == row['id'], 'displayed advanced receipt has exact history identity')
    authoritative = _result(ctx.call('history.detail', {'id': row['id']}))
    ctx.expect(detail['bytes'] == authoritative['bytes'], 'advanced receipt byte count matches history detail')
    _size(ctx, authoritative['bytes'])
    _frame(ctx, 'after', '60-history-previous-link-details')
    ctx.click('historyReceiptExpanded', window='history')


@recipe('history.multi-select', needs=('history_pair',))
def multi_select(ctx):
    data = ctx.fixture('history_pair')
    _open(ctx)
    ctx.expect(_state(ctx)['rows'] == 2, 'fixture has two separate operation rows')
    _frame(ctx, 'before')
    _focus_items(ctx)
    ctx.keys('Home', 'Shift+Down')
    state = ctx.wait(lambda: _state(ctx) if _state(ctx)['selectedRows'] == 2 else None, 'Shift selects both rows')
    ctx.expect(set(state['selectedIds']) == set(data['ids']), 'only exact two fixture IDs selected')
    ctx.expect(state['selectionSummary']['bytes'] == data['bytes'], 'selection summary adds bytes exactly')
    _size(ctx, data['bytes'], 'Total size')
    _frame(ctx, 'selected', '41a-history-selection-inspector')
    ctx.click('historySelectionMore', window='history')
    ctx.click('text:Clear selection', window='historySelectionMore')
    ctx.wait(lambda: _state(ctx)['selectedRows'] == 0, 'Clear selection menu clears both rows')
    _frame(ctx, 'after')


@recipe('history.paging', needs=('history_paging',))
def paging(ctx):
    _open(ctx)
    before = _state(ctx)
    ctx.expect(before['query']['offset'] == 0 and before['query']['limit'] == 50, 'known first page')
    ctx.expect(before['total'] == ctx.fixture('history_paging')['total'] > 50, 'fixture spans pages')
    _frame(ctx, 'before')
    ctx.click('text:Next', window='history')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['offset'] == 50, 'Next advances one page')
    _frame(ctx, 'next')
    ctx.click('text:Previous', window='history')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['offset'] == 0, 'Previous restores first page')
    _frame(ctx, 'after')


@recipe('history.key-menu', needs=('history-row',))
def key_menu(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    for key in ('Menu', 'Shift+F10'):
        ctx.keys(key)
        ctx.frame('menu-' + key)
        ctx.click('text:Copy details', window='historyRowMenu')
        detail = json.loads(ctx.clipboard_text())
        ctx.expect(any(item['id'] == row['id'] for item in detail['records']), 'keyboard row menu copies selected receipt')
    _frame(ctx, 'after')


@recipe('history.inspector-more', needs=('history-row', 'file-dialog-fallback'))
def inspector_more(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('historyInspectorMore', window='history')
    ctx.frame('menu')
    ctx.click('text:Save a copy…', window='historyInspectorMenu')
    chooser = _result(ctx.call('ui.fileDialog', {'dialog': 'historySaveAs', 'action': 'inspect', 'id': row['id']}))
    ctx.expect(chooser['open'] is True, 'More file actions opens Save a copy chooser')
    # Bind the actual chooser before sending its cancellation key.
    ctx.focus('historySaveDialog', raise_window=False)
    ctx.keys('Esc')
    ctx.focus('history', raise_window=False)
    ctx.wait(lambda: _result(ctx.call('ui.fileDialog', {'dialog': 'historySaveAs', 'action': 'inspect', 'id': row['id']}))['open'] is False, 'chooser canceled')
    _frame(ctx, 'after')


@recipe('history.saved-copy-strip', needs=('history_saved_copy',))
def saved_copy_strip(ctx):
    data = _row(ctx, 'history_saved_copy')
    state = _state(ctx)
    ctx.expect(state['savedCopyVisible'] and state['savedPath'] == data['path'], 'known completed saved copy')
    _frame(ctx, 'before', '69-history-saved-copy')
    ctx.click('copyHistorySavedPath', window='history')
    ctx.expect(ctx.clipboard_text() == data['path'], 'Copy path copies exact local path')
    ctx.click('showHistorySavedCopy', window='history')
    ctx.wait(lambda: _state(ctx)['savedCopyFolder'].get('opened'), 'saved copy folder opens')
    _frame(ctx, 'folder', '69-history-saved-copy')
    ctx.click('dismissHistorySavedCopy', window='history')
    ctx.wait(lambda: not _state(ctx)['savedCopyVisible'], 'dismiss hides saved-copy feedback')
    ctx.expect(_state(ctx)['savedPath'] == data['path'], 'dismiss preserves saved file path')
    _frame(ctx, 'after', '69b-history-dismissed-copy')


def _retry(ctx, fixture, local=False):
    row = _row(ctx, fixture)
    before = _result(ctx.call('operation.inspect', {'id': row['operation_id']}))
    _frame(ctx, 'local-before' if local else 'before', '50a-history-failure-inspector')
    ctx.click('text:Retry local save' if local else 'text:Retry upload', window='history')
    def saved():
        detail = _result(ctx.call('history.detail', {'id': row['id']}))
        return detail if (detail['local']['status'] == 'saved' if local else detail['uploadStatus'] == 'uploaded') else None
    detail = ctx.wait(saved, 'retry settles on original history ID', timeout=60)
    terminal = ('completed', 'partial', 'failed', 'cancelled', 'interrupted') if local else ('completed', 'partial')
    after = ctx.wait(lambda: (op if (op := _result(ctx.call('operation.inspect', {'id': row['operation_id']})))['state'] in terminal else None),
                     'retried stage leaves its running observation', timeout=30)
    if local:
        ctx.expect(after['state'] == before['state'], 'local save retry preserves its durable operation result')
    ctx.expect(after['id'] == before['id'], 'retry preserves operation identity')
    ctx.expect(detail['id'] == row['id'], 'retry preserves history identity')
    _frame(ctx, 'after' if local else 'upload-after')


@recipe('history.retry-upload', needs=('history_retry_upload', 'history_retry_local'), destinations=('local', 's3'), timeout=180)
def retry_upload(ctx):
    _retry(ctx, 'history_retry_upload')
    _retry(ctx, 'history_retry_local', local=True)


@recipe('history.active-banner', needs=('history_active_uploads',), destinations=('local', 's3'), timeout=180)
def active_banner(ctx):
    data = ctx.fixture('history_active_uploads')
    _open(ctx)
    ctx.expect(_state(ctx)['activeOperationCount'] == len(data['operation_ids']) >= 2, 'known stalled loopback operations')
    _frame(ctx, 'before', 'history-many-active')
    ctx.click('text-glob:* more active', window='history')
    ctx.wait(lambda: _state(ctx)['query']['uploadStatus'] == 'uploading', 'More active filters running uploads')
    ctx.click('text-glob:Cancel this upload:*', window='history')
    ctx.wait(lambda: _state(ctx)['activeOperationCount'] < len(data['operation_ids']), 'Cancel this upload stops an active operation', timeout=75)
    states = [_result(ctx.call('operation.inspect', {'id': item})) for item in data['operation_ids']]
    ctx.expect(any(item['state'] == 'cancelled' for item in states), 'one exact fixture operation canceled')
    _frame(ctx, 'after', 'history-many-active')


@recipe('history.cancel-all-dialog', 'confirm.history-cancel-all', needs=('history_active_uploads',), destinations=('local', 's3'), timeout=240)
def cancel_all(ctx):
    ids = ctx.fixture('history_active_uploads')['operation_ids']
    _open(ctx)
    _frame(ctx, 'before', 'history-many-active')
    ctx.click('text-glob:Cancel all (*)…', window='history')
    state = ctx.wait(lambda: (s if (s := _state(ctx))['cancelAllConfirmation']['open'] else None), 'cancel-all confirmation opens')['cancelAllConfirmation']
    _frame(ctx, 'keep-question', 'history-cancel-all-confirmation', 'historyCancelAllConfirmation')
    ctx.expect(state['open'] and set(state['operationIds']) == set(ids), 'confirmation captures exact active identities')
    ctx.click('text:Keep uploading', window='historyCancelAllConfirmation')
    ctx.wait(lambda: not _state(ctx)['cancelAllConfirmation']['open'], 'Keep uploading dismisses confirmation')
    ctx.expect(_state(ctx)['activeOperationCount'] == len(ids), 'Keep uploading leaves all operations active')
    ctx.click('text-glob:Cancel all (*)…', window='history')
    second = ctx.wait(lambda: (s if (s := _state(ctx))['cancelAllConfirmation']['open'] else None), 'second cancel-all confirmation opens')
    ctx.expect(set(second['cancelAllConfirmation']['operationIds']) == set(ids), 'second confirmation captures same identities')
    ctx.click('text-glob:Cancel * uploads', window='historyCancelAllConfirmation')
    ctx.wait(lambda: all(_result(ctx.call('operation.inspect', {'id': item}))['state'] == 'cancelled' for item in ids), 'all confirmed uploads cancel', timeout=75)
    ctx.expect(_state(ctx)['activeOperationCount'] == 0, 'banner clears canceled uploads')
    _frame(ctx, 'after')


@recipe('history.delete-remote', 'confirm.history-delete', needs=('history_delete',), destinations=('local', 's3'))
def delete_remote(ctx):
    row = _row(ctx, 'history_delete')
    original = _result(ctx.call('history.detail', {'id': row['id']}))
    _frame(ctx, 'before')
    ctx.click('text:Delete from destination…', window='history')
    ctx.frame('question', window='historyConfirmAction')
    ctx.click('text:Show the full deletion preparation', window='historyConfirmAction')
    ctx.click('text:Copy technical details', window='Deletion technical details')
    preparation = json.loads(ctx.clipboard_text())
    ctx.frame('technical-details', window='Deletion technical details')
    ctx.expect([target['historyId'] for target in preparation['targets']] == [row['id']], 'dialog prepares only exact fixture identity')
    ctx.keys('Esc')
    ctx.click('text:Cancel', window='historyConfirmAction')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['remoteStatus'] == original['remoteStatus'], 'Cancel preserves remote state')
    ctx.click('text:Delete from destination…', window='history')
    ctx.click('text-glob:Delete * file*', window='historyConfirmAction')
    final = ctx.wait(lambda: (d if (d := _result(ctx.call('history.detail', {'id': row['id']})))['remoteStatus'] in ('removed', 'already_missing') else None), 'recorded loopback object deleted', timeout=60)
    ctx.expect(final['id'] == original['id'] and final['local'] == original['local'], 'deletion preserves history and local copy')
    _frame(ctx, 'after', '48-history-deletion-recovery')


@recipe('history.more-menu', 'confirm.history-clear', needs=('history_rows',))
def clear_history(ctx):
    _open(ctx)
    before = _state(ctx)['total']
    ctx.expect(before > 0, 'private fixture history is populated')
    local_before = _result(ctx.call('history.detail', {'id': ctx.fixture('history_rows')['image']['id']}))['local']
    _frame(ctx, 'before')
    for action in ('Clear temporary files…', 'Clear history records…'):
        ctx.click('historyManagementExpanded', window='history')
        ctx.frame('more-menu-' + ('cache' if 'temporary' in action else 'records'))
        ctx.click('text:' + action, window='historyManagementMenu')
        ctx.frame('question-' + ('cache' if 'temporary' in action else 'records'), window='historyConfirmAction')
        ctx.click('text:Cancel', window='historyConfirmAction')
        ctx.expect(_state(ctx)['total'] == before, 'Cancel preserves history')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear temporary files…', window='historyManagementMenu')
    ctx.click('text:Clear temporary files', window='historyConfirmAction')
    ctx.wait(lambda: not _state(ctx)['busy'], 'temporary file cleanup finishes')
    ctx.expect(_state(ctx)['total'] == before, 'cache cleanup preserves history records')
    ctx.expect(_result(ctx.call('history.detail', {'id': ctx.fixture('history_rows')['image']['id']}))['local'] == local_before, 'cache cleanup preserves local-copy receipt')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear history records…', window='historyManagementMenu')
    ctx.click('text:Clear history records', window='historyConfirmAction')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['total'] == 0, 'confirmed clear removes local history')
    _frame(ctx, 'after')
    ctx.review('Catalog has no empty-history reference or confirmation controls dump; inspect saved frames for empty state and both safe defaults.')


def _header_point(ctx, box):
    """Find blank chrome from live accessible controls, on either display server."""
    query = {'owned': True, 'visible': True, 'pid': box['pid'],
             'within': [box[k] for k in ('x', 'y', 'width', 'height')]}
    def layout():
        nodes = ctx.accessible(query)
        anchors = [n for n in nodes if n.get('name') == 'Application menu' and n.get('rect')]
        if len(anchors) > 1:
            ctx.blocked('History has ambiguous live Application menu anchors.', issue=675)
        return (nodes, anchors[0]) if anchors else None
    nodes, anchor = ctx.wait(layout, 'accessible History header')
    _, top, _, height = anchor['rect']
    y = top + height / 2
    intervals = [(box['x'] + 2, box['x'] + box['width'] - 2)]
    for node in nodes:
        rect = node.get('rect')
        if not rect or node.get('role') in ('frame', 'panel', 'filler', 'root pane', 'application'):
            continue
        x, top, width, height = rect
        if top <= y < top + height:
            intervals = [part for left, right in intervals
                         for part in ((left, min(right, x - 2)), (max(left, x + width + 2), right))
                         if part[1] > part[0]]
    if not intervals or max(right - left for left, right in intervals) < 16:
        ctx.blocked('History has no verified blank header drag target.', issue=675)
    left, right = max(intervals, key=lambda v: v[1] - v[0])
    point = ((left + right) / 2, y)
    return tuple(round((point[i] - box.get(('screen_x', 'screen_y')[i], 0))
                       * box.get('scale', 1)) for i in (0, 1))


def _menu_names(ctx):
    return {n.get('name') for n in ctx.accessible({'owned': True, 'visible': True, 'role': 'menu item'})}


def _application_menu(ctx):
    ctx.click('applicationMenu', window='history')
    expected = {'Capture actions', 'Check for updates', 'About put.gd', 'Quit put.gd'}
    ctx.wait(lambda: expected <= _menu_names(ctx), 'application menu exposes all four commands')


@recipe('shell.app-menu', needs=('history-row',))
def application_menu(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    _application_menu(ctx)
    ctx.frame('menu')
    ctx.keys('Esc')
    ctx.wait(lambda: 'Quit put.gd' not in _menu_names(ctx), 'Escape dismisses application menu')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'],
               'dismissing menu preserves history and resident')
    _frame(ctx, 'after')


@recipe('shell.app-menu-capture-help', needs=('history-row',))
def capture_help(ctx):
    _row(ctx)
    _frame(ctx, 'before')
    _application_menu(ctx)
    ctx.click('text:Capture actions', window='applicationMenuPopup')
    ctx.wait(lambda: ctx.accessible({'owned': True, 'visible': True, 'name': 'Capture actions'}),
             'Capture actions help is visible')
    ctx.frame('help', window='captureHelp')
    ctx.click('text:Close', window='captureHelp')
    ctx.wait(lambda: not ctx.accessible({'owned': True, 'visible': True, 'name': 'Help'}),
             'Close dismisses capture help')
    ctx.call('ui.viewState')
    _frame(ctx, 'after')


@recipe('shell.header-context-menu', needs=('history-row',))
def header_context_menu(ctx):
    _row(ctx)
    _frame(ctx, 'before')
    ctx.click_at(*_header_point(ctx, ctx.geometry('history')), button='right')
    ctx.wait(lambda: {'Minimize', 'Maximize', 'Close'} <= _menu_names(ctx),
             'header context menu exposes the native window commands')
    ctx.frame('menu')
    ctx.keys('Esc')
    ctx.wait(lambda: 'Minimize' not in _menu_names(ctx), 'Escape dismisses header menu')
    ctx.call('ui.viewState')
    _frame(ctx, 'after')


@recipe('shell.header-drag', needs=('history-row',))
def header_drag(ctx):
    _row(ctx)
    box = ctx.geometry('history')
    _frame(ctx, 'before')
    start = _header_point(ctx, box)
    ctx.expect(box['x'] <= start[0] < box['x'] + box['width']
               and box['y'] <= start[1] < box['y'] + box['height'], 'header hit point is inside shell')
    ctx.drag(start, (start[0] + 64, start[1] + 48))
    moved = ctx.wait(lambda: (g if ((g := ctx.geometry('history'))['x'], g['y']) != (box['x'], box['y']) else None), 'real header drag moves window')
    ctx.expect((moved['width'], moved['height']) == (box['width'], box['height']), 'drag preserves size')
    ctx.call('ui.viewState')
    _frame(ctx, 'after')
    ctx.drag((start[0] + moved['x'] - box['x'], start[1] + moved['y'] - box['y']), start)


@recipe('shell.edge-resize', 'shell.minimum-size', needs=('history-row',))
def edge_resize(ctx):
    _row(ctx)
    box = ctx.geometry('history')
    _frame(ctx, 'before')
    edge = (box['x'] + box['width'] - 1, box['y'] + box['height'] - 1)
    ctx.drag(edge, (edge[0] - 600, edge[1] - 500))
    resized = ctx.wait(lambda: (g if ((g := ctx.geometry('history'))['width'], g['height']) != (box['width'], box['height']) else None), 'edge drag resizes shell')
    scale = box.get('scale', 1)
    ctx.expect(resized['width'] == round(1060 * scale) and resized['height'] == round(680 * scale), 'shell respects 1060 by 680 logical minimum')
    ctx.call('ui.viewState')
    _frame(ctx, 'after', COMPACT)
    new_edge = (resized['x'] + resized['width'] - 1, resized['y'] + resized['height'] - 1)
    ctx.drag(new_edge, edge)


@recipe('shell.window-maximize', needs=('history-row',))
def maximize(ctx):
    _row(ctx)
    original = ctx.geometry('history')
    _frame(ctx, 'before')
    ctx.click('windowMaximize', window='history')
    enlarged = ctx.wait(lambda: (g if (g := ctx.geometry('history'))['width'] > original['width'] else None), 'Maximize enlarges shell')
    ctx.expect(enlarged['height'] >= original['height'], 'maximized height grows')
    ctx.call('ui.viewState')
    _frame(ctx, 'maximized')
    ctx.click('windowMaximize', window='history')
    ctx.wait(lambda: ctx.geometry('history') == original, 'Restore returns exact original geometry')
    _frame(ctx, 'after')


def _main_windows(ctx, include_hidden=False):
    """Inspect only the private family's exact main-window title."""
    key = 'history_hidden_window_query' if include_hidden else 'history_window_query'
    return ctx.windows(ctx.profile.get(key, {'title': r'^put\.gd$', 'include_hidden': include_hidden}))


def _minimized(window):
    """X11 inventory and native AT-SPI use different minimized-state names."""
    states = window.get('states', {})
    return bool(states.get('minimized') or states.get('iconified'))


@recipe('shell.window-minimize', needs=('history-row',))
def minimize(ctx):
    """Minimize with the chrome button and observe the compositor state."""
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('windowMinimize', window='history')
    try:
        ctx.wait(lambda: any(_minimized(w) for w in _main_windows(ctx, True)),
                 'window manager confirms minimized shell')
        ctx.frame('minimized')
    finally:
        ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: any(not _minimized(w) for w in _main_windows(ctx)),
             'resident restores the minimized window')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'],
               'resident and history survive minimize')
    _frame(ctx, 'after')


@recipe('shell.window-close', needs=('history-row',))
def close_window(ctx):
    """Close the shell and prove the resident still serves its original row."""
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('windowClose', window='history')
    try:
        ctx.wait(lambda: not _main_windows(ctx), 'closed shell has no visible main window')
        ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'],
                   'resident remains available after window Close')
        ctx.frame('closed')
    finally:
        ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: _main_windows(ctx), 'resident reopens the closed shell')
    _frame(ctx, 'after')


def _launcher_identity(ctx):
    text = ctx.cli('--version')
    if text['returncode'] == 0 and text['stdout'].startswith('putgd-cli '):
        ctx.blocked('Context.cli routed the launcher flag to putgd-cli; launcher dispatch remains a core capability gap.', issue=675)
    return text


@recipe('cli.version', needs=('launcher',))
def cli_version(ctx):
    ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: _result(ctx.call('ui.viewState'))['state'] == 'ready', 'History helper ready')
    _frame(ctx, 'before')
    text = _launcher_identity(ctx)
    ctx.expect(text['returncode'] == 0 and text['stdout'].startswith('putgd '), 'launcher prints version')
    version = _cli(ctx, '--version-json')
    authoritative = _result(ctx.call('ui.snapshot'))['build']
    ctx.expect(version == authoritative, 'version JSON matches exact resident build')
    _frame(ctx, 'after')


@recipe('cli.launcher-history', 'cli.second-launch', needs=('history-row', 'launcher'))
def launcher_history(ctx):
    _launcher_identity(ctx)
    _row(ctx)
    _frame(ctx, 'before')
    # cli is the launcher for leading flags; method names address putgd-cli.
    for flag in ('--history', '--activity'):
        reply = ctx.cli(flag)
        ctx.expect(reply['returncode'] == 0, 'second launcher forwards ' + flag)
        ctx.wait(lambda: _result(ctx.call('ui.viewState'))['state'] == 'ready', 'existing resident shows History')
        ctx.expect(_state(ctx)['total'] >= 1, 'forwarded launch retains history')
    _frame(ctx, 'after')
    ctx.review('ui.viewState reports helper readiness, not active page; verify second launcher raised History in the saved frame.')


@recipe('mcp.inspect')
def mcp_inspect(ctx):
    ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: _result(ctx.call('ui.viewState'))['state'] == 'ready', 'History helper ready')
    _frame(ctx, 'before')
    inspection = _result(ctx.mcp('inspect'))
    ctx.expect(set(inspection) == {'capabilities', 'diagnostics', 'configuration'}, 'MCP inspect returns three read-only inspections')
    view = _result(ctx.mcp('get_view_state'))
    ctx.expect(view == _result(ctx.call('ui.viewState')), 'MCP view-state matches resident')
    _frame(ctx, 'after')


# Gallery observation runs last so a failed native probe cannot hide unrelated window/menu results.
@recipe('history.view-gallery', 'history.view-list', needs=('history-row',))
def view_modes(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    _gallery(ctx)
    _frame(ctx, 'gallery', GALLERY)
    _details(ctx)
    ctx.expect(row['id'] in _state(ctx)['selectedIds'], 'gallery keeps exact row identity')
    _frame(ctx, 'gallery-details', '55-history-gallery-details')
    ctx.keys('Esc')
    ctx.wait(lambda: not _state(ctx)['detailsOpen'], 'Escape returns to gallery')
    ctx.click('historyListMode', window='history')
    ctx.wait(lambda: not _state(ctx)['gridMode'], 'List button restores list')
    _frame(ctx, 'after')



@recipe('history.key-enter', needs=('history-row',))
def enter_details(ctx):
    row = _row(ctx)
    _gallery(ctx)
    _frame(ctx, 'before', GALLERY)
    _focus_items(ctx)
    ctx.keys('Home', 'Return')
    ctx.wait(lambda: _state(ctx)['detailsOpen'], 'Return opens single selected row details')
    ctx.expect(row['id'] in _state(ctx)['selectedIds'], 'details belong to selected history ID')
    _frame(ctx, 'after', '55-history-gallery-details')
    ctx.keys('Esc')



@recipe('history.key-esc', needs=('history-row',), timeout=180)
def escape_details(ctx):
    preview(ctx)
    _details(ctx)
    ctx.keys('Esc')
    ctx.wait(lambda: not _state(ctx)['detailsOpen'], 'Escape without expanded preview closes details')
    _frame(ctx, 'after', GALLERY)



# Explicit declarations, rather than optimistic click-and-review passes. These
# remain functional=blocked until the named setup/oracle exists. Each reason is
# independently included in the per-ID acceptance table generated from REGISTRY.
BLOCKED = {
    'history.list': 'Double-click is available, but row/tile hit targets still need verified runtime delegate geometry (#675/#712); clicking the list center cannot identify the selected row.',
    'history.filters': 'Needs full date/MIME/destination/state fixture matrix and deterministic result oracles for every + Filter choice; clear-filters recipe exercises one upload filter only.',
    'history.row-primary': 'Needs historyRowPrimary controls captures for selected/hovered Retry upload/local save/deletion and Copy result variants; fixture variants do not yet cover all states.',
    'history.row-menu': 'Fresh upload/local-save/cancel rows now exist, but queued/failed deletion variants still need deterministic failed/queued deletion fixtures (#675); link opening stays with links.',
    'history.edit-again': 'Retained history images now exist; GUI Edit still needs a returned editor identity and an immutable-source oracle to prove the new editing session (#675).',
    'history.retry-deletion': 'Needs tray_progress-style moto failed-delete fixture with stable deletion ID plus queued Cancel deletion variant.',
    'history.save-diagnostics': 'Failed-row fixtures now exist; diagnostics still needs its private output-directory/archive manifest oracle (#675).',
    'history.local-copy-settings': 'Needs missing-local-copy guidance fixture; ui.viewState does not report the selected Settings tab.',
    'history.empty-states': 'Private empty history is available, but the composite empty/load-error/retry surface still needs deterministic production load-failure setup (#675).',
    'history.compact': 'Live geometry is available; compact Back to list/gallery transitions still need a verified resize and current controls with a complete compact-layout oracle (#675).',
    'shell.app-menu-quit': 'Needs isolated resident lifecycle fixture/restart boundary; quitting a shared run would invalidate later recipes.',
    'shell.nav': 'Needs mainNavigation tab hit rectangles and active main.page oracle; ui.viewState currently exposes state/appearance/captureRouting only.',
    'shell.button-layout': 'Needs isolated left/right GNOME button-layout fixture and current-build control rectangles; never mutate a shared desktop setting.',
    'shell.command-capture-region': 'Needs capture selector fixture, selection bounds and artifact oracle; must complete/cancel real selector before next recipe.',
    'shell.command-capture-window': 'Needs independent synthetic target window, real window selector completion and operation identity.',
    'shell.command-capture-desktop': 'Needs local-output capture fixture and captured artifact/hash oracle.',
    'shell.command-capture-edit': 'Needs region selector completion fixture and inspectable editor ID.',
    'shell.command-record': 'Needs recording selector fixture and cleanup through recording.cancel with exact recording ID.',
    'shell.command-upload-clipboard': 'Needs independent native clipboard fixture with payload/hash oracle, reused from tray_progress.py.',
    'shell.edit-source-menu': 'Needs clipboard image and native edit-file chooser fixtures plus current menu inventory.',
    'shell.drop-image': 'Context has pointer drag but no file-manager drag source fixture; shelling a synthetic drop event is not real input.',
    'shell.command-tooltips': 'Hover is available, but the complete command tooltip/output/unavailability matrix needs current native capture and disabled-output fixtures plus a live tooltip ownership witness (#675).',
    'shell.narrow-labels': 'Needs command-band accessible label/style projection at multiple widths; ui.viewState has no shell label oracle.',
    'shell.output-summary': 'Needs output-summary popup capture and Settings Capture navigation oracle.',
    'shell.output-warning': 'settings-no-output now supplies configuration; the full warning-text matrix and Settings Capture navigation still need live assertions (#675).',
    'shell.action-recovery-banner': 'Needs rejected-action fixture for Change output/Keep local copies/Review destination variants.',
    'shell.autosave-status': 'Needs accepted/refused autosave fixtures and saved-status accessible projection.',
    'shell.edit-chooser-failure': 'chooser-failure explicitly blocks: production portal errors fall back; deterministic failure/retry/dismiss needs an isolated production failure boundary (#671).',
    'shell.first-launch': 'app-profile(first_launch=True) now provides lifecycle reset; the full initial Settings/no-output contract still needs live selected-page and warning-strip assertions (#675).',
    'cli.launcher-background': 'The launcher descriptor exists, but Context.cli still routes leading flags to putgd-cli; stopped-resident/tray-absent lifecycle and the 10-second fallback oracle are also missing (#675).',
    'cli.action-invoke': 'Needs native source and six action completion fixtures; capture/record selectors cannot be left active.',
    'cli.history': 'History/operation rows now exist; the full history.*, operation.*, delete.*, share.inspect composite still needs queued/failed deletion and sharing fixtures (#675).',
    'mcp.perform-action': 'Needs native source and six action completion fixtures for perform_action.',
    'mcp.operations': 'Failed/local-save/active rows now exist; the complete operation/delete retry, cancel and clipboard-result matrix still needs deletion variants and a MIME oracle (#675).',
    'mcp.history': 'History/delete rows now exist; complete preview/diagnostics output oracles remain missing, and this composite also includes link tools owned by the links lane (#675).',
}


def _declare_blocked(interaction, reason):
    def declaration(ctx):
        ctx.blocked(reason, issue=675)
    declaration.__name__ = 'blocked_' + interaction.replace('.', '_').replace('-', '_')
    recipe(interaction)(declaration)


for _interaction, _reason in BLOCKED.items():
    _declare_blocked(_interaction, _reason)
