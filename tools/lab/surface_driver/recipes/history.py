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
import html
from decimal import Decimal, ROUND_HALF_UP

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, recipe

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
    if window:
        _wait_window(ctx, window)
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
            ctx.blocked('History list has ambiguous native keyboard-focus witnesses.', issue=820)
        ctx.keys('Tab')
    ctx.blocked('History list did not acquire verified native keyboard focus within 20 Tab steps.', issue=820)


def _wait_window(ctx, window):
    """A click can return before an asynchronous dialog or popup settles."""
    def observed():
        try:
            return ctx.geometry(window)
        except RecipeBlocked as error:
            if 'live window match is missing or ambiguous' not in error.reason:
                raise
            return None
    try:
        return ctx.wait(observed, 'live ' + window + ' window settles')
    except RecipeFailed:
        ctx.blocked('No unambiguous live geometry after waiting for ' + window + '.', issue=820)


def _window_node(ctx, window, query):
    """Click one app child through its bounds and its own window's guard."""
    box = _wait_window(ctx, window)
    query = dict(query)
    query.setdefault('within', [box[k] for k in ('x', 'y', 'width', 'height')])
    def observed():
        nodes = ctx.accessible(dict(query, owned=True, visible=True, showing=True, enabled=True, pid=box['pid']))
        return nodes[0] if len(nodes) == 1 and nodes[0].get('rect') else None
    try:
        node = ctx.wait(observed, 'one enabled History child acquires live bounds')
    except RecipeFailed:
        ctx.blocked('History input requires one owned, enabled native child with live bounds after waiting.', issue=820)
    x, y, width, height = node['rect']
    point = (x + width / 2, y + height / 2)
    if (min(width, height) <= 0 or node.get('coordinate_space') == 'window-relative'
            or not box['x'] <= point[0] < box['x'] + box['width']
            or not box['y'] <= point[1] < box['y'] + box['height']):
        ctx.blocked('History child has no verified desktop point inside its live window.', issue=820)
    # accessible() returns desktop coordinates; click_at() takes head pixels.
    scale = 1 if ctx.profile.get('session') == 'x11' else box.get('scale', 1)
    point = tuple(round((value - box.get(origin, 0)) * scale)
                  for value, origin in zip(point, ('screen_x', 'screen_y')))
    return box, node, point


def _click_window_node(ctx, window, query):
    box, node, point = _window_node(ctx, window, query)
    ctx.focus(window, raise_window=False)
    ctx.click_at(*point, head=box.get('head', 0))
    return node


def _hover_history_node(ctx, query):
    box, node, point = _window_node(ctx, 'history', query)
    ctx.focus('history', raise_window=False)
    ctx.hover(*point, head=box.get('head', 0))
    return node


def _searched_row_node(ctx):
    box = ctx.geometry('history')
    lists = ctx.accessible({'owned': True, 'pid': box['pid'], 'visible': True, 'showing': True,
                            'name': 'History items and upload activity'})
    if len(lists) != 1 or not lists[0].get('rect'):
        ctx.blocked('Double-click requires one live History list before resolving its unique row.', issue=820)
    rows = ctx.accessible({'owned': True, 'pid': box['pid'], 'visible': True, 'showing': True,
                           'role': 'list item', 'within': lists[0]['rect']})
    if len(rows) != 1 or not rows[0].get('name'):
        ctx.blocked('Double-click requires a uniquely searched native row with its exact accessible name.', issue=820)
    return rows[0]


def _double_history_row(ctx):
    node = _searched_row_node(ctx)
    previous = ctx.profile.get('live_controls', {})
    ctx.profile['live_controls'] = {**previous, 'historySearchedRow': {'name': node['name'], 'role': 'list item'}}
    try:
        ctx.double_click('historySearchedRow', window='history')
    finally:
        ctx.profile['live_controls'] = previous


def _click_history_node(ctx, query):
    return _click_window_node(ctx, 'history', query)


def _confirm_button(ctx, window, name):
    return _click_window_node(ctx, window, {'name': name, 'role': 'push button'})


def _row(ctx, fixture='history-row'):
    data = ctx.fixture(fixture)
    row = data.row if fixture == 'history-row' else data['image'] if fixture == 'history_rows' else data
    _open(ctx)
    detail = _result(ctx.call('history.detail', {'id': row['id']}))
    ctx.expect(detail['id'] == row['id'] and detail['name'] == row['name'], 'fixture has the expected identity')
    _search(ctx, row['name'])
    ctx.wait(lambda: _state(ctx)['rows'] == 1, 'unique fixture row is shown')
    pid = ctx.geometry('history')['pid']
    lists = ctx.accessible({'owned': True, 'visible': True, 'showing': True, 'pid': pid,
                            'name': 'History items and upload activity'})
    if len(lists) != 1 or not lists[0].get('rect'):
        ctx.blocked('The uniquely searched History list has no unambiguous live bounds.', issue=820)
    # The query has exactly one row. Target its actual delegate inside the live
    # list, rather than blank viewport space or an unselected Home focus item.
    _click_history_node(ctx, {'role': 'list item', 'within': lists[0]['rect']})
    ctx.wait(lambda: row['id'] in _state(ctx)['selectedIds'], 'fixture row is selected with real input')
    _focus_items(ctx)
    return row


def _gallery(ctx):
    ctx.click('historyGridMode', window='history')
    ctx.wait(lambda: _state(ctx)['gridMode'], 'gallery mode')


