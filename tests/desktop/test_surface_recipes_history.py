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

from tools.lab.surface_driver.api import Context, Frame, RecipeBlocked, RecipeFailed
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
        # The expanded receipt starts below the details pane's visible end.
        self.receipt_top = 900
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
        # #1396: in the wide list the pane follows the selection only while
        # details are wanted; closeDetails remembers "closed" there.
        self.details_wanted = True
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
            total = rows = len(self.matching(self.query['search']))
        return dict(busy=False, query=copy.deepcopy(self.query), rows=rows, total=total,
                    selectedIds=list(self.selected), selectedRows=len(self.selected), gridMode=self.grid,
                    activeFilters=['Upload state: Uploaded'] if self.query['uploadStatus'] and self.query['uploadStatus'] != 'failed' else [],
                    detailsWanted=self.details_wanted, detailsOpen=self.details, inspectorVisible=self.pane(),
                    canExpandPreview=self.pane(),
                    previewExpanded=self.preview, selectionSummary={'bytes': 3 * 1024 * 1024},
                    activeOperationCount=self.active, cancelAllConfirmation=copy.deepcopy(self.confirmation),
                    savedCopyVisible=self.saved, savedPath='/tmp/private/copy.png', savedCopyFolder=self.folder)

    def pane(self):
        return bool(self.selected) and (self.details if self.grid else self.details_wanted)

    def matching(self, text):
        # The resident's search is a case-insensitive substring of the name;
        # a second, similarly named record shares every short prefix.
        names = {'image-1': self.fixture_row['name'], 'image-2': 'surface-history-image-2.png'}
        return [{'id': i, 'name': n} for i, n in names.items() if text.lower() in n.lower()]

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
            if self.grid:
                self.details = False
            else:
                self.details_wanted = False
        elif action == 'toggleDetails':
            if self.selected and self.grid:
                self.details = not self.details
            else:
                self.details_wanted = not self.details_wanted
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
        if method == 'history.list':
            if set(params) != {'search', 'limit'}:
                raise AssertionError('unexpected history.list query')
            return {'records': self.matching(params['search'])[:params['limit']]}
        if method == 'delete.prepare':
            return {'targets': [{'historyId': 'image-1'}]}
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
            'text:More selection actions': 'historySelectionMore', 'text:Advanced original receipt details': 'historyDetails',
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
            if self.fault == 'clear-drops-search':
                self.query['search'] = ''
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
            'historySearch', 'historyCategories', 'historyFiltersExpanded',
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
        if query.get('name') in ('Advanced original receipt details', 'Delete from destination…',
                                 'Show advanced receipt details') and 'within' in query:
            # As the probe does: a node matches 'within' only by its centre.
            x, y, w, h = query['within']
            return [n for n in self.accessible({k: v for k, v in query.items() if k != 'within'})
                    if n.get('rect') and x <= n['rect'][0] + n['rect'][2] / 2 < x + w
                    and y <= n['rect'][1] + n['rect'][3] / 2 < y + h]
        if query.get('name') == 'Advanced original receipt details' and self.receipt:
            return [{'name': query['name'], 'rect': [1250, self.receipt_top, 340, 180]}]
        if query.get('name') == 'Delete from destination…' and query.get('role') == 'push button':
            # The pane's last control; scrolling to the end leaves it in place.
            return [{'name': query['name'], 'rect': [1250, max(self.receipt_top, 640) + 200, 200, 36]}]
        if query.get('name') == 'Show advanced receipt details':
            return [{'name': query['name'], 'rect': [1250, self.receipt_top - 40, 160, 30]}]
        if query.get('name') == 'Expand preview':
            if query.get('states') not in ({'focusable': True}, {'focusable': False}):
                raise AssertionError('image and glyph share their button role')
            return [{'rect': [1200 if query['states']['focusable'] else 1300, 400, 40, 40]}] if self.pane() else []
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
        if query['name'].startswith('Total'):
            # The multi-selection summary lives in the details pane.
            return [{'name': 'Total size: 3 MB'}] if self.pane() and len(self.selected) > 1 else []
        return [{'name': 'Size: 1 MB · 640×320'}]

    def scroll(self, x, y, clicks, head=None):
        self.events.append(('scroll', x, y, clicks))
        # The pane's scroll range ends with the receipt inside it.
        self.receipt_top += 120 * clicks
        if self.fault != 'receipt-keeps-moving':
            self.receipt_top = max(self.receipt_top, 640)

    def clipboard_text(self, *, expected_json=None):
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


class HistoryRecipeTests(unittest.TestCase):
    def test_refused_window_inventory_never_counts_as_absence(self):
        ctx = FakeContext()
        def windows(query):
            raise RecipeBlocked('accessibility walk incomplete: Message recipient disconnected from message bus without replying')
        ctx.windows = windows
        with self.assertRaises(RecipeBlocked):
            history._main_windows(ctx)

    def test_summary_absent_without_details_pane_fails(self):
        ctx = FakeContext(('history_pair',))
        with patch.object(history, '_open', lambda c: setattr(c, 'details_wanted', False)):
            with self.assertRaisesRegex(RecipeFailed, 'visible binary size summary'):
                history.multi_select(ctx)

    def test_search_grows_past_a_shared_tail(self):
        def matching(other):
            return lambda text: [{'id': i, 'name': n} for i, n in (
                ('image-1', 'surface-history-image.png'), ('image-2', other)) if text in n]
        ctx = FakeContext(('history-row',))
        ctx.matching = matching('the-history-image.png')
        self.assertEqual(history._distinct_text(ctx, ctx.fixture_row), 'ce-history-image.png')
        # No part of the name finds this row alone: never search for a guess.
        ctx.matching = matching('copy-of-surface-history-image.png')
        with self.assertRaisesRegex(RecipeFailed, 'exactly that row'):
            history._distinct_text(ctx, ctx.fixture_row)

    def test_clear_filters_dropping_search_fails(self):
        with self.assertRaisesRegex(RecipeFailed, 'preserves search'):
            history.clear_filters(FakeContext(('history_rows',), fault='clear-drops-search'))

    def test_receipt_that_never_settles_blocks(self):
        ctx = FakeContext(('history-row',), fault='receipt-keeps-moving')
        with self.assertRaises(RecipeBlocked):
            history.receipt_details(ctx)
        self.assertNotIn(('click', 'historyDetails'), [e[:2] for e in ctx.events])

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

    def test_missing_local_artifact_cannot_pass_command(self):
        ctx = FakeContext()
        with patch.object(history, '_recent', return_value=[]), patch.object(history, '_new_operation', return_value={'id': 'new'}), patch.object(history, '_settle', return_value={'state': 'completed', 'items': []}):
            with self.assertRaisesRegex(RecipeFailed, 'local command completed'):
                history._exercise_command(ctx, 'desktop', lambda action: None)
        self.assertIn(('call', 'operation.cancel', {'id': 'new'}), ctx.events)

    def test_failed_envelope_is_never_treated_as_result(self):
        with self.assertRaisesRegex(AssertionError, 'fixture_missing'):
            history._result({'ok': False, 'error': {'code': 'fixture_missing'}, 'result': {}})


if __name__ == '__main__':
    unittest.main()
