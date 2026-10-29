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
* history_active_uploads: {operation_ids: [id, id]}; stalled local moto uploads.
* history_delete: {id, name}; disposable moto receipt with an unchanged local copy.
* history_saved_copy: {id, name, path}; completed save-as in the private profile.
* history_window: {header_drag_point: [screen_x, screen_y]}; a current-build
  blank header hit target and normal 1320x880 shell, with room to move/grow.

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
    target = ctx.profile.get('history_catalog_size')
    if target:
        box = ctx.geometry('history')
        if [box['width'], box['height']] != target:
            ctx.frame('setup-before-resize', window='history')
            scale = box.get('scale', 1)
            start = tuple(round(v * scale) for v in (box['x'] + box['width'] - 1, box['y'] + box['height'] - 1))
            end = tuple(round(v * scale) for v in (box['x'] + target[0] - 1, box['y'] + target[1] - 1))
            ctx.drag(start, end)
            ctx.wait(lambda: [(g := ctx.geometry('history'))['width'], g['height']] == target, 'shell fits current catalog through a real edge drag')
            ctx.frame('setup-after-resize', window='history')
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


def _row(ctx, fixture='history-row'):
    data = ctx.fixture(fixture)
    row = data.row if fixture == 'history-row' else data['image'] if fixture == 'history_rows' else data
    _open(ctx)
    detail = _result(ctx.call('history.detail', {'id': row['id']}))
    ctx.expect(detail['id'] == row['id'] and detail['name'] == row['name'], 'fixture has the expected identity')
    _search(ctx, row['name'])
    ctx.wait(lambda: _state(ctx)['rows'] == 1, 'unique fixture row is shown')
    ctx.click('historyList', window='history')
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
    ctx.click('historyList', window='history', button='right')
    ctx.keys('Esc', 'Home', 'Return')
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
    rows = ctx.wait(lambda: ctx.accessible({'name': caption + ': *'}), 'visible binary size summary')
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
    ctx.click('historyAddFilter', window='history')
    ctx.click('historyAddFilteruploadStatus', window='historyAddFilterMenu')
    ctx.click('historyChoiceuploadStatus', window='history')
    ctx.keys('Home', 'Down', 'Return')
    ctx.wait(lambda: bool(_state(ctx)['query']['uploadStatus']), 'upload filter applied')
    _frame(ctx, 'filtered', '40-history-filters-active')
    ctx.click('historyClearFilters', window='history')
    ctx.wait(lambda: not _state(ctx)['activeFilters'], 'advanced filters cleared')
    ctx.expect(_state(ctx)['query']['search'] == row['name'], 'Clear filters preserves search')
    _frame(ctx, 'after')


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
    ctx.click('historyList', window='history', button='right')
    ctx.keys('Esc', 'Home', 'Return')
    ctx.wait(lambda: _state(ctx)['detailsOpen'], 'Return opens single selected row details')
    ctx.expect(row['id'] in _state(ctx)['selectedIds'], 'details belong to selected history ID')
    _frame(ctx, 'after', '55-history-gallery-details')
    ctx.keys('Esc')


@recipe('history.preview', 'history.expand-preview', 'history.key-esc', needs=('history-row',))
def preview(ctx):
    _row(ctx)
    ctx.wait(lambda: _state(ctx)['canExpandPreview'], 'retained image preview loads')
    _frame(ctx, 'before')
    ctx.click('historyPreview', window='history')
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'preview click expands')
    _frame(ctx, 'expanded-click', '68-history-expanded-preview', 'historyExpandedPreview')
    ctx.keys('Esc')
    ctx.wait(lambda: not _state(ctx)['previewExpanded'], 'Escape closes preview')
    ctx.keys('Space')
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'Space on focused preview expands')
    ctx.click('closeHistoryPreview', window='historyExpandedPreview')
    ctx.wait(lambda: not _state(ctx)['previewExpanded'], 'Close preview closes dialog')
    ctx.click('expandHistoryPreview', window='history')
    ctx.wait(lambda: _state(ctx)['previewExpanded'], 'expand glyph opens preview')
    ctx.keys('Esc')
    _details(ctx)
    ctx.keys('Esc')
    ctx.wait(lambda: not _state(ctx)['detailsOpen'], 'Escape without expanded preview closes details')
    _frame(ctx, 'after', GALLERY)