def _details(ctx):
    # In split-list mode details are already beside the row; gallery provides
    # an observable open/close transition at both compact and full widths.
    _gallery(ctx)
    _focus_items(ctx)
    ctx.keys('Home', 'Ctrl+A')
    ctx.wait(lambda: _state(ctx)['selectedRows'] == 1, 'keyboard selects the unique gallery row')
    ctx.keys('Return')
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
    # The real Add Filter action focuses its newly revealed control. Bind the
    # owning window, then prove that focus by the resulting query change.
    ctx.focus('history', raise_window=False)
    _frame(ctx, 'filter-revealed', None)
    ctx.keys('Home', 'Down', 'Return')
    ctx.wait(lambda: bool(_state(ctx)['query']['uploadStatus']) and not _state(ctx)['busy'], 'upload filter applied')
    # Return opens this noneditable combo on some Qt styles. Dismiss its grab
    # before clicking the underlying Clear filters button.
    ctx.keys('Esc')
    ctx.expect(bool(_state(ctx)['query']['uploadStatus']), 'dismissing the combo retains the applied filter')
    _frame(ctx, 'filtered', '40-history-filters-active')
    clear = _click_history_node(ctx, {'name': 'Clear filters', 'role': 'push button'})
    ctx.note('Clear filters clicked through owned native bounds: ' + json.dumps(clear['rect']))
    _frame(ctx, 'clear-click', None)
    ctx.wait(lambda: not _state(ctx)['activeFilters'], 'advanced filters cleared')
    ctx.expect(_state(ctx)['query']['search'] == row['name'], 'Clear filters preserves search')
    _frame(ctx, 'after')






def _click_preview(ctx, button=False):
    # Both are exposed as buttons. The large image accepts keyboard focus;
    # its small expand glyph deliberately has Qt::NoFocus.
    _click_history_node(ctx, {'name': 'Expand preview', 'states': {'focusable': not button}})


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
    ctx.click('text:More selection actions', window='history')
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
    for key in ('Shift+F10', 'Menu'):
        # A real delegate click establishes both selection and currentIndex.
        # Home on an already-current row need not restore a cleared selection.
        box = ctx.geometry('history')
        lists = ctx.accessible({'owned': True, 'pid': box['pid'], 'visible': True, 'showing': True,
                                'name': 'History items and upload activity'})
        ctx.expect(len(lists) == 1 and bool(lists[0].get('rect')), 'one live History list before keyboard row binding')
        query = {'role': 'list item', 'within': lists[0]['rect']}
        _click_history_node(ctx, query)
        _focus_items(ctx)
        ctx.wait(lambda: _state(ctx)['selectedIds'] == [row['id']] and len(ctx.accessible(dict(
            query, owned=True, pid=box['pid'], visible=True, showing=True, states={'focused': True}))) == 1,
            'real delegate click establishes the exact selected current keyboard row')
        _frame(ctx, 'selected-' + key)
        ctx.keys(key)
        try:
            _wait_menu(ctx, 'Copy details')
        except RecipeFailed:
            if key == 'Menu':
                ctx.blocked('Shift+F10 opened the real row menu and copied the exact receipt; the portable Menu key produced no owned popup after verified current-row focus. A native Menu-key delivery witness is still needed for this second binding.', issue=835)
            raise
        _frame(ctx, 'menu-' + key, None)
        _copied_row_details(ctx, row)
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
    # The resident reports the chooser request before its native window maps.
    _wait_window(ctx, 'historySaveDialog')
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
    _wait_window(ctx, 'historyCancelAllConfirmation')
    _frame(ctx, 'keep-question', 'history-cancel-all-confirmation', 'historyCancelAllConfirmation')
    ctx.expect(state['open'] and set(state['operationIds']) == set(ids), 'confirmation captures exact active identities')
    _confirm_button(ctx, 'historyCancelAllConfirmation', 'Keep uploading')
    ctx.wait(lambda: not _state(ctx)['cancelAllConfirmation']['open'], 'Keep uploading dismisses confirmation')
    ctx.expect(_state(ctx)['activeOperationCount'] == len(ids), 'Keep uploading leaves all operations active')
    ctx.click('text-glob:Cancel all (*)…', window='history')
    second = ctx.wait(lambda: (s if (s := _state(ctx))['cancelAllConfirmation']['open'] else None), 'second cancel-all confirmation opens')
    ctx.expect(set(second['cancelAllConfirmation']['operationIds']) == set(ids), 'second confirmation captures same identities')
    _confirm_button(ctx, 'historyCancelAllConfirmation', 'Cancel ' + str(len(ids)) + ' uploads')
    ctx.wait(lambda: all(_result(ctx.call('operation.inspect', {'id': item}))['state'] == 'cancelled' for item in ids), 'all confirmed uploads cancel', timeout=75)
    ctx.expect(_state(ctx)['activeOperationCount'] == 0, 'banner clears canceled uploads')
    _frame(ctx, 'after')


@recipe('history.delete-remote', 'confirm.history-delete', needs=('history_delete',), destinations=('local', 's3'))
def delete_remote(ctx):
    row = _row(ctx, 'history_delete')
    original = _result(ctx.call('history.detail', {'id': row['id']}))
    _frame(ctx, 'before')
    ctx.click('text:Delete from destination…', window='history')
    _wait_window(ctx, 'historyConfirmAction')
    ctx.frame('question', window='historyConfirmAction')
    ctx.click('text:Show the full deletion preparation', window='historyConfirmAction')
    _wait_window(ctx, 'Deletion technical details')
    ctx.click('text:Copy technical details', window='Deletion technical details')
    preparation = json.loads(ctx.clipboard_text())
    ctx.frame('technical-details', window='Deletion technical details')
    ctx.expect([target['historyId'] for target in preparation['targets']] == [row['id']], 'dialog prepares only exact fixture identity')
    ctx.keys('Esc')
    _confirm_button(ctx, 'historyConfirmAction', 'Cancel')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['remoteStatus'] == original['remoteStatus'], 'Cancel preserves remote state')
    ctx.click('text:Delete from destination…', window='history')
    _wait_window(ctx, 'historyConfirmAction')
    _confirm_button(ctx, 'historyConfirmAction', 'Delete * file*')
    final = ctx.wait(lambda: (d if (d := _result(ctx.call('history.detail', {'id': row['id']})))['remoteStatus'] in ('removed', 'already_missing') else None), 'recorded loopback object deleted', timeout=60)
    ctx.expect(final['id'] == original['id'] and final['local'] == original['local'], 'deletion preserves history and local copy')
    _frame(ctx, 'after', '48-history-deletion-recovery')


