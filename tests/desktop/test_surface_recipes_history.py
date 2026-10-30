#!/usr/bin/env python3
"""History recipe contract tests. This fake is not desktop acceptance evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch
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
        if name == 'launcher':
            return SimpleNamespace(executable='putgd')
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
        if name == 'history_retry_upload':
            self.operations['op-1']['state'] = 'partial'
        if name == 'history_retry_local':
            self.operations['op-1']['state'] = 'failed'
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
        if method == 'operation.cancel':
            return {'cancelled': True}
        if method == 'operation.inspect':
            return copy.deepcopy(self.operations[params['id']])
        if method == 'ui.snapshot':
            return {'build': self.build, 'residentEpoch': 'resident-1'}
        raise AssertionError('unknown production method ' + method)

    def click(self, control, window, button='left', offset=None):
        aliases = {
            'text:More selection actions': 'historySelectionMore', 'text:Add a history filter': 'historyAddFilter', 'text:Advanced original receipt details': 'historyDetails',
            'text:Retry upload': 'retryOperation', 'text:Retry local save': 'retryOperation',
            'text:Next': 'historyNext', 'text:Previous': 'historyPrevious',
            'text:Maximize': 'windowMaximize', 'text:Restore': 'windowMaximize',
            'text:Minimize': 'windowMinimize', 'text:Close window': 'windowClose',
            'text:Clear selection': 'historyClearSelection', 'text:Clear filters': 'historyClearFilters',
            'text:Keep uploading': 'historyKeepUploading', 'text:Cancel 2 uploads': 'historyConfirmCancelAll',
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
        self.focused_control = 'historySearch' if control == 'historyList' and self.fault == 'list-cannot-focus' else control
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
            self.selected = [] if self.fault == 'row-click-no-effect' else ['image-1']
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
            self.menu = None
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
            self.operations['op-1']['state'] = 'failed' if self.local['status'] == 'failed' else 'partial' if self.fault == 'upload-partial-result' else 'completed'
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
        elif control == 'historyAddFilteruploadStatus':
            self.focused_control = 'historyChoiceuploadStatus'
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
            elif key == 'Tab' and self.fault != 'list-cannot-focus':
                self.focused_control = 'historyList'
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
            elif key in ('Menu', 'Shift+F10'):
                self.menu = 'row'
            elif key == 'Ctrl+C' and self.focused_control == 'historyDetails':
                self.clipboard = json.dumps(self.detail())

    def type_text(self, text):
        self.events.append(('type_text', text))
        if text:
            self.typed = text

    def focus(self, window, raise_window=True):
        self.events.append(('focus', window, raise_window))
        if window == 'historySaveDialog' and not self.chooser:
            raise AssertionError('chooser must be visible before binding input')

    def click_accessible(self, query, button='left'):
        raise AssertionError('app children must use the normal guarded window input path')

    def click_at(self, x, y, button='left', head=None):
        if (x, y) == (650, 620):
            name = self.pending_button
            return self.click(('text-glob:' if '*' in name else 'text:') + name, self.pending_button_window, button)
        if (x, y) == (720, 220):
            self.events.append(('delegate', copy.deepcopy(self.last_delegate_query)))
            return self.click('historyList', 'history', button)
        if (x, y) == (1250, 270):
            return self.click('historyClearFilters', 'history', button)
        if (x, y) == (1220, 220):
            return self.click('historyChoiceuploadStatus', 'history', button)
        if (x, y) in ((1220, 420), (1320, 420)):
            return self.click('historyPreview' if x == 1220 else 'expandHistoryPreview', 'history', button)
        self.events.append(('click', 'header', 'history', button))
        self.menu = 'header'

    def accessible(self, query):
        if query.get('role') == 'push button' and query.get('name') in ('Cancel', 'Keep uploading', 'Cancel 2 uploads', 'Delete * file*', 'Clear temporary files', 'Clear history records', 'Maximize', 'Restore', 'Minimize', 'Close window'):
            if query.get('pid') != 123 or not query.get('owned') or not query.get('within'):
                raise AssertionError('confirmation input must constrain native owner and dialog bounds')
            self.pending_button = query['name']
            self.pending_button_window = 'history' if query['name'] in ('Maximize', 'Restore', 'Minimize', 'Close window') else 'historyCancelAllConfirmation' if query['name'] in ('Keep uploading', 'Cancel 2 uploads') else 'historyConfirmAction'
            return [{'rect': [600, 600, 100, 40]}]
        if query.get('name') == 'Clear filters' and query.get('role') == 'push button':
            return [{'name': 'Clear filters', 'rect': [1200, 250, 100, 40]}]
        if query.get('name') == 'History items and upload activity':
            if query.get('states', {}).get('focused') and self.focused_control != 'historyList':
                return []
            return [{'name': query['name'], 'rect': [320, 200, 800, 650], 'pid': 123, 'states': {'focused': self.focused_control == 'historyList'}}]
        if query.get('role') == 'list item':
            if not query.get('owned') or query.get('pid') != 123 or query.get('within') != [320, 200, 800, 650]:
                raise AssertionError('row input requires the owned live list bounds')
            self.last_delegate_query = query
            return [{'rect': [320, 200, 800, 40], 'states': {'focused': self.focused_control == 'historyList'}}]
        if query.get('name') == 'Upload state' and query.get('role') == 'combo box':
            return [{'rect': [1200, 200, 40, 40]}]
        if query.get('name') == 'Expand preview':
            if query.get('states') not in ({'focusable': True}, {'focusable': False}):
                raise AssertionError('image and glyph share their button role')
            return [{'rect': [1200 if query['states']['focusable'] else 1300, 400, 40, 40]}]
        if query.get('role') == 'menu item':
            return [{'name': n} for n in ({'Capture actions', 'Check for updates', 'About put.gd', 'Quit put.gd'} if self.menu == 'app' else {'Minimize', 'Maximize', 'Close'} if self.menu == 'header' else {'Copy details'} if self.menu == 'row' else set())]
        if query.get('name') == 'Capture actions':
            return [{'name': 'Capture actions'}] if self.help else []
        if query.get('role') == 'page tab':
            return [{'name': query['name'], 'states': {'selected': query['name'] == 'History'}}]
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

    def shell(self, script, timeout=60, cleanup=False):
        import shlex
        args = shlex.split(script)
        self.assert_launcher_args(args)
        self.events.append(('shell', script))
        if args[1] in ('--history', '--activity'):
            self.closed = self.minimized = False
        value = json.dumps(self.build) if args[1] == '--version-json' else 'putgd 26.1029.42-nightly\n' if args[1] == '--version' else ''
        return {'returncode': 0, 'stdout': value, 'stderr': ''}

    def assert_launcher_args(self, args):
        if len(args) != 2 or args[0] != 'putgd' or args[1] not in ('--version', '--version-json', '--history', '--activity'):
            raise AssertionError('nonliteral launcher command')

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
# This widget simulator models the original History interactions. The shell
# composites use independently tested admission/cleanup oracles below and real
# three-desktop receipts, rather than simulating another compositor/lifecycle.
SIMULATED = [r for r in LANE if r.function.__code__.co_firstlineno < history.autosave_status.__code__.co_firstlineno]


class HistoryRecipeTests(unittest.TestCase):
    def simulate(self, item, ctx):
        if item.function == history.header_drag:
            original = copy.deepcopy(ctx.window)
            with self.assertRaises(RecipeBlocked) as error:
                item.function(ctx)
            self.assertEqual(error.exception.issue, 904)
            self.assertEqual(ctx.window, original)
            self.assertEqual(len([e for e in ctx.events if e[0] == 'drag']), 2)
        else:
            item.function(ctx)

    def test_exactly_the_owned_71_ids_once(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        expected = {key for key, owner in owners.items() if owner == 'history'}
        actual = [key for item in LANE for key in item.interactions]
        self.assertEqual(len(expected), 71)
        self.assertEqual(len(actual), len(set(actual)))
        self.assertEqual(set(actual), expected)

    def test_every_executable_recipe_uses_input_inspection_and_reference_frames(self):
        for item in SIMULATED:
            if item.interactions[0] in history.BLOCKED:
                continue
            with self.subTest(recipe=item.name):
                ctx = FakeContext(item.needs)
                self.simulate(item, ctx)
                self.assertTrue(any(event[0] in {'click', 'drag', 'cli', 'mcp', 'shell'} for event in ctx.events))
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
                self.assertEqual(error.exception.issue, 820)
                self.assertGreater(len(error.exception.reason), 50)

    def test_references_name_existing_catalog_captures(self):
        source = (ROOT / 'tests/gui/ui_catalog.cpp').read_text()
        for item in SIMULATED:
            if item.interactions[0] in history.BLOCKED:
                continue
            ctx = FakeContext(item.needs)
            self.simulate(item, ctx)
            for _, _, reference in ctx.frames:
                if reference:
                    self.assertIn('"' + reference, source, reference)

    def test_row_menu_does_not_rely_on_home_reselecting_current_row(self):
        class CurrentButUnselected(FakeContext):
            def keys(self, *keys):
                if 'Home' in keys:
                    self.selected = []
                    return
                return super().keys(*keys)
        ctx = CurrentButUnselected(('history-row',))
        history.key_menu(ctx)
        self.assertIn(('keys', ('Shift+F10',)), ctx.events)
        self.assertIn(('keys', ('Menu',)), ctx.events)
        self.assertTrue(ctx.clipboard)

    def test_clear_filter_uses_reflowed_native_button_not_catalog_point(self):
        class Reflowed(FakeContext):
            def click(self, control, window='history', button='left'):
                if control == 'text:Clear filters':
                    raise AssertionError('static catalog target precedes active-filter toolbar reflow')
                return super().click(control, window, button)
        ctx = Reflowed(('history_rows',))
        history.clear_filters(ctx)
        self.assertEqual(ctx.query['uploadStatus'], '')
        self.assertEqual(ctx.query['search'], ctx.fixture_row['name'])
        self.assertIn(('click', 'historyClearFilters', 'history', 'left'), ctx.events)

    def test_window_inventory_waits_for_clean_read_after_departed_client(self):
        ctx = FakeContext()
        calls = []
        def windows(query):
            calls.append(query)
            if len(calls) == 1:
                raise RecipeBlocked('accessibility walk incomplete: Message recipient disconnected from message bus without replying')
            return []
        ctx.windows = windows
        self.assertEqual(history._main_windows(ctx), [])
        self.assertEqual(len(calls), 2)

    def test_refused_window_inventory_never_counts_as_absence(self):
        ctx = FakeContext()
        def windows(query):
            raise RecipeBlocked('accessibility walk incomplete: Message recipient disconnected from message bus without replying')
        ctx.windows = windows
        with self.assertRaises(RecipeBlocked):
            history._main_windows(ctx)

    def test_chrome_uses_owned_native_names_when_catalog_control_is_absent(self):
        class NoCatalogChrome(FakeContext):
            def click(self, control, window='history', button='left'):
                if control in ('windowMaximize', 'windowMinimize', 'windowClose'):
                    raise AssertionError('catalog chrome cannot be resolved in this native toolkit')
                return super().click(control, window, button)
        for function in (history.maximize, history.minimize, history.close_window):
            with self.subTest(function=function.__name__):
                function(NoCatalogChrome(('history-row',)))

    def test_chrome_setup_accepts_small_native_placement_adjustment(self):
        class Adjusted(FakeContext):
            def drag(self, start, end, button='left', steps=12):
                super().drag(start, end, button, steps)
                self.window['x'] += 2
                self.window['y'] += 2
        ctx = Adjusted()
        ctx.profile = {'history_window_anchor': {'x': 24, 'y': 72}, 'history_window_anchor_tolerance': 32}
        size = (ctx.window['width'], ctx.window['height'])
        history._window_buttons_ready(ctx)
        self.assertEqual((ctx.window['x'], ctx.window['y']), (26, 74))
        self.assertEqual((ctx.window['width'], ctx.window['height']), size)

    def test_edit_command_accepts_native_menu_role_without_losing_owner_bounds(self):
        ctx = FakeContext()
        queries, points = [], []
        def accessible(query):
            queries.append(query)
            if query.get('role'):
                return []
            return [{'name': 'Capture region edit', 'role': 'menu', 'rect': [700, 200, 120, 40]}]
        ctx.accessible = accessible
        ctx.hover = lambda x, y, **options: points.append((x, y))
        node = history._hover_history_node(ctx, history._command_control('edit'))
        self.assertEqual(node['role'], 'menu')
        self.assertEqual(points, [(760, 220)])
        self.assertTrue(all(q['owned'] and q['pid'] == 123 and q['within'] for q in queries))

    def test_clear_filter_dismisses_combo_grab_before_underlying_click(self):
        class PopupGrab(FakeContext):
            popup = False
            def keys(self, *keys):
                super().keys(*keys)
                if 'Return' in keys and self.focused_control == 'historyChoiceuploadStatus':
                    self.popup = True
                if 'Esc' in keys:
                    self.popup = False
            def click(self, control, window='history', button='left'):
                if control == 'historyClearFilters' and self.popup:
                    self.popup = False
                    return  # Native popup consumes the first outside click.
                return super().click(control, window, button)
        ctx = PopupGrab(('history_rows',))
        history.clear_filters(ctx)
        self.assertFalse(ctx.popup)
        self.assertFalse(ctx.query['uploadStatus'])
        self.assertEqual(ctx.query['search'], ctx.fixture_row['name'])

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

    def test_fixed_chrome_rows_run_on_all_three_desktops(self):
        from tools.lab.surface_driver.profiles import load_profile
        for desktop in ('mint-cinnamon-x11', 'ubuntu-gnome-wayland', 'kubuntu-kde-wayland'):
            self.assertNotIn('history_window_buttons_blocked', load_profile(desktop))

    def test_new_composites_use_available_core_fixtures(self):
        from tools.lab.surface_driver.fixtures import FACTORIES
        for entry in LANE:
            self.assertTrue(set(entry.needs) <= set(FACTORIES), entry.name)

    def test_dynamic_row_alias_is_restored_when_a_double_click_is_refused(self):
        class RowContext(FakeContext):
            def accessible(self, query):
                value = super().accessible(query)
                if query.get('role') == 'list item':
                    value[0]['name'] = 'only synthetic row'
                return value
            def double_click(self, control, window, button='left'):
                self_test.assertEqual(self.profile['live_controls'][control], {'name': 'only synthetic row', 'role': 'list item'})
                raise RecipeFailed('gesture refused by foreground guard')
        self_test = self
        ctx = RowContext()
        original = {'existing': {'name': 'Existing', 'role': 'push button'}}
        ctx.profile = {'live_controls': original}
        with self.assertRaisesRegex(RecipeFailed, 'foreground guard'):
            history._double_history_row(ctx)
        self.assertIs(ctx.profile['live_controls'], original)

    def test_new_operation_ignores_existing_and_other_actions(self):
        ctx = FakeContext()
        with patch.object(history, '_recent', return_value=[
                {'id': 'old', 'action': 'edit'}, {'id': 'other', 'action': 'clipboard'},
                {'id': 'new', 'action': 'edit'}]):
            self.assertEqual(history._new_operation(ctx, {'old'}, 'edit')['id'], 'new')

    def test_duplicate_operation_admission_fails(self):
        ctx = FakeContext()
        with patch.object(history, '_recent', return_value=[{'id': i, 'action': 'edit'} for i in ('a', 'b')]):
            with self.assertRaisesRegex(RecipeFailed, 'one operation'):
                history._new_operation(ctx, set(), 'edit')

    def test_selector_cleanup_runs_when_real_escape_does_not_cancel(self):
        ctx = FakeContext()
        with patch.object(history, '_recent', return_value=[]), patch.object(history, '_new_operation', return_value={'id': 'new', 'state': 'selecting'}), patch.object(history, '_settle', return_value={'state': 'failed'}):
            with self.assertRaisesRegex(RecipeFailed, 'real Escape'):
                history._exercise_command(ctx, 'region', lambda action: None)
        self.assertIn(('call', 'operation.cancel', {'id': 'new'}), ctx.events)

    def test_missing_local_artifact_cannot_pass_command(self):
        ctx = FakeContext()
        with patch.object(history, '_recent', return_value=[]), patch.object(history, '_new_operation', return_value={'id': 'new'}), patch.object(history, '_settle', return_value={'state': 'completed', 'items': []}):
            with self.assertRaisesRegex(RecipeFailed, 'local command completed'):
                history._exercise_command(ctx, 'desktop', lambda action: None)
        self.assertIn(('call', 'operation.cancel', {'id': 'new'}), ctx.events)

    def test_action_matrix_cannot_pass_after_one_success(self):
        ctx = FakeContext(('app-profile', 'selector-desktop'))
        attempted = []
        def exercise(ctx, action, dispatch):
            attempted.append(action)
            if action == 'desktop':
                raise RecipeFailed('fixture refuses third action')
        with patch.object(history, '_exercise_command', side_effect=exercise):
            with self.assertRaisesRegex(RecipeFailed, 'third action'):
                history._action_matrix(ctx, lambda action: None)
        self.assertEqual(attempted, ['clipboard', 'region', 'window', 'desktop'])

    def test_chrome_clearance_moves_the_live_window_without_changing_its_size(self):
        ctx = FakeContext()
        ctx.profile = {'history_window_anchor': {'x': 24, 'y': 72}}
        original = copy.deepcopy(ctx.window)
        history._window_buttons_ready(ctx)
        self.assertEqual((ctx.window['x'], ctx.window['y']), (24, 72))
        self.assertEqual((ctx.window['width'], ctx.window['height']), (original['width'], original['height']))
        self.assertEqual(len([e for e in ctx.events if e[0] == 'drag']), 1)
        history._window_buttons_ready(ctx)
        self.assertEqual(len([e for e in ctx.events if e[0] == 'drag']), 1)

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

    def test_launcher_flags_use_literal_commands_in_the_owned_shell(self):
        ctx = FakeContext(('launcher',), fault='launcher-routed-to-client')
        ctx.cli = lambda *a, **k: self.fail('launcher flag reached the method client')
        history.cli_version(ctx)
        self.assertIn(('shell', 'putgd --version'), ctx.events)
        self.assertIn(('shell', 'putgd --version-json'), ctx.events)

    def test_geometry_rows_use_supported_accessibility_filters_without_window_fixture(self):
        class NativeContext(FakeContext):
            def windows(self, query=None):
                raise AssertionError('geometry must not send X11 title queries')
            def accessible(self, query):
                if set(query) - {'owned', 'visible', 'showing', 'enabled', 'pid', 'within', 'name', 'states', 'role'}:
                    raise AssertionError('unsupported accessible header query')
                return super().accessible(query)
        for action in (history.header_drag, history.edge_resize):
            entry = next(r for r in LANE if r.function == action)
            self.assertEqual(entry.needs, ('history-row',))
            self.simulate(entry, NativeContext(entry.needs))

    def test_header_point_excludes_navigation_and_refuses_covered_header(self):
        ctx = FakeContext()
        point = history._header_point(ctx, ctx.window)
        self.assertFalse(550 <= point[0] <= 850)
        ctx.accessible = lambda query: [
            {'name': 'Application menu', 'role': 'push button', 'rect': [100, 80, 1400, 40]}]
        with self.assertRaisesRegex(RecipeBlocked, 'no verified blank header'):
            history._header_point(ctx, ctx.window)

    def test_app_child_input_refuses_ambiguous_or_unplaced_native_bounds(self):
        for nodes in ([], [{'rect': [500, 300, 20, 20]}] * 2,
                      [{'rect': [500, 300, 20, 20], 'coordinate_space': 'window-relative'}],
                      [{'rect': [0, 0, 20, 20]}]):
            with self.subTest(nodes=nodes):
                ctx = FakeContext()
                ctx.accessible = lambda query: nodes
                with self.assertRaises(RecipeBlocked):
                    history._click_history_node(ctx, {'name': 'Expand preview'})
                self.assertFalse(any(e[0] in ('click', 'focus') for e in ctx.events))

    def test_dialog_geometry_waits_for_mapping_before_frame_or_input(self):
        ctx = FakeContext()
        calls = []
        def geometry(window):
            calls.append(window)
            if len(calls) < 3:
                raise RecipeBlocked('geometry: RuntimeError: live window match is missing or ambiguous (0 matches)')
            return ctx.window
        ctx.geometry = geometry
        self.assertEqual(history._wait_window(ctx, 'historyConfirmAction'), ctx.window)
        self.assertEqual(calls, ['historyConfirmAction'] * 3)
        self.assertFalse(ctx.events)

    def test_window_wait_preserves_unavailable_native_probe_errors(self):
        ctx = FakeContext()
        def geometry(window):
            raise RecipeBlocked('geometry: RuntimeError: fresh accessibility probe timed out')
        ctx.geometry = geometry
        with self.assertRaisesRegex(RecipeBlocked, 'fresh accessibility probe timed out'):
            history._frame(ctx, 'before')
        self.assertFalse(ctx.frames)

    def test_cancel_all_targets_the_counted_button_without_matching_dialog_title(self):
        class CountedContext(FakeContext):
            def click(self, control, window, button='left', offset=None):
                if window == 'historyCancelAllConfirmation' and control.startswith('text-glob:Cancel'):
                    raise AssertionError('glob also matches the dialog title')
                return super().click(control, window, button, offset)
        history.cancel_all(CountedContext(('history_active_uploads',)))

    def test_confirmation_uses_button_role_and_modal_window_guard(self):
        ctx = FakeContext()
        queries = []
        def accessible(query):
            queries.append(query)
            return [{'rect': [600, 600, 100, 40]}] if query.get('role') == 'push button' else [{'rect': [600, 600, 100, 40]}] * 2
        points = []
        ctx.accessible = accessible
        ctx.click_at = lambda *point, **kwargs: points.append((point, kwargs))
        history._confirm_button(ctx, 'historyConfirmAction', 'Clear history records')
        self.assertEqual(queries[0]['role'], 'push button')
        self.assertEqual(queries[0]['pid'], 123)
        self.assertEqual(queries[0]['within'], [100, 80, 1400, 950])
        self.assertIn(('focus', 'historyConfirmAction', False), ctx.events)
        self.assertEqual(points, [((650, 620), {'head': 0})])

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
            for desktop in ('mint-cinnamon-x11', 'ubuntu-gnome-wayland', 'kubuntu-kde-wayland'):
                catalog = Catalog(directory, load_profile(desktop))
                for control, label in names.items():
                    self.assertIn('setAccessibleName("' + label + '")', source)
                    self.assertEqual(selector_matches({'name': label, 'objectName': ''}, control,
                                                      catalog.candidates(control, 'history')), 1)

    def test_single_row_click_uses_its_live_delegate_bounds(self):
        ctx = FakeContext(('history-row',))
        history._row(ctx)
        queries = [e[1] for e in ctx.events if e[0] == 'delegate']
        self.assertEqual(len(queries), 1)
        self.assertEqual(queries[0]['within'], [320, 200, 800, 650])
        self.assertEqual(queries[0]['pid'], 123)
        self.assertEqual(queries[0]['role'], 'list item')
        with self.assertRaisesRegex(RecipeFailed, 'fixture row is selected'):
            history._row(FakeContext(('history-row',), fault='row-click-no-effect'))

    def test_unverified_list_focus_never_receives_navigation_or_activation(self):
        ctx = FakeContext(('history-row',), fault='list-cannot-focus')
        with self.assertRaisesRegex(RecipeBlocked, '20 Tab steps'):
            history._row(ctx)
        keys = [e[1] for e in ctx.events if e[0] == 'keys']
        self.assertNotIn(('Home',), keys)
        self.assertEqual(keys.count(('Tab',)), 20)

    def test_local_save_retry_keeps_its_original_terminal_result(self):
        ctx = FakeContext(('history_retry_local',))
        history._retry(ctx, 'history_retry_local', local=True)
        self.assertEqual(ctx.operations['op-1']['state'], 'failed')
        self.assertEqual(ctx.local['status'], 'saved')

    def test_upload_retry_can_finish_partial_after_its_upload_succeeds(self):
        ctx = FakeContext(('history_retry_upload',), fault='upload-partial-result')
        history._retry(ctx, 'history_retry_upload')
        self.assertEqual(ctx.upload, 'uploaded')
        self.assertEqual(ctx.operations['op-1']['state'], 'partial')

    def test_popup_frames_do_not_require_a_native_menu_window(self):
        class PopupContext(FakeContext):
            def geometry(self, window):
                if window.endswith(('Menu', 'Popup')):
                    raise AssertionError('runtime menus resolve through their owner, never standalone geometry')
                return super().geometry(window)
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
                if self.chooser and self.input_window != 'historySaveDialog':
                    raise AssertionError('keyboard would target History behind the chooser')
                super().keys(*sequence)
        history.inspector_more(ChooserContext(('history-row', 'file-dialog-fallback')))

    def test_inspector_waits_for_native_chooser_mapping_before_focus(self):
        class MappingContext(FakeContext):
            chooser_geometry_calls = 0
            mapped = False
            def geometry(self, window):
                if window == 'historySaveDialog':
                    self.chooser_geometry_calls += 1
                    if self.chooser_geometry_calls == 1:
                        raise RecipeBlocked('geometry: RuntimeError: live window match is missing or ambiguous (0 matches)')
                    self.mapped = True
                return super().geometry(window)
            def focus(self, window, raise_window=True):
                if window == 'historySaveDialog' and not self.mapped:
                    raise AssertionError('chooser request is open but its native window is not mapped')
                return super().focus(window, raise_window)
        ctx = MappingContext(('history-row', 'file-dialog-fallback'))
        history.inspector_more(ctx)
        self.assertEqual(ctx.chooser_geometry_calls, 2)

    def test_history_popup_owners_are_scoped_on_both_guests(self):
        from tools.lab.surface_driver.profiles import load_profile
        for desktop in ('mint-cinnamon-x11', 'ubuntu-gnome-wayland', 'kubuntu-kde-wayland'):
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