@recipe('history.receipt-details', needs=('history-row',))
def receipt_details(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('historyReceiptExpanded', window='history')
    ctx.click('historyDetails', window='history')
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
    ctx.click('historyList', window='history')
    ctx.keys('Home', 'Shift+Down')
    state = ctx.wait(lambda: _state(ctx) if _state(ctx)['selectedRows'] == 2 else None, 'Shift selects both rows')
    ctx.expect(set(state['selectedIds']) == set(data['ids']), 'only exact two fixture IDs selected')
    ctx.expect(state['selectionSummary']['bytes'] == data['bytes'], 'selection summary adds bytes exactly')
    _size(ctx, data['bytes'], 'Total size')
    _frame(ctx, 'selected', '41a-history-selection-inspector')
    ctx.click('historySelectionMore', window='history')
    ctx.click('historyClearSelection', window='historySelectionMore')
    ctx.wait(lambda: _state(ctx)['selectedRows'] == 0, 'Clear selection menu clears both rows')
    _frame(ctx, 'after')


@recipe('history.paging', needs=('history_paging',))
def paging(ctx):
    _open(ctx)
    before = _state(ctx)
    ctx.expect(before['query']['offset'] == 0 and before['query']['limit'] == 50, 'known first page')
    ctx.expect(before['total'] == ctx.fixture('history_paging')['total'] > 50, 'fixture spans pages')
    _frame(ctx, 'before')
    ctx.click('historyNext', window='history')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['offset'] == 50, 'Next advances one page')
    _frame(ctx, 'next')
    ctx.click('historyPrevious', window='history')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['query']['offset'] == 0, 'Previous restores first page')
    _frame(ctx, 'after')