@recipe('history.more-menu', 'confirm.history-clear', needs=('history_rows',), timeout=240)
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
        _wait_window(ctx, 'historyConfirmAction')
        ctx.frame('question-' + ('cache' if 'temporary' in action else 'records'), window='historyConfirmAction')
        _confirm_button(ctx, 'historyConfirmAction', 'Cancel')
        ctx.expect(_state(ctx)['total'] == before, 'Cancel preserves history')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear temporary files…', window='historyManagementMenu')
    _wait_window(ctx, 'historyConfirmAction')
    _confirm_button(ctx, 'historyConfirmAction', 'Clear temporary files')
    ctx.wait(lambda: not _state(ctx)['busy'], 'temporary file cleanup finishes')
    ctx.expect(_state(ctx)['total'] == before, 'cache cleanup preserves history records')
    ctx.expect(_result(ctx.call('history.detail', {'id': ctx.fixture('history_rows')['image']['id']}))['local'] == local_before, 'cache cleanup preserves local-copy receipt')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear history records…', window='historyManagementMenu')
    _wait_window(ctx, 'historyConfirmAction')
    _confirm_button(ctx, 'historyConfirmAction', 'Clear history records')
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
            ctx.blocked('History has ambiguous live Application menu anchors.', issue=820)
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
        ctx.blocked('History has no verified blank header drag target.', issue=820)
    left, right = max(intervals, key=lambda v: v[1] - v[0])
    point = ((left + right) / 2, y)
    return tuple(round((point[i] - box.get(('screen_x', 'screen_y')[i], 0))
                       * box.get('scale', 1)) for i in (0, 1))


def _menu_names(ctx):
    return {n.get('name') for n in ctx.accessible({'owned': True, 'visible': True, 'role': 'menu item'})}


def _wait_menu(ctx, *names):
    # Runtime menus are resolved by click() through their owner. geometry()
    # deliberately addresses toplevel windows and cannot locate an untitled
    # QMenu as an independent native window.
    pid = ctx.geometry('history')['pid']
    return ctx.wait(lambda: set(names) <= {n.get('name') for n in ctx.accessible(
        {'owned': True, 'pid': pid, 'visible': True, 'showing': True, 'enabled': True, 'role': 'menu item'})},
        'owned runtime menu actions are visible: ' + ', '.join(names))


def _copied_row_details(ctx, row):
    ctx.click('text:Copy details', window='historyRowMenu')
    def copied():
        try:
            value = json.loads(ctx.clipboard_text())
        except (ValueError, TypeError):
            return None
        return value if isinstance(value, dict) and any(r.get('id') == row['id'] for r in value.get('records', [])) else None
    return ctx.wait(copied, 'keyboard row menu copies selected receipt')


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


@recipe('shell.header-drag', needs=('history-row',), timeout=180)
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
    ctx.wait(lambda: ((g := ctx.geometry('history')) and all(g[key] == box[key] for key in ('x', 'y', 'width', 'height'))), 'header drag restores original rectangle')
    ctx.blocked('Header movement and restoration are verified; the composite also requires double-click maximize/restore at this blank span, but Context has no atomic point double-click.', issue=904)


def _resize_point(ctx, box):
    # Ui::WindowEdges has 10x10 corner grips. Their center is inside the live
    # client even when the outermost corner pixel is rounded/transparent.
    scale = 1 if ctx.profile.get('session') == 'x11' else box.get('scale', 1)
    return tuple(round((box[axis] + box[extent] - 5 - box.get(origin, 0)) * scale)
                 for axis, extent, origin in (('x', 'width', 'screen_x'), ('y', 'height', 'screen_y')))


@recipe('shell.edge-resize', 'shell.minimum-size', needs=('history-row',), timeout=180)
def edge_resize(ctx):
    _row(ctx)
    box = ctx.geometry('history')
    _frame(ctx, 'before')
    edge = _resize_point(ctx, box)
    ctx.drag(edge, (edge[0] - 600, edge[1] - 500))
    resized = ctx.wait(lambda: (g if ((g := ctx.geometry('history'))['width'], g['height']) != (box['width'], box['height']) else None), 'edge drag resizes shell')
    scale = box.get('scale', 1)
    ctx.expect(resized['width'] == round(1060 * scale) and resized['height'] == round(680 * scale), 'shell respects 1060 by 680 logical minimum')
    ctx.call('ui.viewState')
    _frame(ctx, 'after', COMPACT)
    new_edge = _resize_point(ctx, resized)
    ctx.drag(new_edge, edge)


def _window_buttons_ready(ctx):
    anchor = ctx.profile.get('history_window_anchor')
    if not anchor:
        return
    box = ctx.geometry('history')
    tolerance = ctx.profile.get('history_window_anchor_tolerance', 0)
    def in_pose(geometry):
        return all(max(0, anchor[k] - tolerance) <= geometry[k] <= anchor[k] + tolerance for k in ('x', 'y'))
    if in_pose(box):
        return
    # Keep chrome outside the desktop's right-aligned notification column.
    # Move by a live blank header point; every gesture retains the normal guard.
    start = _header_point(ctx, box)
    scale = 1 if ctx.profile.get('session') == 'x11' else box.get('scale', 1)
    end = tuple(round(start[i] + (anchor[k] - box[k]) * scale) for i, k in enumerate(('x', 'y')))
    ctx.drag(start, end)
    moved = ctx.wait(lambda: (g if in_pose(g := ctx.geometry('history')) else None),
                     'main window moved clear of notification column')
    ctx.expect(all(moved[k] == box[k] for k in ('width', 'height')), 'placement preserves the exact window size')
    ctx.note('History chrome setup pose: ' + json.dumps({k: moved[k] for k in ('x', 'y', 'width', 'height')}))



