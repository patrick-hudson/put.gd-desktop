#!/usr/bin/env python3
"""History recipe contract tests. This fake is not desktop acceptance evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import Context, Frame, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import history


class FakeContext(Context):
    desktop = 'mint-cinnamon-x11'
    host = 'boat'
    placement = 'boat'
    package = 'deb'
    destination = 's3'
    profile = {}

    def __init__(self, needs=(), fault=None):
        self.needs = needs
        self.fault = fault
        self.events = []
        self.frames = []
        self.reviews = []
        self.focus = ''
        self.typed = ''
        self.clipboard = ''
        self.category_index = 0
        self.window = dict(x=100, y=80, width=1400, height=950, scale=1)
        self.minimized = False
        self.closed = False
        self.saved_window = None
        self.preview = False
        self.receipt = False
        self.chooser = False
        self.question = None
        self.total = 60 if 'history_paging' in needs else 2 if 'history_pair' in needs else 5
        self.operations = {i: {'id': i, 'state': 'running'} for i in ('op-1', 'op-2')}
        self.local = {'status': 'saved', 'path': '/tmp/private/copy.png'}
        self.remote = 'present'
        self.upload = 'failed'
        self.query = dict(limit=50, offset=0, search='', category='', uploadStatus='')
        self.selected = []
        self.grid = False
        self.details = False
        self.active = 2 if 'history_active_uploads' in needs else 0
        self.confirmation = {'open': False, 'operationIds': []}
        self.saved = 'history_saved_copy' in needs
        self.folder = {}
        self.fixture_row = {'id': 'image-1', 'name': 'surface-history-image.png', 'operation_id': 'op-1'}
        self.build = {'version': '26.1029.42-nightly', 'commit': '0123456789'}

    def fixture(self, name):
        self.events.append(('fixture', name))
        if name not in self.needs:
            raise AssertionError('undeclared fixture ' + name)
        row = dict(self.fixture_row)
        if name == 'history-row':
            return SimpleNamespace(id='image-1', history_id='image-1', row=row, operation_id='op-1')
        if name == 'history_window':
            return {'header_drag_point': [450, 104]}
        if name == 'history_rows':
            return {'image': row, 'category_counts': {'': 5, 'images': 2, 'videos': 1, 'text+other': 2, 'failed': 1}}
        if name == 'history_pair':
            return {'ids': ['image-1', 'image-2'], 'bytes': 3 * 1024 * 1024}
        if name == 'history_paging':
            return {'total': 60}
        if name == 'history_active_uploads':
            return {'operation_ids': ['op-1', 'op-2']}
        if name == 'history_retry_local':
            self.local = {'status': 'failed', 'path': ''}
        if name == 'history_saved_copy':
            row['path'] = '/tmp/private/copy.png'
        return row

    def state(self):
        counts = {'': 5, 'images': 2, 'videos': 1, 'text+other': 2}
        total = (counts.get(self.query['category'], 5) if self.query['category'] else self.total)
        if self.query['uploadStatus'] == 'failed':
            total = 1
        rows = min(total, 50)
        if self.query['search']:
            total = rows = 1 if self.query['search'] == self.fixture_row['name'] else 0
        return dict(busy=False, query=copy.deepcopy(self.query), rows=rows, total=total,
                    selectedIds=list(self.selected), selectedRows=len(self.selected), gridMode=self.grid,
                    activeFilters=['Upload state: Uploaded'] if self.query['uploadStatus'] and self.query['uploadStatus'] != 'failed' else [],
                    detailsOpen=self.details, inspectorVisible=bool(self.selected), canExpandPreview=bool(self.selected),
                    previewExpanded=self.preview, selectionSummary={'bytes': 3 * 1024 * 1024},
                    activeOperationCount=self.active, cancelAllConfirmation=copy.deepcopy(self.confirmation),
                    savedCopyVisible=self.saved, savedPath='/tmp/private/copy.png', savedCopyFolder=self.folder)

    def detail(self):
        return dict(id='image-1', name=self.fixture_row['name'], bytes=1024 * 1024,
                    records=[{'id': 'image-1'}], local=copy.deepcopy(self.local),
                    remoteStatus=self.remote, uploadStatus=self.upload)

    def view(self, method, params=None):
        self.events.append(('view', method, params))
        if method == 'ui.show':
            if params != {'view': 'history'}:
                raise AssertionError('invalid ui.show params')
            self.minimized = self.closed = False
            return {'shown': 'history'}
        if method != 'ui.history':
            raise AssertionError('unexpected setup mutation: ' + method)
        action = params['action']
        if action == 'clearFilters':
            self.query.update(search='', category='', uploadStatus='', offset=0)
        elif action == 'closePreview':
            self.preview = False
        elif action == 'closeDetails':
            self.details = False
        elif action == 'clearSelection':
            self.selected = []
        else:
            raise AssertionError('unexpected setup action: ' + action)
        return self.state()

    def call(self, method, params=None):
        self.events.append(('call', method, params))
        if method == 'ui.history':
            if params != {'action': 'inspect'}:
                raise AssertionError('interaction used resident mutation instead of input')
            return self.state()
        if method == 'ui.viewState':
            return {'state': 'ready'}
        if method == 'history.detail':
            return self.detail()
        if method == 'ui.fileDialog':
            if params['action'] != 'inspect':
                raise AssertionError('file chooser mutation')
            return {'open': self.chooser, 'mode': 'save' if self.chooser else None}
        if method == 'operation.inspect':
            return copy.deepcopy(self.operations[params['id']])
        if method == 'ui.snapshot':
            return {'build': self.build}
        raise AssertionError('unknown production method ' + method)

    def click(self, control, window, button='left', offset=None):
        self.events.append(('click', control, window, button))
        self.focus = control
        if control == 'historyGridMode':
            self.grid = True
        elif control == 'historyListMode':
            self.grid = False
            self.details = False
        elif control == 'historyList':
            self.selected = ['image-1']
            self.details = self.grid and button == 'left'
        elif control == 'historyClearFilters':
            self.query['uploadStatus'] = ''
        elif control == 'historyPreview' or control == 'expandHistoryPreview':
            self.preview = self.fault != 'preview-no-effect'
        elif control == 'closeHistoryPreview':
            self.preview = False
            self.focus = 'historyPreview'
        elif control == 'historyReceiptExpanded':
            self.receipt = not self.receipt
        elif control == 'historyClearSelection':
            self.selected = []
        elif control == 'historyNext':
            self.query['offset'] = 50
        elif control == 'historyPrevious':
            self.query['offset'] = 0
        elif control == 'text:Copy details':
            self.clipboard = json.dumps({'records': [{'id': 'wrong' if self.fault == 'wrong-details' else 'image-1'}]})
        elif control == 'saveHistoryAsAction':
            self.chooser = True
        elif control == 'copyHistorySavedPath':
            self.clipboard = '/tmp/private/copy.png'
        elif control == 'showHistorySavedCopy':
            self.folder = {'opened': True}
        elif control == 'dismissHistorySavedCopy':
            self.saved = False
        elif control == 'retryOperation':
            self.operations['op-1']['state'] = 'completed'
            if self.fault == 'retry-new-identity':
                self.operations['op-1']['id'] = 'replacement-operation'
            self.upload = 'uploaded'
            self.local = {'status': 'saved', 'path': '/tmp/private/copy.png'}
        elif control == 'historyMoreActive':
            self.query['uploadStatus'] = 'uploading'
        elif control == 'cancelBannerOperation':
            self.operations['op-1']['state'] = 'cancelled'
            self.active -= 1
        elif control == 'cancelAllBannerOperations':
            self.confirmation = {'open': True, 'operationIds': ['wrong'] if self.fault == 'wrong-cancel-ids' else ['op-1', 'op-2']}
        elif control == 'historyKeepUploading':
            self.confirmation['open'] = False
        elif control == 'historyConfirmCancelAll':
            self.confirmation['open'] = False
            for item in self.operations.values():
                item['state'] = 'cancelled'
            self.active = 0
        elif control == 'deleteHistoryRemote':
            self.question = 'delete'
        elif control == 'copyDeletionTechnicalDetails':
            self.clipboard = json.dumps({'targets': [{'historyId': 'wrong' if self.fault == 'wrong-delete-id' else 'image-1'}]})
        elif control == 'confirmCancel' or control == 'text:Cancel':
            self.question = None
        elif control == 'confirmActionButton':
            self.remote = 'removed'
        elif control == 'text:Clear temporary files…':
            self.question = 'cache'
        elif control == 'text:Clear history records…':
            self.question = 'clear'
        elif control == 'confirmDiscard':
            if self.question == 'clear':
                self.total = 0
        elif control == 'windowMinimize':
            self.minimized = self.fault != 'minimize-no-effect'
        elif control == 'windowClose':
            self.closed = self.fault != 'close-no-effect'
        elif control == 'windowMaximize':
            if self.saved_window:
                self.window, self.saved_window = self.saved_window, None
            else:
                self.saved_window = copy.deepcopy(self.window)
                self.window.update(x=0, y=0, width=1920, height=1080)
        elif control not in {
            'historySearch', 'historyCategories', 'historyFiltersExpanded', 'historyAddFilter',
            'historyAddFilteruploadStatus', 'historyChoiceuploadStatus', 'historyDetails',
            'historySelectionMore', 'historyInspectorMore', 'historyManagementExpanded',
            'deleteConfirmationDetailsExpanded',
        }:
            raise AssertionError('unexpected control: ' + control)

    def keys(self, *sequence):
        self.events.append(('keys', sequence))
        for key in sequence:
            if key == 'Home':
                if self.focus == 'historyCategories':
                    self.category_index = 0
                    self.query.update(category='', uploadStatus='')
                elif self.focus == 'historyList':
                    self.selected = ['image-1']
            elif key == 'Down' and self.focus == 'historyCategories':
                self.category_index += 1
                category = ['', 'images', 'videos', 'text+other', ''][self.category_index]
                self.query.update(category=category, uploadStatus='failed' if self.category_index == 4 else '')
            elif key == 'Down' and self.focus == 'historyChoiceuploadStatus':
                self.query['uploadStatus'] = 'uploaded'
            elif key == 'Backspace' and self.focus == 'historySearch':
                self.typed = ''
            elif key == 'Return' and self.focus == 'historySearch':
                self.query['search'] = self.typed
            elif key == 'Return' and self.focus == 'historyList':
                self.details = self.grid
            elif key == 'Shift+Down':
                self.selected = ['image-1', 'image-2']
            elif key == 'Esc':
                if self.preview:
                    self.preview = False
                    self.focus = 'historyPreview'
                elif self.chooser:
                    self.chooser = False
                else:
                    self.details = False
            elif key == 'Space' and self.focus == 'historyPreview':
                self.preview = True
            elif key == 'Ctrl+C' and self.focus == 'historyDetails':
                self.clipboard = json.dumps(self.detail())

    def type_text(self, text):
        self.events.append(('type_text', text))
        if text:
            self.typed = text

    def accessible(self, query):
        return [{'name': 'Total size: 3 MB' if query['name'].startswith('Total') else 'Size: 1 MB · 640×320'}]

    def clipboard_text(self):
        return self.clipboard

    def windows(self, query=None):
        if self.closed or (self.minimized and not (query or {}).get('include_hidden')):
            return []
        return [{'title': 'put.gd', 'states': {'minimized': self.minimized}}]

    def geometry(self, window):
        return copy.deepcopy(self.window)

    def drag(self, start, end, button='left', steps=12):
        self.events.append(('drag', start, end))
        dx, dy = end[0] - start[0], end[1] - start[1]
        if start[1] < self.window['y'] + 48:
            self.window['x'] += dx
            self.window['y'] += dy
        else:
            self.window['width'] = max(1060, self.window['width'] + dx)
            self.window['height'] = max(680, self.window['height'] + dy)

    def cli(self, *args, timeout=60):
        self.events.append(('cli', args))
        value = self.build if args[0] == '--version-json' else {}
        return {'returncode': 0, 'stdout': 'putgd 26.1029.42-nightly\n' if args[0] == '--version' else json.dumps(value), 'stderr': ''}

    def mcp(self, tool, arguments=None):
        self.events.append(('mcp', tool, arguments))
        return {'ok': True, 'result': {'capabilities': {}, 'diagnostics': {}, 'configuration': {}} if tool == 'inspect' else {'state': 'ready'}}

    def wait(self, predicate, what, timeout=10, interval=.2):
        for _ in range(3):
            result = predicate()
            if result:
                return result
        raise RecipeFailed(what)

    def frame(self, label, window=None, reference=None):
        self.frames.append((label, window, reference))
        return Frame(label, label + '.png', window, reference)

    def review(self, reason):
        self.reviews.append(reason)

    def note(self, text):
        self.events.append(('note', text))


LANE = [r for r in REGISTRY if r.function.__module__ == history.__name__]


class HistoryRecipeTests(unittest.TestCase):
    def test_exactly_the_owned_71_ids_once(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        expected = {key for key, owner in owners.items() if owner == 'history'}
        actual = [key for item in LANE for key in item.interactions]
        self.assertEqual(len(expected), 71)
        self.assertEqual(len(actual), len(set(actual)))
        self.assertEqual(set(actual), expected)

    def test_every_executable_recipe_uses_input_inspection_and_reference_frames(self):
        for item in LANE:
            if item.interactions[0] in history.BLOCKED:
                continue
            with self.subTest(recipe=item.name):
                ctx = FakeContext(item.needs)
                item.function(ctx)
                self.assertTrue(any(event[0] in {'click', 'drag', 'cli', 'mcp'} for event in ctx.events))
                self.assertTrue(any(event[0] == 'call' for event in ctx.events))
                referenced = {label for label, _, reference in ctx.frames if reference}
                self.assertTrue({'before', 'after'} <= referenced)

    def test_all_blocked_declarations_raise_with_specific_reason(self):
        for item in LANE:
            if item.interactions[0] not in history.BLOCKED:
                continue
            with self.subTest(interaction=item.interactions[0]):
                with self.assertRaises(RecipeBlocked) as error:
                    item.function(FakeContext())
                self.assertEqual(error.exception.reason, history.BLOCKED[item.interactions[0]])
                self.assertGreater(len(error.exception.reason), 50)

    def test_references_name_existing_catalog_captures(self):
        source = (ROOT / 'tests/gui/ui_catalog.cpp').read_text()
        for item in LANE:
            if item.interactions[0] in history.BLOCKED:
                continue
            ctx = FakeContext(item.needs)
            item.function(ctx)
            for _, _, reference in ctx.frames:
                if reference:
                    self.assertIn('"' + reference, source, reference)

    def test_preview_without_effect_fails(self):
        with self.assertRaisesRegex(RecipeFailed, 'preview click expands'):
            history.preview(FakeContext(('history-row',), fault='preview-no-effect'))

    def test_wrong_row_menu_receipt_fails(self):
        with self.assertRaisesRegex(RecipeFailed, 'selected receipt'):
            history.key_menu(FakeContext(('history-row',), fault='wrong-details'))

    def test_retry_replacing_operation_identity_fails(self):
        ctx = FakeContext(('history_retry_upload', 'history_retry_local'), fault='retry-new-identity')
        with self.assertRaisesRegex(RecipeFailed, 'preserves operation identity'):
            history.retry_upload(ctx)

    def test_wrong_cancel_identity_never_confirms(self):
        ctx = FakeContext(('history_active_uploads',), fault='wrong-cancel-ids')
        with self.assertRaisesRegex(RecipeFailed, 'exact active identities'):
            history.cancel_all(ctx)
        self.assertNotIn('historyConfirmCancelAll', [e[1] for e in ctx.events if e[0] == 'click'])
        self.assertTrue(all(op['state'] == 'running' for op in ctx.operations.values()))

    def test_wrong_deletion_identity_never_confirms(self):
        ctx = FakeContext(('history_delete',), fault='wrong-delete-id')
        with self.assertRaisesRegex(RecipeFailed, 'exact fixture identity'):
            history.delete_remote(ctx)
        self.assertNotIn('confirmActionButton', [e[1] for e in ctx.events if e[0] == 'click'])
        self.assertEqual(ctx.remote, 'present')

    def test_typing_empty_text_does_not_clear_selected_text(self):
        ctx = FakeContext()
        ctx.typed = 'unchanged'
        ctx.type_text('')
        self.assertEqual(ctx.typed, 'unchanged')
        history._search(ctx, '')
        self.assertEqual(ctx.query['search'], '')
        self.assertIn(('keys', ('Backspace',)), ctx.events)

    def test_cancel_all_waits_for_delayed_dialog_visibility(self):
        class DelayedContext(FakeContext):
            pending = 0

            def click(self, control, window, button='left', offset=None):
                if control == 'historyConfirmCancelAll' and not self.confirmation['open']:
                    raise RecipeFailed('confirmation is not open')
                super().click(control, window, button, offset)
                if control == 'cancelAllBannerOperations':
                    self.confirmation['open'] = False
                    self.pending = 2

            def state(self):
                result = super().state()
                if self.pending:
                    self.pending -= 1
                    if not self.pending:
                        self.confirmation['open'] = True
                return result

        history.cancel_all(DelayedContext(('history_active_uploads',)))

    def test_setup_waits_for_async_query_before_selection_mutation(self):
        class BusyContext(FakeContext):
            pending = 0

            def view(self, method, params=None):
                if method == 'ui.history':
                    action = params['action']
                    if self.pending and action in ('closeDetails', 'clearSelection'):
                        raise RecipeFailed('history_busy')
                    if action == 'clearFilters':
                        self.pending = 3
                return super().view(method, params)

            def state(self):
                result = super().state()
                result['busy'] = self.pending > 0
                self.pending = max(0, self.pending - 1)
                return result

        history._open(BusyContext())

    def test_setup_preserves_live_window_size(self):
        ctx = FakeContext()
        original = copy.deepcopy(ctx.window)
        history._open(ctx)
        self.assertEqual(ctx.window, original)
        self.assertFalse(any(e[0] == 'drag' for e in ctx.events))

    def test_window_buttons_require_observed_effect_and_restore_on_failure(self):
        for action, fault in ((history.minimize, 'minimize-no-effect'),
                              (history.close_window, 'close-no-effect')):
            with self.subTest(action=action.__name__):
                ctx = FakeContext(('history-row',), fault=fault)
                with self.assertRaises(RecipeFailed):
                    action(ctx)
                self.assertFalse(ctx.closed)
                self.assertFalse(ctx.minimized)
                self.assertEqual(ctx.events[-1], ('view', 'ui.show', {'view': 'history'}))

    def test_history_popup_owners_are_scoped_on_both_guests(self):
        from tools.lab.surface_driver.profiles import load_profile
        for desktop in ('mint-cinnamon-x11', 'ubuntu-gnome-wayland'):
            profile = load_profile(desktop)
            for name in ('historyAddFilterMenu', 'historySelectionMore',
                         'historyInspectorMenu', 'historyManagementMenu'):
                self.assertEqual(profile['windows'][name], {'owner': 'history', 'popup': True})
            self.assertNotIn('history_catalog_size', profile)

    def test_loopback_mutations_allow_local_destination(self):
        for name in ('active_banner', 'cancel_all', 'delete_remote'):
            entry = next(r for r in LANE if r.function.__name__ == name)
            self.assertIn('local', entry.destinations)

    def test_no_mutation_calls_used_as_input(self):
        ctx = FakeContext()
        with self.assertRaisesRegex(AssertionError, 'instead of input'):
            ctx.call('ui.history', {'action': 'confirmCancelAll'})

    def test_decimal_size_or_binary_unit_labels_are_rejected(self):
        ctx = FakeContext()
        ctx.accessible = lambda query: [{'name': 'Size: 1.05 MB'}]
        with self.assertRaisesRegex(RecipeFailed, '1024 arithmetic'):
            history._size(ctx, 1024**2)
        ctx.accessible = lambda query: [{'name': 'Size: 1 MiB'}]
        with self.assertRaisesRegex(RecipeFailed, 'KB/MB/GB'):
            history._size(ctx, 1024**2)

    def test_failed_envelope_is_never_treated_as_result(self):
        with self.assertRaisesRegex(AssertionError, 'fixture_missing'):
            history._result({'ok': False, 'error': {'code': 'fixture_missing'}, 'result': {}})

    def test_clear_history_uses_generic_confirmation_button(self):
        ctx = FakeContext(('history_rows',))
        history.clear_history(ctx)
        controls = [e[1] for e in ctx.events if e[0] == 'click']
        self.assertIn('confirmDiscard', controls)
        self.assertNotIn('confirmActionButton', controls)
        self.assertEqual(ctx.total, 0)


if __name__ == '__main__':
    unittest.main()