@recipe('history.key-menu', needs=('history-row',))
def key_menu(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    for key in ('Menu', 'Shift+F10'):
        ctx.keys(key)
        ctx.frame('menu-' + key, window='historyRowMenu')
        ctx.click('text:Copy details', window='historyRowMenu')
        detail = json.loads(ctx.clipboard_text())
        ctx.expect(any(item['id'] == row['id'] for item in detail['records']), 'keyboard row menu copies selected receipt')
    _frame(ctx, 'after')


@recipe('history.inspector-more', needs=('history-row',))
def inspector_more(ctx):
    row = _row(ctx)
    _frame(ctx, 'before')
    ctx.click('historyInspectorMore', window='history')
    ctx.frame('menu', window='historyInspectorMenu')
    ctx.click('saveHistoryAsAction', window='historyInspectorMenu')
    chooser = _result(ctx.call('ui.fileDialog', {'dialog': 'historySaveAs', 'action': 'inspect', 'id': row['id']}))
    ctx.expect(chooser['open'] is True, 'More file actions opens Save a copy chooser')
    # Chooser completion belongs to dialogs; Escape is real cancellation here.
    ctx.keys('Esc')
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


def _retry(ctx, fixture, control, local=False):
    row = _row(ctx, fixture)
    before = _result(ctx.call('operation.inspect', {'id': row['operation_id']}))
    _frame(ctx, 'local-before' if local else 'before', '50a-history-failure-inspector')
    ctx.click(control, window='history')
    def saved():
        detail = _result(ctx.call('history.detail', {'id': row['id']}))
        return detail if (detail['local']['status'] == 'saved' if local else detail['uploadStatus'] == 'uploaded') else None
    detail = ctx.wait(saved, 'retry settles on original history ID', timeout=60)
    after = ctx.wait(lambda: (op if (op := _result(ctx.call('operation.inspect', {'id': row['operation_id']})))['state'] == 'completed' else None),
                     'retried operation completes', timeout=30)
    ctx.expect(after['id'] == before['id'], 'retry preserves operation identity')
    ctx.expect(detail['id'] == row['id'], 'retry preserves history identity')
    _frame(ctx, 'after' if local else 'upload-after')


@recipe('history.retry-upload', needs=('history_retry_upload', 'history_retry_local'), destinations=('local', 's3'), timeout=180)
def retry_upload(ctx):
    _retry(ctx, 'history_retry_upload', 'retryOperation')
    _retry(ctx, 'history_retry_local', 'retryOperation', local=True)


@recipe('history.active-banner', needs=('history_active_uploads',), destinations=('s3',))
def active_banner(ctx):
    data = ctx.fixture('history_active_uploads')
    _open(ctx)
    ctx.expect(_state(ctx)['activeOperationCount'] == len(data['operation_ids']) >= 2, 'known stalled moto operations')
    _frame(ctx, 'before', 'history-many-active')
    ctx.click('historyMoreActive', window='history')
    ctx.wait(lambda: _state(ctx)['query']['uploadStatus'] == 'uploading', 'More active filters running uploads')
    ctx.click('cancelBannerOperation', window='history')
    ctx.wait(lambda: _state(ctx)['activeOperationCount'] < len(data['operation_ids']), 'Cancel this upload stops an active operation')
    states = [_result(ctx.call('operation.inspect', {'id': item})) for item in data['operation_ids']]
    ctx.expect(any(item['state'] == 'cancelled' for item in states), 'one exact fixture operation canceled')
    _frame(ctx, 'after', 'history-many-active')


@recipe('history.cancel-all-dialog', 'confirm.history-cancel-all', needs=('history_active_uploads',), destinations=('s3',))
def cancel_all(ctx):
    ids = ctx.fixture('history_active_uploads')['operation_ids']
    _open(ctx)
    _frame(ctx, 'before', 'history-many-active')
    ctx.click('cancelAllBannerOperations', window='history')
    state = ctx.wait(lambda: (s if (s := _state(ctx))['cancelAllConfirmation']['open'] else None), 'cancel-all confirmation opens')['cancelAllConfirmation']
    _frame(ctx, 'keep-question', 'history-cancel-all-confirmation', 'historyCancelAllConfirmation')
    ctx.expect(state['open'] and set(state['operationIds']) == set(ids), 'confirmation captures exact active identities')
    ctx.click('historyKeepUploading', window='historyCancelAllConfirmation')
    ctx.wait(lambda: not _state(ctx)['cancelAllConfirmation']['open'], 'Keep uploading dismisses confirmation')
    ctx.expect(_state(ctx)['activeOperationCount'] == len(ids), 'Keep uploading leaves all operations active')
    ctx.click('cancelAllBannerOperations', window='history')
    second = ctx.wait(lambda: (s if (s := _state(ctx))['cancelAllConfirmation']['open'] else None), 'second cancel-all confirmation opens')
    ctx.expect(set(second['cancelAllConfirmation']['operationIds']) == set(ids), 'second confirmation captures same identities')
    ctx.click('historyConfirmCancelAll', window='historyCancelAllConfirmation')
    ctx.wait(lambda: all(_result(ctx.call('operation.inspect', {'id': item}))['state'] == 'cancelled' for item in ids), 'all confirmed uploads cancel', timeout=30)
    ctx.expect(_state(ctx)['activeOperationCount'] == 0, 'banner clears canceled uploads')
    _frame(ctx, 'after')


@recipe('history.delete-remote', 'confirm.history-delete', needs=('history_delete',), destinations=('s3',))
def delete_remote(ctx):
    row = _row(ctx, 'history_delete')
    ctx.expect(ctx.destination == 's3', 'deletion recipe uses isolated moto destination')
    original = _result(ctx.call('history.detail', {'id': row['id']}))
    _frame(ctx, 'before')
    ctx.click('deleteHistoryRemote', window='history')
    ctx.frame('question', window='confirmAction')
    ctx.click('deleteConfirmationDetailsExpanded', window='confirmAction')
    ctx.click('copyDeletionTechnicalDetails', window='Deletion technical details')
    preparation = json.loads(ctx.clipboard_text())
    ctx.frame('technical-details', window='Deletion technical details')
    ctx.expect([target['historyId'] for target in preparation['targets']] == [row['id']], 'dialog prepares only exact fixture identity')
    ctx.keys('Esc')
    ctx.click('confirmCancel', window='confirmAction')
    ctx.expect(_result(ctx.call('history.detail', {'id': row['id']}))['remoteStatus'] == original['remoteStatus'], 'Cancel preserves remote state')
    ctx.click('deleteHistoryRemote', window='history')
    ctx.click('confirmActionButton', window='confirmAction')
    final = ctx.wait(lambda: (d if (d := _result(ctx.call('history.detail', {'id': row['id']})))['remoteStatus'] in ('removed', 'already_missing') else None), 'recorded moto object deleted', timeout=60)
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
        ctx.frame('more-menu-' + ('cache' if 'temporary' in action else 'records'), window='historyManagementMenu')
        ctx.click('text:' + action, window='historyManagementMenu')
        ctx.frame('question-' + ('cache' if 'temporary' in action else 'records'), window='confirmAction')
        ctx.click('text:Cancel', window='confirmAction')
        ctx.expect(_state(ctx)['total'] == before, 'Cancel preserves history')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear temporary files…', window='historyManagementMenu')
    ctx.click('confirmDiscard', window='confirmAction')
    ctx.wait(lambda: not _state(ctx)['busy'], 'temporary file cleanup finishes')
    ctx.expect(_state(ctx)['total'] == before, 'cache cleanup preserves history records')
    ctx.expect(_result(ctx.call('history.detail', {'id': ctx.fixture('history_rows')['image']['id']}))['local'] == local_before, 'cache cleanup preserves local-copy receipt')
    ctx.click('historyManagementExpanded', window='history')
    ctx.click('text:Clear history records…', window='historyManagementMenu')
    ctx.click('confirmDiscard', window='confirmAction')
    ctx.wait(lambda: not _state(ctx)['busy'] and _state(ctx)['total'] == 0, 'confirmed clear removes local history')
    _frame(ctx, 'after')
    ctx.review('Catalog has no empty-history reference or confirmation controls dump; inspect saved frames for empty state and both safe defaults.')


@recipe('shell.header-drag', needs=('history-row', 'history_window'))
def header_drag(ctx):
    _row(ctx)
    box = ctx.geometry('history')
    _frame(ctx, 'before')
    # Do not guess at header center: it contains mainNavigation. The core
    # resolves a blank hit target from the live layout/current-build catalog.
    start = tuple(ctx.fixture('history_window')['header_drag_point'])
    ctx.expect(box['x'] <= start[0] < box['x'] + box['width']
               and box['y'] <= start[1] < box['y'] + box['height'], 'header hit point is inside shell')
    ctx.drag(start, (start[0] + 64, start[1] + 48))
    moved = ctx.wait(lambda: (g if ((g := ctx.geometry('history'))['x'], g['y']) != (box['x'], box['y']) else None), 'real header drag moves window')
    ctx.expect((moved['width'], moved['height']) == (box['width'], box['height']), 'drag preserves size')
    ctx.call('ui.viewState')
    _frame(ctx, 'after')
    ctx.drag((start[0] + moved['x'] - box['x'], start[1] + moved['y'] - box['y']), start)


@recipe('shell.edge-resize', 'shell.minimum-size', needs=('history-row', 'history_window'))
def edge_resize(ctx):
    _row(ctx)
    ctx.fixture('history_window')
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


@recipe('shell.window-maximize', needs=('history-row', 'history_window'))
def maximize(ctx):
    _row(ctx)
    ctx.fixture('history_window')
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


@recipe('cli.version', needs=('launcher',))
def cli_version(ctx):
    _open(ctx)
    _frame(ctx, 'before')
    text = ctx.cli('--version')
    ctx.expect(text['returncode'] == 0 and text['stdout'].startswith('putgd '), 'launcher prints version')
    version = _cli(ctx, '--version-json')
    authoritative = _result(ctx.call('ui.snapshot'))['build']
    ctx.expect(version == authoritative, 'version JSON matches exact resident build')
    _frame(ctx, 'after')


@recipe('cli.launcher-history', 'cli.second-launch', needs=('history-row', 'launcher'))
def launcher_history(ctx):
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


@recipe('mcp.inspect', needs=('history-row',))
def mcp_inspect(ctx):
    _row(ctx)
    _frame(ctx, 'before')
    inspection = _result(ctx.mcp('inspect'))
    ctx.expect(set(inspection) == {'capabilities', 'diagnostics', 'configuration'}, 'MCP inspect returns three read-only inspections')
    view = _result(ctx.mcp('get_view_state'))
    ctx.expect(view == _result(ctx.call('ui.viewState')), 'MCP view-state matches resident')
    _frame(ctx, 'after')


# Explicit declarations, rather than optimistic click-and-review passes. These
# remain functional=blocked until the named setup/oracle exists. Each reason is
# independently included in the per-ID acceptance table generated from REGISTRY.
BLOCKED = {
    'history.list': 'Context has no double-click primitive for the row double-click path; key-enter and multi-select recipes cover the other row navigation paths.',
    'history.filters': 'Needs full date/MIME/destination/state fixture matrix and dynamic + Filter menu capture; clear-filters recipe exercises one upload filter only.',
    'history.row-primary': 'Needs historyRowPrimary controls captures for selected/hovered Retry upload/local save/deletion and Copy result variants; baseline catalog alone cannot resolve all states.',
    'history.row-menu': 'Needs fresh cancel/retry/local-copy/deletion row variants and dynamic menu inventory; key-menu proves only Copy details, links lane owns link actions.',
    'history.edit-again': 'Needs retained-image editor fixture with editor ID and immutable-source oracle; opening alone does not prove edit-again semantics.',
    'history.retry-deletion': 'Needs tray_progress-style moto failed-delete fixture with stable deletion ID plus queued Cancel deletion variant.',
    'history.save-diagnostics': 'Needs failed-row diagnostics fixture and private diagnostics directory/archive manifest oracle.',
    'history.local-copy-settings': 'Needs missing-local-copy guidance fixture; ui.viewState does not report the selected Settings tab.',
    'history.empty-states': 'Needs empty-history and injected load-failure fixtures; search recipe covers only No matching items.',
    'history.compact': 'Needs configured compact geometry and Back to list/gallery control capture; baseline width is not guaranteed by Context.',
    'shell.app-menu': 'Needs application-menu capture with dynamic actions; baseline history catalog contains its button only.',
    'shell.app-menu-capture-help': 'Needs capture-help dialog control inventory and close oracle.',
    'shell.app-menu-quit': 'Needs isolated resident lifecycle fixture/restart boundary; quitting a shared run would invalidate later recipes.',
    'shell.nav': 'Needs mainNavigation tab hit rectangles and active main.page oracle; ui.viewState currently exposes state/appearance/captureRouting only.',
    'shell.window-minimize': 'Needs WM minimized-state oracle plus real restore gesture; ui.viewState does not distinguish minimized from visible.',
    'shell.window-close': 'Needs helper-lifecycle fixture and resident-alive oracle; ui.viewState alone cannot prove close-with-resident behavior.',
    'shell.header-context-menu': 'Needs safe header hit target and custom system menu capture; menu is absent from baseline catalog.',
    'shell.button-layout': 'Needs isolated left/right GNOME button-layout fixture and current-build control rectangles; never mutate a shared desktop setting.',
    'shell.command-capture-region': 'Needs capture selector fixture, selection bounds and artifact oracle; must complete/cancel real selector before next recipe.',
    'shell.command-capture-window': 'Needs independent synthetic target window, real window selector completion and operation identity.',
    'shell.command-capture-desktop': 'Needs local-output capture fixture and captured artifact/hash oracle.',
    'shell.command-capture-edit': 'Needs region selector completion fixture and inspectable editor ID.',
    'shell.command-record': 'Needs recording selector fixture and cleanup through recording.cancel with exact recording ID.',
    'shell.command-upload-clipboard': 'Needs independent native clipboard fixture with payload/hash oracle, reused from tray_progress.py.',
    'shell.edit-source-menu': 'Needs clipboard image and native edit-file chooser fixtures plus current menu inventory.',
    'shell.drop-image': 'Context has pointer drag but no file-manager drag source fixture; shelling a synthetic drop event is not real input.',
    'shell.command-tooltips': 'Context has no pointer hover primitive; click_at would activate commands rather than reveal tooltips.',
    'shell.narrow-labels': 'Needs command-band accessible label/style projection at multiple widths; ui.viewState has no shell label oracle.',
    'shell.output-summary': 'Needs output-summary popup capture and Settings Capture navigation oracle.',
    'shell.output-warning': 'Needs no-output configuration fixture and current warning-strip capture.',
    'shell.action-recovery-banner': 'Needs rejected-action fixture for Change output/Keep local copies/Review destination variants.',
    'shell.autosave-status': 'Needs accepted/refused autosave fixtures and saved-status accessible projection.',
    'shell.edit-chooser-failure': 'Needs portal/chooser failure injection fixture, retry inventory and dismissal oracle.',
    'shell.first-launch': 'Needs new resident with no configuration under private XDG roots; cannot reset a shared Context resident.',
    'cli.launcher-background': 'Needs stopped-resident fixture, tray-absent variant and lifecycle reset; Context.cli currently documents putgd ARGS only.',
    'cli.action-invoke': 'Needs native source and six action completion fixtures; capture/record selectors cannot be left active.',
    'cli.history': 'Needs fresh operation/delete/share fixtures for the composite CLI history.*, operation.*, delete.*, share.inspect interaction.',
    'mcp.perform-action': 'Needs native source and six action completion fixtures for perform_action.',
    'mcp.operations': 'Needs independent failed/local-save/active operation fixtures and clipboard oracle for get/retry/copy/cancel tool matrix.',
    'mcp.history': 'Needs fresh deletion/diagnostics/preview fixtures; composite ID also lists link tools assigned to links lane and requires coordinator scope clarification.',
}


def _declare_blocked(interaction, reason):
    def declaration(ctx):
        ctx.blocked(reason, issue=675)
    declaration.__name__ = 'blocked_' + interaction.replace('.', '_').replace('-', '_')
    recipe(interaction)(declaration)


for _interaction, _reason in BLOCKED.items():
    _declare_blocked(_interaction, _reason)