@recipe('shell.window-maximize', needs=('history-row',))
def maximize(ctx):
    _row(ctx)
    _window_buttons_ready(ctx)
    original = ctx.geometry('history')
    _frame(ctx, 'before')
    _click_history_node(ctx, {'name': 'Maximize', 'role': 'push button'})
    enlarged = ctx.wait(lambda: (g if (g := ctx.geometry('history'))['width'] > original['width'] else None), 'Maximize enlarges shell')
    ctx.expect(enlarged['height'] >= original['height'], 'maximized height grows')
    ctx.call('ui.viewState')
    _frame(ctx, 'maximized')
    _click_history_node(ctx, {'name': 'Restore', 'role': 'push button'})
    ctx.wait(lambda: ctx.geometry('history') == original, 'Restore returns exact original geometry')
    _frame(ctx, 'after')


def _main_windows(ctx, include_hidden=False):
    """Inspect only the private family's exact main-window title."""
    key = 'history_hidden_window_query' if include_hidden else 'history_window_query'
    query = ctx.profile.get(key, {'title': r'^put\.gd$', 'include_hidden': include_hidden})
    observed, departed = {}, []
    def inventory():
        try:
            observed['rows'] = ctx.windows(query)
            return True  # A successfully read empty inventory is meaningful.
        except RecipeBlocked as error:
            if ('accessibility walk incomplete' not in error.reason
                    or 'Message recipient disconnected from message bus' not in error.reason):
                raise
            departed.append(error)
            return False
    try:
        ctx.wait(inventory, 'native window inventory settles after departed AT-SPI client', timeout=10)
    except RecipeFailed:
        if departed:
            raise departed[-1]
        raise
    if departed:
        ctx.note('Native window inventory was re-read successfully after a departed AT-SPI client; no absence inferred from the refused query.')
    return observed['rows']


def _minimized(window):
    """X11 inventory and native AT-SPI use different minimized-state names."""
    states = window.get('states', {})
    return bool(states.get('minimized') or states.get('iconified'))


@recipe('shell.window-minimize', needs=('history-row',))
def minimize(ctx):
    """Minimize with the chrome button and observe the compositor state."""
    row = _row(ctx)
    _window_buttons_ready(ctx)
    _frame(ctx, 'before')
    _click_history_node(ctx, {'name': 'Minimize', 'role': 'push button'})
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
    _window_buttons_ready(ctx)
    _frame(ctx, 'before')
    _click_history_node(ctx, {'name': 'Close window', 'role': 'push button'})
    try:
        ctx.wait(lambda: not _main_windows(ctx), 'closed shell has no visible main window')
        ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'],
                   'resident remains available after window Close')
        ctx.frame('closed')
    finally:
        ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: _main_windows(ctx), 'resident reopens the closed shell')
    _frame(ctx, 'after')


def _launcher(ctx, flag):
    # The public shell runs in Target's owned private XDG environment. Launcher
    # flags are literal, nonsecret commands; JSON method parameters stay on the
    # core's params-stdin CLI path. No alternate transport/backend is created.
    import shlex
    ctx.expect(flag in ('--version', '--version-json', '--history', '--activity'), 'supported launcher flag')
    executable = ctx.fixture('launcher').executable
    return ctx.shell(shlex.quote(executable) + ' ' + flag)


def _launcher_identity(ctx):
    return _launcher(ctx, '--version')


@recipe('cli.version', needs=('launcher',))
def cli_version(ctx):
    ctx.view('ui.show', {'view': 'history'})
    ctx.wait(lambda: _result(ctx.call('ui.viewState'))['state'] == 'ready', 'History helper ready')
    _frame(ctx, 'before')
    text = _launcher_identity(ctx)
    ctx.expect(text['returncode'] == 0 and text['stdout'].startswith('putgd '), 'launcher prints version')
    reply = _launcher(ctx, '--version-json')
    ctx.expect(reply['returncode'] == 0, 'launcher version JSON succeeds')
    version = json.loads(reply['stdout'])
    authoritative = _result(ctx.call('ui.snapshot'))['build']
    ctx.expect(version == authoritative, 'version JSON matches exact resident build')
    _frame(ctx, 'after')


@recipe('cli.launcher-history', 'cli.second-launch', needs=('history-row', 'launcher'), timeout=240)
def launcher_history(ctx):
    _launcher_identity(ctx)
    _row(ctx)
    _frame(ctx, 'before')
    epoch = _result(ctx.call('ui.snapshot'))['residentEpoch']
    for flag in ('--history', '--activity'):
        _window_buttons_ready(ctx)
        _click_history_node(ctx, {'name': 'Close window', 'role': 'push button'})
        ctx.wait(lambda: not _main_windows(ctx), 'shell is hidden before second launcher')
        ctx.frame('closed-' + flag)
        reply = _launcher(ctx, flag)
        ctx.expect(reply['returncode'] == 0, 'second launcher forwards ' + flag)
        ctx.wait(lambda: _result(ctx.call('ui.viewState'))['state'] == 'ready', 'existing resident shows History')
        _wait_window(ctx, 'history')
        ctx.wait(lambda: _selected_page(ctx, 'History'), 'forwarded launcher selects History')
        ctx.expect(_result(ctx.call('ui.snapshot'))['residentEpoch'] == epoch, 'second launch forwards to the same resident')
        ctx.expect(_state(ctx)['total'] >= 1, 'forwarded launch retains history')
    _frame(ctx, 'after')


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
    ctx.keys('Home', 'Ctrl+A')
    ctx.wait(lambda: _state(ctx)['selectedRows'] == 1, 'keyboard selects the unique gallery row')
    ctx.keys('Return')
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





