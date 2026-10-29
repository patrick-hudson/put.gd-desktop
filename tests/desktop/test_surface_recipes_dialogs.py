#!/usr/bin/env python3
"""Lightweight recipe contracts. These fakes are never desktop evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import dialogs


class FakeContext(Context):
    def __init__(self, backend):
        self.backend = backend
        self.profile = {'dialogs': {'backends': {
            'portal': {'available': True, 'location_keys': ['Ctrl+L'], 'folder_accept_keys': ['Alt+S']},
            'fallback': {'available': True, 'references': {'open': 'test-open', 'save': 'test-save', 'folder': 'test-folder'}},
        }}}
        self.document = {'archive': {'root': '/old', 'folders': dict.fromkeys(('images', 'videos', 'text', 'other'))}}
        self.package_folder = '/old-package'
        self.history_path = ''
        self.last_save = None
        self.saved_copy = {'path': '', 'revision': 1, 'current': False}
        self.operations = []
        self.dialog = {'open': False, 'last': None}
        self.owner = None
        self.frames = []
        self.inputs = []
        self.calls = []
        self.reviews = []
        self.typed = ''
        self.no_accessibility = False
        self.corrupt_choice = False
        self.cancel_mutates = False
        self.witness_fails = False
        self.fallback_during_prepare = False
        self.names_only_accessibility = False

    def fixture(self, name):
        if name.startswith('file-dialog-'):
            return {'available': True, 'reason': ''}
        return {'history': SimpleNamespace(history_id='history-1', path='/retained/original.png'),
                'recording': SimpleNamespace(id='record-1')}[name]

    def shell(self, script, timeout=60):
        self.calls.append(('shell', script))
        if 'tempfile.mkdtemp' in script:
            return {'returncode': 0, 'stdout': json.dumps({'root': '/tmp/pgfd.test', 'image': '/tmp/pgfd.test/Synthetic image.png', 'folder': '/tmp/pgfd.test/Chosen folder'})}
        return {'returncode': 1 if self.witness_fails else 0, 'stdout': ''}

    def view(self, method, params=None):
        self.calls.append(('view', method, params))
        return {}

    def call(self, method, params=None):
        params = params or {}
        self.calls.append(('call', method, params))
        if method in ('ui.fileDialog', 'editor.fileDialog'):
            return self._chooser(method, params)
        if method == 'settings.get':
            return {'document': copy.deepcopy(self.document)}
        if method == 'ui.history':
            return {'savedPath': self.history_path, 'copyJob': {'status': 'saved'}, 'busy': False}
        if method == 'ui.diagnostics':
            return {'package': {'folder': self.package_folder}}
        if method == 'editor.open':
            return {'operationId': 'edit-1'}
        if method == 'editor.inspect':
            return {'phase': 'editing', 'view': {'lastSave': copy.deepcopy(self.last_save)}}
        if method == 'editor.cancel':
            return {}
        if method == 'operation.recent':
            return {'operations': list(self.operations)}
        if method == 'recording.review':
            action = params['action']
            if action in ('saveCopyDialog', 'closeFileDialog'):
                self._chooser(method, {'action': 'open' if action == 'saveCopyDialog' else 'cancel', 'dialog': 'review'})
            if action == 'closeFileDialog' and not self.dialog['open'] and self.saved_copy['path']:
                raise AssertionError('Review controls cannot mutate during export')
            return {'fileDialog': copy.deepcopy(self.dialog), 'savedCopy': copy.deepcopy(self.saved_copy), 'edit': {'revision': 1}}
        raise AssertionError('Unexpected method: ' + method)

    def _chooser(self, method, params):
        action = params['action']
        if action == 'open':
            self.owner = params.get('dialog', 'editor')
            self.field = params.get('field', 'root')
            mode = 'open' if self.owner == 'editImage' else 'folder' if self.owner in ('outputFolder', 'diagnosticsFolder') else 'save'
            self.dialog = {'open': True, 'backend': self.backend, 'mode': mode, 'title': 'Synthetic chooser', 'last': self.dialog['last']}
            self.prepare_inspects = 0
        elif action == 'inspect' and self.dialog['open']:
            self.prepare_inspects += 1
            if self.fallback_during_prepare and self.prepare_inspects > 1:
                self.dialog.update(backend='fallback', fallbackReason='portal_failed')
        elif action == 'cancel' and self.dialog['open']:
            self._finish('cancelled')
        return copy.deepcopy(self.dialog)

    def _finish(self, status):
        self.dialog['open'] = False
        self.dialog['last'] = {'status': status, 'backend': self.backend, 'path': '/wrong' if self.corrupt_choice else self.typed}
        if status == 'cancelled':
            if self.cancel_mutates:
                self.document['archive']['root'] = '/incorrect'
            return
        if self.owner == 'outputFolder':
            if self.field == 'root':
                self.document['archive']['root'] = self.typed
            else:
                self.document['archive']['folders'][self.field] = self.typed
        elif self.owner == 'diagnosticsFolder':
            self.package_folder = self.typed
        elif self.owner == 'historySaveAs':
            self.history_path = self.typed
        elif self.owner == 'editor':
            self.last_save = {'status': 'saved', 'path': self.typed}
        elif self.owner == 'review':
            self.saved_copy['path'] = self.typed
        elif self.owner == 'editImage':
            self.operations.append({'id': 'new-edit', 'action': 'edit'})

    def mcp(self, tool, arguments=None):
        self.calls.append(('mcp', tool, arguments))
        assert tool == 'drive_file_dialog'
        return self._chooser('ui.fileDialog', arguments)

    def cli(self, *args, timeout=60):
        return {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': self.call(args[0], json.loads(args[1]))})}

    def wait(self, predicate, what, timeout=10, interval=.2):
        for _ in range(3):
            result = predicate()
            if result:
                return result
        raise RecipeFailed(what)

    def geometry(self, window):
        self.calls.append(('geometry', window))
        return {'x': 0, 'y': 0, 'width': 820, 'height': 520, 'scale': 1}

    def accessible(self, query):
        self.calls.append(('accessible', query))
        if self.names_only_accessibility:
            return [{'name': 'Synthetic chooser', 'role': 'dialog'}]
        return [] if self.no_accessibility else [{'name': query.get('name'), 'text': query.get('text')}]

    def keys(self, *sequence):
        self.inputs.append(('keys', sequence))
        if self.dialog['open']:
            if 'Escape' in sequence:
                self._finish('cancelled')
            elif 'Return' in sequence:
                self._finish('chosen')

    def type_text(self, text):
        self.inputs.append(('type', text))
        self.typed = text

    def click(self, control, window, button='left', offset=None):
        self.inputs.append(('click', control, window))

    def frame(self, label, window=None, reference=None):
        self.frames.append({'label': label, 'window': window, 'reference': reference})

    def note(self, text):
        pass

    def review(self, reason):
        self.reviews.append(reason)


class DialogRecipes(unittest.TestCase):
    def test_owned_ids_have_two_named_variants(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        registered = [r for r in REGISTRY if r.function.__module__ == dialogs.__name__]
        self.assertEqual(len(registered), 22)
        self.assertEqual(len({r.name for r in registered}), 22)
        for interaction in (key for key, lane in owners.items() if lane == 'dialogs'):
            rows = [r for r in registered if interaction in r.interactions]
            self.assertEqual(len(rows), 2, interaction)
            self.assertEqual({r.desktop_drawn for r in rows}, {True, False})
            self.assertTrue(all(r.destinations == ('local',) for r in rows))
        self.assertEqual({i for r in registered for i in r.interactions}, {key for key, lane in owners.items() if lane == 'dialogs'})

    def test_every_scenario_on_both_backends(self):
        for scenario in dialogs.SCENARIOS:
            for backend in dialogs.BACKENDS:
                with self.subTest(scenario=scenario, backend=backend):
                    ctx = FakeContext(backend)
                    blocked = scenario == 'failure-strip' or scenario in dialogs.BACKENDS and scenario != backend
                    if blocked:
                        with self.assertRaises(RecipeBlocked):
                            dialogs._run(ctx, backend, scenario)
                        continue
                    dialogs._run(ctx, backend, scenario)
                    self.assertFalse(ctx.dialog['open'])
                    self.assertIn(('keys', ('Escape',)), ctx.inputs)
                    if scenario != 'cancel':
                        self.assertTrue(any(i[0] == 'type' for i in ctx.inputs))
                    self.assertTrue(any(f['label'].startswith('after') for f in ctx.frames))
                    if backend == 'portal':
                        self.assertTrue(any(c[0] == 'accessible' for c in ctx.calls))
                        self.assertFalse(any(f['reference'] for f in ctx.frames))
                        self.assertFalse(any(i[0] == 'click' for i in ctx.inputs))
                    else:
                        self.assertTrue(any(f['reference'] for f in ctx.frames))
                        self.assertTrue(all(f['window'] == 'putgdFileDialog' for f in ctx.frames if f['reference']))

    def test_cancel_mutation_fails(self):
        ctx = FakeContext('fallback')
        ctx.cancel_mutates = True
        with self.assertRaisesRegex(RecipeFailed, 'cancel preserves'):
            dialogs.cancel_fallback(ctx)

    def test_wrong_path_fails_and_cleans_up(self):
        ctx = FakeContext('portal')
        ctx.corrupt_choice = True
        with self.assertRaisesRegex(RecipeFailed, 'exact typed path'):
            dialogs.output_folder_portal(ctx)
        self.assertFalse(ctx.dialog['open'])

    def test_silent_portal_fallback_is_blocked(self):
        ctx = FakeContext('fallback')
        with self.assertRaisesRegex(RecipeBlocked, 'Requested portal, got fallback'):
            dialogs.output_folder_portal(ctx)
        self.assertFalse(ctx.dialog['open'])

    def test_missing_accessibility_fails_and_cancels(self):
        ctx = FakeContext('portal')
        ctx.no_accessibility = True
        with self.assertRaisesRegex(RecipeFailed, 'accessible and drawn'):
            dialogs.output_folder_portal(ctx)
        self.assertFalse(ctx.dialog['open'])

    def test_portal_fallback_while_waiting_for_draw_is_blocked(self):
        ctx = FakeContext('portal')
        ctx.fallback_during_prepare = True
        with self.assertRaisesRegex(RecipeBlocked, 'Portal changed backend'):
            dialogs.output_folder_portal(ctx)
        self.assertFalse(ctx.dialog['open'])

    def test_probe_ignoring_text_query_cannot_false_pass(self):
        ctx = FakeContext('portal')
        ctx.names_only_accessibility = True
        with self.assertRaisesRegex(RecipeBlocked, 'exposes names only') as blocked:
            dialogs.output_folder_portal(ctx)
        self.assertEqual(blocked.exception.issue, 671)
        self.assertFalse(ctx.dialog['open'])

    def test_independent_file_witness_is_required(self):
        ctx = FakeContext('fallback')
        ctx.witness_fails = True
        with self.assertRaisesRegex(RecipeFailed, 'file witness'):
            dialogs.history_save_as_fallback(ctx)

    def test_mcp_drives_open_inspect_cancel_and_real_input(self):
        ctx = FakeContext('portal')
        dialogs.mcp_file_dialogs_portal(ctx)
        self.assertEqual({c[2]['action'] for c in ctx.calls if c[0] == 'mcp'}, {'open', 'inspect', 'cancel'})
        self.assertTrue(any(i[0] == 'type' for i in ctx.inputs))

    def test_missing_catalog_marks_review(self):
        ctx = FakeContext('fallback')
        ctx.profile['dialogs']['backends']['fallback']['references'] = {}
        dialogs.cancel_fallback(ctx)
        self.assertTrue(ctx.reviews)

    def test_profiles_cover_model_desktops(self):
        model = json.loads((ROOT / 'tools/lab/surface_map/model.json').read_text())
        for desktop in model['desktops']:
            profile = ROOT / 'tools/lab/surface_driver/profiles' / (desktop['id'] + '.d') / 'dialogs.json'
            settings = json.loads(profile.read_text())['dialogs']['backends']
            self.assertEqual(settings['fallback']['environment'], {'PUTGD_FILE_DIALOG': 'fallback'})
            self.assertIn(settings['portal']['chooser'], ('gtk', 'gnome', 'kde'))


if __name__ == '__main__':
    unittest.main()
