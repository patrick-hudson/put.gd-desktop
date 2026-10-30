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
        self.focused_control = ''
        self.typed = ''
        self.clipboard = ''
        self.category_index = 0
        self.window = dict(x=100, y=80, width=1400, height=950, scale=1, pid=123)
        self.minimized = False
        self.closed = False
        self.saved_window = None
        self.preview = False
        self.receipt = False
        self.chooser = False
        self.question = None
        self.menu = None
        self.help = False
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
        aliases = {
            'text:Retry upload': 'retryOperation', 'text:Retry local save': 'retryOperation',
            'text:Next': 'historyNext', 'text:Previous': 'historyPrevious',
            'text:Clear selection': 'historyClearSelection', 'text:Clear filters': 'historyClearFilters',
            'text:Keep uploading': 'historyKeepUploading', 'text-glob:Cancel * uploads': 'historyConfirmCancelAll',
            'text-glob:* more active': 'historyMoreActive', 'text-glob:Cancel this upload:*': 'cancelBannerOperation',
            'text-glob:Cancel all (*)…': 'cancelAllBannerOperations', 'text:Upload state': 'historyAddFilteruploadStatus',
            'text:Save a copy…': 'saveHistoryAsAction', 'text:Close preview': 'closeHistoryPreview',
            'text:Delete from destination…': 'deleteHistoryRemote',
            'text:Show the full deletion preparation': 'deleteConfirmationDetailsExpanded',
            'text:Copy technical details': 'copyDeletionTechnicalDetails',
            'text-glob:Delete * file*': 'confirmActionButton',
            'text:Clear temporary files': 'confirmDiscard', 'text:Clear history records': 'confirmDiscard',
        }
        control = aliases.get(control, control)
        self.events.append(('click', control, window, button))
        self.focused_control = control
        if control == 'applicationMenu':
            self.menu = 'app'
        elif control == 'text:Capture actions':
            self.menu = None
            self.help = True
        elif control == 'text:Close':
            self.help = False
        elif control == 'historyGridMode':
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
            self.focused_control = 'historyPreview'
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
                if self.focused_control == 'historyCategories':
                    self.category_index = 0
                    self.query.update(category='', uploadStatus='')
                elif self.focused_control == 'historyList':
                    self.selected = ['image-1']
            elif key == 'Down' and self.focused_control == 'historyCategories':
                self.category_index += 1
                category = ['', 'images', 'videos', 'text+other', ''][self.category_index]
                self.query.update(category=category, uploadStatus='failed' if self.category_index == 4 else '')
            elif key == 'Down' and self.focused_control == 'historyChoiceuploadStatus':
                self.query['uploadStatus'] = 'uploaded'
            elif key == 'Backspace' and self.focused_control == 'historySearch':
                self.typed = ''
            elif key == 'Return' and self.focused_control == 'historySearch':
                self.query['search'] = self.typed
            elif key == 'Return' and self.focused_control == 'historyList':
                self.details = self.grid
            elif key == 'Shift+Down':
                self.selected = ['image-1', 'image-2']
            elif key == 'Esc':
                self.menu = None
                if self.preview:
                    self.preview = False
                    self.focused_control = 'historyPreview'
                elif self.chooser:
                    self.chooser = False
                else:
                    self.details = False
            elif key == 'Space' and self.focused_control == 'historyPreview':
                self.preview = True
            elif key == 'Ctrl+C' and self.focused_control == 'historyDetails':
                self.clipboard = json.dumps(self.detail())

    def type_text(self, text):
        self.events.append(('type_text', text))
        if text:
            self.typed = text

    def focus(self, window, raise_window=True):
        self.events.append(('focus', window, raise_window))
        if window == 'fileDialog' and not self.chooser:
            raise AssertionError('chooser must be visible before binding input')

    def click_accessible(self, query, button='left'):
        if query.get('name') == 'Upload state' and query.get('role') == 'combo box':
            return self.click('historyChoiceuploadStatus', 'history', button)
        if not query.get('owned') or query.get('name') != 'Expand preview' or query.get('pid') != 123:
            raise AssertionError('preview click must constrain private family and observed pid')
        control = {'icon': 'historyPreview', 'label': 'historyPreview', 'push button': 'expandHistoryPreview'}[query['role']]
        self.click(control, 'history', button)

    def click_at(self, x, y, button='left', head=None):
        self.events.append(('click', 'header', 'history', button))
        self.menu = 'header'

    def accessible(self, query):
        if query.get('name') == 'Expand preview':
            return [{'name': 'Expand preview', 'role': role, 'pid': 123, 'rect': [100, 100, 40, 40]} for role in ('icon', 'push button')]
        if query.get('role') == 'menu item':
            return [{'name': n} for n in ({'Capture actions', 'Check for updates', 'About put.gd', 'Quit put.gd'} if self.menu == 'app' else {'Minimize', 'Maximize', 'Close'} if self.menu == 'header' else set())]
        if query.get('name') == 'Capture actions':
            return [{'name': 'Capture actions'}] if self.help else []
        if query.get('name') == 'Help':
            return [{'name': 'Help'}] if self.help else []
        if 'within' in query:
            return [{'name': 'Application menu', 'role': 'push button',
                     'rect': [self.window['x'] + 5, self.window['y'] + 5, 24, 28]},
                    {'name': 'Navigation', 'role': 'page tab list',
                     'rect': [self.window['x'] + 450, self.window['y'] + 5, 300, 28]}]
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
        return {'returncode': 0, 'stdout': ('putgd-cli 26.1029.42-nightly\n' if self.fault == 'launcher-routed-to-client' else 'putgd 26.1029.42-nightly\n') if args[0] == '--version' else json.dumps(value), 'stderr': ''}

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

    def test_launcher_dispatch_to_resident_client_is_a_core_blocker(self):
        ctx = FakeContext(('launcher',), fault='launcher-routed-to-client')
        with self.assertRaises(RecipeBlocked) as error:
            history.cli_version(ctx)
        self.assertEqual(error.exception.issue, 675)
        self.assertIn('putgd-cli', error.exception.reason)
        self.assertFalse(any(e == ('cli', ('--version-json',)) for e in ctx.events))

    def test_geometry_rows_use_supported_accessibility_filters_without_window_fixture(self):
        class NativeContext(FakeContext):
            def windows(self, query=None):
                raise AssertionError('geometry must not send X11 title queries')
            def accessible(self, query):
                if set(query) - {'owned', 'visible', 'pid', 'within'}:
                    raise AssertionError('unsupported accessible header query')
                return super().accessible(query)
        for action in (history.header_drag, history.edge_resize):
            entry = next(r for r in LANE if r.function == action)
            self.assertEqual(entry.needs, ('history-row',))
            action(NativeContext(entry.needs))

    def test_header_point_excludes_navigation_and_refuses_covered_header(self):
        ctx = FakeContext()
        point = history._header_point(ctx, ctx.window)
        self.assertFalse(550 <= point[0] <= 850)
        ctx.accessible = lambda query: [
            {'name': 'Application menu', 'role': 'push button', 'rect': [100, 80, 1400, 40]}]
        with self.assertRaisesRegex(RecipeBlocked, 'no verified blank header'):
            history._header_point(ctx, ctx.window)

    def test_native_window_observation_uses_accessible_query_and_iconified_state(self):
        from tools.lab.surface_driver.profiles import load_profile
        class NativeContext(FakeContext):
            profile = load_profile('ubuntu-gnome-wayland')
            def windows(self, query=None):
                self.assert_native_query(query)
                if self.closed or (self.minimized and query.get('visible')):
                    return []
                return [{'states': {'iconified': self.minimized}}]
            def assert_native_query(self, query):
                if set(query) - {'name', 'role', 'visible'} or query['name'] != 'put.gd':
                    raise AssertionError('X11 query sent to native accessibility inventory')
        for action in (history.minimize, history.close_window):
            with self.subTest(action=action.__name__):
                action(NativeContext(('history-row',)))

    def test_read_only_command_entries_do_not_require_pointer_geometry(self):
        for action, needs in ((history.cli_version, ('launcher',)),
                              (history.mcp_inspect, ('history-row',))):
            ctx = FakeContext(needs)
            action(ctx)
            self.assertFalse(any(e[0] in ('click', 'drag', 'keys', 'type_text') for e in ctx.events))

    def test_older_catalog_lists_resolve_through_supported_profile_aliases(self):
        import tempfile
        from tools.lab.surface_driver.catalog import Catalog
        from tools.lab.surface_driver.probe import selector_matches
        from tools.lab.surface_driver.profiles import load_profile
        source = (ROOT / 'src/ui/HistoryWindow.cpp').read_text()
        names = {'historyList': 'History items and upload activity', 'historyCategories': 'Show history items', 'historyAddFilter': 'Add a history filter'}
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, '01-history-list.controls.json').write_text(json.dumps({'controls': [
                {'name': control, 'text': '', 'class': 'QListView'} for control in names]}))
            for desktop in ('mint-cinnamon-x11', 'ubuntu-gnome-wayland'):
                catalog = Catalog(directory, load_profile(desktop))
                for control, label in names.items():
                    self.assertIn('setAccessibleName("' + label + '")', source)
                    self.assertEqual(selector_matches({'name': label, 'objectName': ''}, control,
                                                      catalog.candidates(control, 'history')), 1)

    def test_popup_frames_do_not_require_a_native_menu_window(self):
        class PopupContext(FakeContext):
            def frame(self, label, window=None, reference=None):
                if window and (window.endswith('Menu') or window.endswith('Popup')):
                    raise AssertionError('runtime popup is not an independently titled window')
                return super().frame(label, window, reference)
        for action in (history.application_menu, history.header_context_menu, history.key_menu, history.inspector_more):
            entry = next(r for r in LANE if r.function == action)
            action(PopupContext(entry.needs))

    def test_file_chooser_input_requires_explicit_target_binding(self):
        class ChooserContext(FakeContext):
            input_window = 'history'
            def focus(self, window, raise_window=True):
                super().focus(window, raise_window)
                self.input_window = window
            def keys(self, *sequence):
                if self.chooser and self.input_window != 'fileDialog':
                    raise AssertionError('keyboard would target History behind the chooser')
                super().keys(*sequence)
        history.inspector_more(ChooserContext(('history-row', 'file-dialog-fallback')))

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