@recipe('shell.autosave-status', needs=('settings-save-failure',), timeout=240)
def autosave_status(ctx):
    fault = ctx.fixture('settings-save-failure')
    ctx.view('ui.show', {'view': 'settings'})
    _click_history_node(ctx, {'name': 'General', 'role': 'list item'})
    ctx.wait(lambda: _result(ctx.call('ui.viewState'))['settings']['section'] == 0, 'General settings selected')
    before = _result(ctx.call('settings.get'))
    enabled = before['document']['preferences']['notifications']
    _frame(ctx, 'before', None)
    _click_history_node(ctx, {'name': 'Show desktop notifications', 'role': 'check box'})
    def refused():
        rows = _nodes(ctx, name='Settings save status')
        return rows[0] if len(rows) == 1 and rows[0].get('description') else None
    ctx.wait(refused, 'shell save status carries the refused-save explanation')
    ctx.expect(_result(ctx.call('settings.get'))['document'] == before['document'],
               'refused write preserves the accepted configuration')
    ctx.frame('changes-not-saved')
    fault.restore()
    _click_history_node(ctx, {'name': 'Retry saving', 'role': 'push button'})
    ctx.wait(lambda: _result(ctx.call('settings.get'))['document']['preferences']['notifications'] != enabled,
             'real Retry saving commits the pending edit')
    ctx.wait(lambda: all(not n.get('description') for n in _nodes(ctx, name='Settings save status')),
             'accepted save clears the shell failure explanation')
    _frame(ctx, 'after', None)
    ctx.review('Inspect the status-band captions in saved frames: Settings save automatically and Changes not saved.')


COMMAND_LABELS = {'region': 'Capture region', 'window': 'Capture window', 'desktop': 'Capture desktop',
                  'edit': 'Capture region & edit', 'record': 'Record screen', 'clipboard': 'Upload clipboard'}


def _command_control(action):
    # Qt exports the split Edit source control as a menu on some versions.
    # Its unique native name, owned PID and live bounds still bind the input.
    return {'name': 'Capture region*edit' if action == 'edit' else COMMAND_LABELS[action],
            **({'role': 'push button'} if action != 'edit' else {})}


@recipe('shell.command-tooltips', needs=('app-profile',), timeout=240)
def command_tooltips(ctx):
    labels = COMMAND_LABELS
    for unavailable in (False, True):
        if unavailable:
            ctx.fixture('settings-no-output')
        _open(ctx)
        _window_buttons_ready(ctx)
        if not unavailable:
            _frame(ctx, 'before')
        config = _result(ctx.call('settings.get'))['document']
        for action, label in labels.items():
            node = _hover_history_node(ctx, _command_control(action))
            def tooltip():
                rows = _nodes(ctx, role='tool tip')
                texts = [html.unescape(n.get('text') or n.get('name', '')) for n in rows]
                return next((text for text in texts if label in text or node.get('name', label) in text), None)
            text = ctx.wait(tooltip, 'owned tooltip for ' + action)
            ctx.expect('Output' in text or 'local' in text.lower() or 'Nothing is saved or uploaded' in text,
                       'command tooltip includes actual output behavior')
            shortcut = config['actions'][action].get('shortcut', '')
            if shortcut:
                ctx.expect(shortcut in text, 'tooltip includes configured shortcut')
            if unavailable:
                ctx.expect('New captures have no output' in text, 'tooltip includes unavailability reason')
            _frame(ctx, 'tooltip-' + action + ('-unavailable' if unavailable else '-local'), None)
            ctx.keys('Esc')
    _frame(ctx, 'after')


@recipe('history.save-diagnostics', needs=('history_retry_upload', 'app-profile'))
def save_diagnostics(ctx):
    row = _row(ctx, 'history_retry_upload')
    root = ctx.fixture('app-profile').output_dir
    _frame(ctx, 'before')
    _click_history_node(ctx, {'name': 'Save diagnostics for this upload', 'role': 'push button'})
    import shlex
    code = """import hashlib,json,zipfile
from pathlib import Path
root=Path(ROOT)/'diagnostics'
files=list(root.glob('putgd-diagnostics-*.zip')) if root.exists() else []
assert len(files)<=1
if files:
 p=files[0];assert p.is_file() and not p.is_symlink()
 with zipfile.ZipFile(p) as z:
  manifest=json.loads(z.read('manifest.json'))
  assert manifest['scope']['operationId']==OPERATION and manifest['operations']==1
  for entry in manifest['files']:
   data=z.read(entry['path'])
   assert len(data)==entry['bytes'] and hashlib.sha256(data).hexdigest()==entry['sha256']
 print(json.dumps({'valid':True,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'files':len(manifest['files'])}))
else:print('{}')
""".replace('ROOT', repr(root)).replace('OPERATION', repr(row['operation_id']))
    def saved():
        reply = ctx.shell('python3 -c ' + shlex.quote(code))
        ctx.expect(reply['returncode'] == 0, 'diagnostics ZIP manifest, scope and every entry hash validate')
        return json.loads(reply['stdout']).get('valid')
    ctx.wait(saved, 'row diagnostics written to private archive', timeout=30)
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'],
               'saving diagnostics preserves the failed receipt')
    _frame(ctx, 'after')


@recipe('history.local-copy-settings', needs=('history_retry_local',))
def local_copy_settings(ctx):
    row = _row(ctx, 'history_retry_local')
    detail = _result(ctx.call('history.detail', {'id': row['id']}))
    ctx.expect(detail['local']['status'] != 'saved', 'fixture has no successfully retained local copy')
    _frame(ctx, 'before')
    _click_history_node(ctx, {'name': 'Local-copy settings', 'role': 'push button'})
    _capture_settings(ctx)
    _frame(ctx, 'after', None)


@recipe('history.compact', needs=('history-row',), timeout=180)
def compact_details(ctx):
    row = _row(ctx)
    box = ctx.geometry('history')
    _frame(ctx, 'before')
    edge = _resize_point(ctx, box)
    ctx.drag(edge, (edge[0] - 600, edge[1] - 500))
    ctx.wait(lambda: _state(ctx)['layout'] == 'compact', 'real resize enters compact History layout')
    try:
        for grid, back in ((False, 'Back to list'), (True, 'Back to gallery')):
            ctx.click('historyGridMode' if grid else 'historyListMode', window='history')
            _focus_items(ctx)
            ctx.keys('Home', 'Return')
            ctx.wait(lambda: _state(ctx)['detailsOpen'] and _state(ctx)['inspectorVisible'], 'compact details replace the list')
            ctx.expect(_state(ctx)['selectedIds'] == [row['id']], 'compact details retain exact history identity')
            ctx.expect(not _nodes(ctx, name='History items and upload activity'), 'details hide the compact list')
            _frame(ctx, 'compact-' + str(grid), None)
            ctx.click('text:' + back, window='history')
            ctx.wait(lambda: not _state(ctx)['detailsOpen'], back + ' restores browsing')
            ctx.expect(bool(_nodes(ctx, name='History items and upload activity')), 'Back restores the native list')
    finally:
        now = ctx.geometry('history')
        ctx.drag(_resize_point(ctx, now), edge)
    ctx.click('historyListMode', window='history')
    _frame(ctx, 'after')

# Composite rows use production inspection plus real input. Fixture setup and
# cleanup stay in core; no fake resident replies or injected Qt events.
def _nodes(ctx, **query):
    return ctx.accessible(dict(owned=True, visible=True, showing=True,
                               pid=ctx.geometry('history')['pid'], **query))


def _selected_page(ctx, label):
    return any(n.get('states', {}).get('selected') for n in _nodes(ctx, role='page tab', name=label))


def _capture_settings(ctx):
    ctx.wait(lambda: _selected_page(ctx, 'Settings'), 'Settings navigation tab selected')
    ctx.wait(lambda: _result(ctx.call('ui.viewState')).get('settings', {}).get('section') == 1,
             'resident reports Settings Capture section')


@recipe('shell.nav', needs=('app-profile',))
def navigation(ctx):
    _open(ctx)
    _frame(ctx, 'before')
    for page in ('Destinations', 'Settings', 'History'):
        _click_history_node(ctx, {'role': 'page tab', 'name': page})
        ctx.wait(lambda: _selected_page(ctx, page), page + ' selected after pointer input')
    # The segmented tab bar implements native arrow-key navigation.
    for key, page in (('Right', 'Destinations'), ('Right', 'Settings'), ('Left', 'Destinations'), ('Left', 'History')):
        ctx.keys(key)
        ctx.wait(lambda: _selected_page(ctx, page), page + ' selected after ' + key)
    ctx.call('ui.viewState')
    _frame(ctx, 'after')


@recipe('shell.app-menu-quit', needs=('resident-lifecycle',), timeout=240)
def quit_application(ctx):
    lifecycle = ctx.fixture('resident-lifecycle')
    _open(ctx)
    _frame(ctx, 'before')
    ctx.click('applicationMenu', window='history')
    _wait_menu(ctx, 'Quit put.gd')
    ctx.click('text:Quit put.gd', window='applicationMenuPopup')
    ctx.wait(lifecycle.stopped, 'private resident socket removed after real Quit')
    ctx.frame('quit')
    new = lifecycle.launch()
    ctx.expect(new != lifecycle.initial, 'relaunch has a new authenticated resident identity')
    _open(ctx)
    _frame(ctx, 'after')


@recipe('shell.output-summary', needs=('settings-action-routes',), timeout=240)
def output_summary(ctx):
    routes = ctx.fixture('settings-action-routes')
    routes.add('clipboard', routes.profile_ids[1])
    _open(ctx)
    _frame(ctx, 'before')
    ctx.click('captureOutputSummary', window='history')
    _wait_menu(ctx, 'Change output')
    rows = ctx.accessible({'owned': True, 'visible': True, 'showing': True,
                           'pid': ctx.geometry('history')['pid'], 'role': 'label'})
    labels = [n['name'] for n in rows]
    ctx.expect(any(n.startswith('Default: ') for n in labels), 'popup exposes inherited output')
    ctx.expect(any(n.startswith('Upload clipboard: ') and 'Synthetic 1' in n for n in labels),
               'popup exposes the exact clipboard exception destination')
    ctx.frame('output-popup')
    ctx.click('text:Change output', window='captureOutputMenu')
    _capture_settings(ctx)
    _frame(ctx, 'after', None)


@recipe('shell.output-warning', needs=('settings-no-output',))
def output_warning(ctx):
    ctx.fixture('settings-no-output')
    _open(ctx)
    _frame(ctx, 'before')
    ctx.expect(bool(_nodes(ctx, name='New captures have no output. Choose a destination or turn on Keep a local copy.')),
               'accepted no-output configuration produces the actual warning strip')
    _click_history_node(ctx, {'name': 'Change output', 'role': 'push button'})
    _capture_settings(ctx)
    ctx.expect(not _nodes(ctx, objectName='chooseCommandOutput'), 'Capture page owns recovery after navigation')
    _frame(ctx, 'after', None)


@recipe('shell.first-launch')
def first_launch(ctx):
    profile = ctx.fixture('app-profile', first_launch=True)
    ctx.expect(profile.first_launch, 'core restarted without a configuration file')
    ctx.wait(lambda: _selected_page(ctx, 'Settings'), 'first launch selects Settings without a show command')
    ctx.call('ui.viewState')
    _frame(ctx, 'before', None)
    ctx.expect(bool(_nodes(ctx, name='New captures have no output. Choose a destination or turn on Keep a local copy.')),
               'first launch explains that no output is configured')
    _click_history_node(ctx, {'name': 'Change output', 'role': 'push button'})
    _capture_settings(ctx)
    _frame(ctx, 'after', None)


@recipe('history.list', needs=('history_rows',), timeout=240)
def list_gestures(ctx):
    data = ctx.fixture('history_rows')
    row = _row(ctx, 'history_rows')
    _frame(ctx, 'before')
    # Compact details have an explicit inspectable open/closed state; list at
    # full width already shows its inspector. Gallery uses the same delegates.
    _gallery(ctx)
    _details(ctx)
    ctx.keys('Esc')
    ctx.wait(lambda: not _state(ctx)['detailsOpen'], 'Escape closes Enter details')
    _double_history_row(ctx)
    ctx.wait(lambda: _state(ctx)['detailsOpen'], 'double-click opens exact selected delegate')
    ctx.expect(_state(ctx)['selectedIds'] == [row['id']], 'double-click preserves receipt identity')
    ctx.keys('Esc')
    ctx.click('historyListMode', window='history')
    _search(ctx, '')
    _focus_items(ctx)
    ctx.keys('Home', 'Ctrl+A')
    ctx.wait(lambda: set(_state(ctx)['selectedIds']) == set(data.ids), 'extended selection contains every independent fixture receipt')
    # Context menu at a native row point, with the native menu's Copy details
    # proving the clicked row rather than optimistic menu visibility alone.
    _search(ctx, row['name'])
    _focus_items(ctx)
    ctx.keys('Home')
    previous = ctx.profile.get('live_controls', {})
    node = _searched_row_node(ctx)
    ctx.profile['live_controls'] = {**previous, 'historySearchedContextRow': {'name': node['name'], 'role': 'list item'}}
    try:
        ctx.click('historySearchedContextRow', window='history', button='right')
    finally:
        ctx.profile['live_controls'] = previous
    _wait_menu(ctx, 'Copy details')
    ctx.frame('context-menu')
    copied = _copied_row_details(ctx, row)
    ctx.expect([r['id'] for r in copied['records']] == [row['id']], 'right-click menu acts on exact row')
    _frame(ctx, 'after')


def _recent(ctx):
    return _result(ctx.call('operation.recent'))['operations']


def _new_operation(ctx, previous, action):
    def observed():
        matches = [o for o in _recent(ctx) if o['id'] not in previous and o['action'] == action]
        ctx.expect(len(matches) <= 1, 'one operation admitted for one command')
        return matches[0] if matches else None
    value = ctx.wait(observed, 'new ' + action + ' operation')
    ctx.note('operationId=' + value['id'] + ' action=' + action)
    return value


def _settle(ctx, identity):
    return ctx.wait(lambda: (v if (v := _result(ctx.call('operation.inspect', {'id': identity})))['state']
                            in ('completed', 'cancelled', 'failed', 'partial', 'interrupted') else None),
                    'owned operation settles', timeout=45)


def _exercise_command(ctx, action, dispatch, *, external_focus=True):
    if action == 'clipboard':
        source = ctx.fixture('events.clipboard')
        ctx.wait(lambda: ctx.clipboard_text() == source.text, 'synthetic text clipboard acquired')
    elif action == 'window' and external_focus:
        scene = ctx.fixture('selector-desktop')
        scene.command('raise')
        ctx.focus('historyFixture', raise_window=False)
    previous = {v['id'] for v in _recent(ctx)}
    dispatch(action)
    operation = _new_operation(ctx, previous, action)
    identity = operation['id']
    try:
        if action in ('region', 'edit', 'record'):
            window = 'recsel' if action == 'record' else 'region'
            _wait_window(ctx, window)
            ctx.expect(operation['state'] not in ('failed', 'interrupted'), 'command reached its real selector')
            ctx.frame('selector-' + action)
            ctx.focus(window, raise_window=False)
            ctx.keys('Esc')
            ctx.expect(_settle(ctx, identity)['state'] == 'cancelled', 'real Escape cancels the new selector')
        else:
            state = _settle(ctx, identity)
            ctx.expect(state['state'] == 'completed' and bool(state.get('items')), 'local command completed')
            ctx.expect(all(i['uploadStatus'] == 'local_only' and i['local']['status'] == 'saved' for i in state['items']),
                       'every artifact saved locally without publication')
            if action == 'clipboard':
                # Read only these operation-owned local paths. No existing
                # clipboard payload or arbitrary user file is inspected.
                import shlex
                path = state['items'][0]['local']['path']
                script = 'python3 -c ' + shlex.quote('from pathlib import Path;assert Path(' + repr(path) + ').read_text() == ' + repr(source.text))
                ctx.expect(ctx.shell(script)['returncode'] == 0, 'saved clipboard bytes match the synthetic source')
        ctx.frame('completed-' + action)
    finally:
        ctx.call('operation.cancel', {'id': identity})


def _shell_command_recipe(interaction, action):
    def command(ctx):
        ctx.fixture('app-profile')
        if action != 'clipboard':
            ctx.fixture('selector-desktop').command('raise')
        _open(ctx)
        _window_buttons_ready(ctx)
        _frame(ctx, 'before')
        _exercise_command(ctx, action, lambda _: _click_history_node(ctx, _command_control(action)), external_focus=False)
        _open(ctx)
        _frame(ctx, 'after')
    command.__name__ = 'command_' + action
    recipe(interaction, needs=('app-profile', 'events.clipboard') if action == 'clipboard'
           else ('app-profile', 'selector-desktop'), timeout=240)(command)
    return command


for _action in ('region', 'window', 'desktop', 'edit', 'record', 'clipboard'):
    _shell_command_recipe('shell.command-' + {'edit': 'capture-edit', 'record': 'record', 'clipboard': 'upload-clipboard'}.get(_action, 'capture-' + _action), _action)


def _action_matrix(ctx, dispatch):
    ctx.fixture('app-profile')
    ctx.fixture('selector-desktop')
    _open(ctx)
    _frame(ctx, 'before')
    # Capture results replace the native clipboard owner; exercise the owned
    # text source first, before any completed screenshot changes it.
    for action in ('clipboard', 'region', 'window', 'desktop', 'edit', 'record'):
        _exercise_command(ctx, action, dispatch)
    _open(ctx)
    _frame(ctx, 'after')


@recipe('cli.action-invoke', needs=('app-profile', 'selector-desktop', 'events.clipboard'), timeout=480)
def cli_actions(ctx):
    _action_matrix(ctx, lambda action: _cli(ctx, 'action.invoke', json.dumps({'action': action})))


@recipe('mcp.perform-action', needs=('app-profile', 'selector-desktop', 'events.clipboard'), timeout=480)
def mcp_actions(ctx):
    _action_matrix(ctx, lambda action: _result(ctx.mcp('perform_action', {'action': action})))


def _local_digest(ctx, path):
    import shlex
    code = ('from pathlib import Path;import hashlib,json;print(json.dumps({"sha256":'
            'hashlib.sha256(Path(' + repr(path) + ').read_bytes()).hexdigest()}))')
    result = ctx.shell('python3 -c ' + shlex.quote(code))
    ctx.expect(result['returncode'] == 0, 'retained synthetic source readable')
    return json.loads(result['stdout'])['sha256']


@recipe('history.edit-again', needs=('history-row',), timeout=240)
def edit_again(ctx):
    row = _row(ctx)
    detail = _result(ctx.call('history.detail', {'id': row['id']}))
    path = detail['local']['path']
    before = _local_digest(ctx, path)
    _frame(ctx, 'before')
    previous = {o['id'] for o in _recent(ctx)}
    _click_history_node(ctx, {'name': 'Edit image…', 'role': 'push button'})
    operation = _new_operation(ctx, previous, 'edit')
    identity = operation['id']
    try:
        def ready():
            reply = ctx.cli('editor.inspect', json.dumps({'id': identity}))
            value = json.loads(reply['stdout'])
            return _result(value) if value.get('ok') is not False else None
        state = ctx.wait(ready, 'new editor session ready')
        ctx.expect(state['phase'] == 'editing' and [state['width'], state['height']] == [640, 320],
                   'History Edit opens an independent editor on the retained fixture')
        ctx.frame('editor')
    finally:
        ctx.call('editor.cancel', {'id': identity})
    ctx.expect(_local_digest(ctx, path) == before, 'editing and cancelling preserve immutable history source bytes')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['id'] == row['id'], 'original receipt survives Edit')
    _open(ctx)
    _frame(ctx, 'after')


# Explicit declarations, rather than optimistic click-and-review passes. These
# remain functional=blocked until the named setup/oracle exists. Each reason is
# independently included in the per-ID acceptance table generated from REGISTRY.
BLOCKED = {
    'history.filters': 'Needs full date/MIME/destination/state fixture matrix and deterministic result oracles for every + Filter choice; clear-filters recipe exercises one upload filter only.',
    'history.row-primary': 'Needs historyRowPrimary controls captures for selected/hovered Retry upload/local save/deletion and Copy result variants; fixture variants do not yet cover all states.',
    'history.row-menu': 'Fresh upload/local-save/cancel rows now exist, but queued/failed deletion variants still need deterministic failed/queued deletion fixtures; link opening stays with links.',
    'history.retry-deletion': 'Needs tray_progress-style moto failed-delete fixture with stable deletion ID plus queued Cancel deletion variant.',
    'history.empty-states': 'Private empty history is available, but the composite empty/load-error/retry surface still needs deterministic production load-failure setup.',
    'shell.button-layout': 'Needs isolated left/right GNOME button-layout fixture and current-build control rectangles; never mutate a shared desktop setting.',
    'shell.edit-source-menu': 'Needs clipboard image and native edit-file chooser fixtures plus current menu inventory.',
    'shell.drop-image': 'Context has pointer drag but no file-manager drag source fixture; shelling a synthetic drop event is not real input.',
    'shell.narrow-labels': 'Needs command-band accessible label/style projection at multiple widths; ui.viewState has no shell label oracle.',
    'shell.action-recovery-banner': 'settings-no-output covers Change output; no controlled rejected-action fixtures yet supply Keep local copies and Review destination variants for the complete banner matrix.',
    'shell.edit-chooser-failure': 'chooser-failure explicitly blocks: production portal errors fall back; deterministic failure/retry/dismiss needs an isolated production failure boundary (#671).',
    'cli.launcher-background': 'Private resident lifecycle exists; no isolated absent-tray fixture exposes the required 10-second Settings fallback after --background.',
    'cli.history': 'History/operation rows now exist; the full history.*, operation.*, delete.*, share.inspect composite still needs queued/failed deletion and sharing fixtures.',
    'mcp.operations': 'Failed/local-save/active rows now exist; the complete operation/delete retry, cancel and clipboard-result matrix still needs deletion variants and a MIME oracle.',
    'mcp.history': 'History/delete rows now exist; complete preview/diagnostics output oracles remain missing, and this composite also includes link tools owned by the links lane.',
}


def _declare_blocked(interaction, reason):
    def declaration(ctx):
        ctx.blocked(reason, issue=820)
    declaration.__name__ = 'blocked_' + interaction.replace('.', '_').replace('-', '_')
    recipe(interaction)(declaration)


for _interaction, _reason in BLOCKED.items():
    _declare_blocked(_interaction, _reason)
