#!/usr/bin/env python3
"""Contract tests only: fake Context is deliberately not desktop evidence."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import tray


class FakeContext(Context):
    desktop = 'mint-cinnamon-x11'
    host = 'boat'
    placement = 'boat'
    destination = 'local'
    package = 'deb'

    def __init__(self):
        self.profile = json.loads((ROOT / 'tools/lab/surface_driver/profiles/mint-cinnamon-x11.d/tray.json').read_text())
        # Exercise recipe logic against a capable fake, independently of the
        # real Cinnamon preflight blocker. This is never desktop evidence.
        self.profile['tray'].pop('preflight_blocked', None)
        self.profile['tray']['icon'] = {'name': 'put.gd', 'application': 'cinnamon'}
        self.events = []
        self.rects = {}
        self.about_open = False
        self.menu = False
        self.output = False
        self.edit_sources = False
        self.fixture_open = False
        self.operations = []
        self.notices = []
        self.clipboard = ''
        self.document = {'actions': {}, 'preferences': {'notifications': True}, 'archive': {}}
        self.original = copy.deepcopy(self.document)
        self.target = 'Settings'
        self.obstruction = False
        self.latch = False
        self.readonly = False
        self.shortcut_available = True
        self.broken_save = False
        self.ignored_input = False

    def fixture(self, name, **options):
        from tools.lab.surface_driver.fixtures.base import Fixture
        self.events.append(('fixture', name))
        if name == 'app-profile':
            return Fixture(root='/tmp/tray-fake', output_dir='/tmp/tray-fake/out')
        if name == 'selector-desktop':
            return Fixture(synthetic=True)
        if name == 'events.clipboard':
            self.clipboard = 'putgd synthetic clipboard text'
            return Fixture(text=self.clipboard)
        if name == 'tray-proton-busy':
            raise RecipeBlocked('proton_account_busy fixture unavailable', 670)
        raise AssertionError(name)

    def click_accessible(self, query, button='left'):
        assert any(k in query for k in ('pid', 'application', 'owned'))
        nodes = self.accessible(query)
        assert len(nodes) == 1
        self.click_at(*tray._point(self, nodes[0]), button=button)
        return nodes[0]

    def focus(self, window, raise_window=True):
        self.events.append(('focus', window))

    def view(self, method, params=None):
        self.events.append(('view', method, params))
        assert method == 'ui.show'
        self.target = params['view'].title()
        return {'shown': params['view']}

    def call(self, method, params=None):
        self.events.append(('call', method, params))
        if method == 'ui.snapshot':
            return {'operationsReady': True, 'settings': {'document': copy.deepcopy(self.document)},
                    'diagnostics': {'trayAvailable': True, 'activeOperations': 0,
                        'shortcuts': {'available': self.shortcut_available,
                                      'bindings': {a: {'bound': True} for a in tray.CHORDS}}}}
        if method == 'settings.get':
            return {'document': copy.deepcopy(self.document), 'metadata': {'acceptedRevision': 'r1'}}
        if method == 'settings.patch':
            assert params['expectedRevision'] == 'r1'
            if 'captureDefaults' in params['patch']:
                required = json.loads((ROOT / 'schemas/config.schema.json').read_text())['properties']['captureDefaults']['required']
                assert set(required) <= params['patch']['captureDefaults'].keys()
            for key, value in params['patch'].items():
                self.document.setdefault(key, {}).update(copy.deepcopy(value))
            return {'acceptedRevision': 'r2'}
        if method == 'settings.save':
            assert params['expectedRevision'] == 'r1'
            self.document = copy.deepcopy(params['document'])
            return {'metadata': {'acceptedRevision': 'r3'}}
        if method == 'operation.recent':
            return {'operations': copy.deepcopy(self.operations)}
        if method == 'operation.inspect':
            return copy.deepcopy(next(o for o in self.operations if o['id'] == params['id']))
        if method == 'operation.retryLocalSave':
            op = next(o for o in self.operations if o['id'] == params['id'])
            op['state'] = 'completed'
            op['items'][0]['local']['status'] = 'saved'
            return {'retryAccepted': True}
        if method == 'ui.history':
            return {'rows': [], 'loaded': True}
        if method == 'ui.viewState':
            return {'settings': {'section': 1}}
        if method == 'ui.fileDialog':
            return {'open': self.target == 'file-dialog'}
        if method == 'diagnostics':
            return {'capabilities': {'source': {}}}
        if method == 'editor.inspect':
            return {'phase': 'editing', 'width': 96, 'height': 64}
        if method == 'editor.cancel':
            self.operations[-1]['state'] = 'cancelled'
            return {}
        raise AssertionError('Unexpected resident method: ' + method)

    def wait(self, predicate, what, timeout=10, interval=.2):
        value = predicate()
        if not value:
            raise RecipeFailed(what)
        return value

    def accessible(self, query):
        name = query['name']
        names = {'put.gd', self.target, 'History', 'Settings', 'Destinations'}
        if self.fixture_open:
            names.add(tray.FIXTURE_TITLE)
        if self.menu:
            names.update(tray.LABELS.values())
            names.update(('History', 'Settings', 'Destinations', 'About put.gd', 'Quit put.gd',
                          'Change output…', 'Local only', 'Edit image'))
            if self.latch:
                names.add('Local save needs attention')
        if self.output:
            names.update(label + ': Local only' for label in tray.LABELS.values())
        if self.edit_sources:
            names.update(('Edit clipboard image', 'Edit an image file…'))
        for n in self.notices:
            names.update(n.get('buttons', ()))
        if name not in names:
            return []
        # A different rectangle per label ensures that tests track the node
        # which input actually targets, rather than accepting any click.
        if name not in self.rects:
            index = len(self.rects)
            self.rects[name] = (40 + 100 * (index % 8), 50 + 40 * (index // 8), 20, 10)
        rx, ry, rw, rh = self.rects[name]
        return [{'name': name, 'role': query.get('role', 'label'),
                 'rect': {'x': rx, 'y': ry, 'width': rw, 'height': rh},
                 'pid': 4242, 'application': 'cinnamon', 'showing': True,
                 'enabled': not name.endswith(': Local only'), 'visible': True,
                 'selected': name == self.target}]

    def click_at(self, x, y, button='left'):
        matches = [name for name, (rx, ry, rw, rh) in self.rects.items()
                   if rx <= x < rx + rw and ry <= y < ry + rh]
        assert len(matches) == 1, 'click must hit one observed rectangle'
        name = matches[0]
        self.events.append(('click', name, button))
        if self.ignored_input:
            return
        if name == 'put.gd':
            if button == 'right':
                self.menu = True
            elif button == 'left':
                self.target = 'History'
        elif name == 'Local only':
            self.output = True
        elif name == 'Edit image':
            self.edit_sources = True
        elif name == 'Edit an image file…':
            self.target = 'file-dialog'
            self.menu = False
        elif name == 'Edit clipboard image':
            self._invoke('edit', editing=True)
        elif name in tray.LABELS.values():
            self._invoke(next(a for a, label in tray.LABELS.items() if label == name))
        elif name == 'Local save needs attention':
            self.latch = False
            self.menu = False
            self.target = 'Capture'
        elif name == 'Save diagnostics for this upload':
            for n in self.notices:
                n['buttons'] = []
            if self.readonly:
                self.notices.append({'id': 99, 'summary': 'put.gd — diagnostics not saved'})
            else:
                self.notices.append({'id': 99, 'summary': 'put.gd — diagnostics saved',
                    'body': 'bundle.zip. Nothing was uploaded.', 'actions': ['show-in-folder'],
                    'buttons': ['Show in folder']})
        elif name == 'Show in folder':
            self.target = 'putgd-diagnostics-20261029-000000Z.zip'
        elif name != tray.FIXTURE_TITLE:
            if name == 'About put.gd':
                self.about_open = True
            self.target = {'Change output…': 'Capture'}.get(name, name)
            self.menu = False

    def _invoke(self, action, editing=False):
        self.menu = False
        pending = action in ('region', 'edit', 'record')
        failed = self.obstruction or self.broken_save
        self.operations.append({'id': 'op-' + str(len(self.operations)), 'action': action,
            'state': 'editing' if editing else 'acquiring' if pending else 'failed' if failed else 'completed',
            'items': [] if pending else [{'artifact': {'id': 'artifact-1'}, 'uploadStatus': 'local_only',
                                          'local': {'status': 'failed' if failed else 'saved'}}]})
        self.latch = failed
        if not pending and self.document['preferences']['notifications']:
            self.notices.append({'id': len(self.notices) + 1, 'summary': 'Local save failed' if failed else 'Saved locally',
                'body': 'Local save failed.' if failed else 'Saved locally.', 'actions': ['save-diagnostics'] if failed else [],
                'buttons': ['Save diagnostics for this upload'] if failed else []})

    def keys(self, *sequence):
        self.events.append(('keys', sequence))
        if self.ignored_input:
            return
        for key in sequence:
            if key == 'Ctrl+Alt+Shift+T':
                self.clipboard = 'putgd synthetic clipboard text'
            elif key in tray.CHORDS.values():
                self._invoke(next(a for a, chord in tray.CHORDS.items() if key == chord))
            elif key == 'Esc':
                self.about_open = False
                self.menu = self.output = self.edit_sources = False
                if self.operations and self.operations[-1]['state'] in ('acquiring', 'editing'):
                    self.operations[-1]['state'] = 'cancelled'

    def shell(self, script, timeout=60):
        self.events.append(('shell', script))
        output = ''
        if 'mktemp -d' in script:
            output = '/tmp/pgtray.fake'
        elif 'printf synthetic' in script:
            self.obstruction = True
        elif 'putgd-desktop-fixture --output' in script:
            self.fixture_open = True
            output = '4242'
        elif script.startswith('kill --'):
            assert script == 'kill -- 4242\n'
            self.fixture_open = False
        elif script.startswith('rm --'):
            self.obstruction = False
        elif 'mkdir -m 500' in script:
            self.readonly = True
        elif 'zipfile' in script:
            output = 'putgd-diagnostics-20261029-000000Z.zip'
        return {'returncode': 0, 'stdout': output, 'stderr': ''}

    def frame(self, label, window=None, reference=None):
        self.events.append(('frame', label, reference))

    def clipboard_text(self):
        return self.clipboard

    def notifications(self):
        return copy.deepcopy(self.notices)

    def scroll(self, x, y, clicks):
        self.events.append(('scroll', clicks))

    def note(self, message):
        self.events.append(('note', message))

    def review(self, message):
        self.events.append(('review', message))


class TrayRecipesTest(unittest.TestCase):
    def recipes(self):
        return [r for r in REGISTRY if r.function.__module__ == tray.__name__]

    def test_exact_ownership_and_no_missing_or_duplicate_ids(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        actual = [i for r in self.recipes() for i in r.interactions]
        self.assertEqual(set(actual), {i for i, owner in owners.items() if owner == 'tray'})
        self.assertEqual(len(actual), len(set(actual)))

    def test_executable_menu_recipes_use_owned_real_input(self):
        for function in (tray.context_menu, tray.activate, tray.tray_menu_about,
                         tray.tray_menu_settings, tray.tray_menu_destinations,
                         tray.tray_menu_upload_clipboard, tray.shortcut_clipboard):
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                function(ctx)
                self.assertTrue(any(e[0] in ('click', 'keys') for e in ctx.events))
                self.assertFalse(any(e[0] == 'call' and e[1] == 'action.invoke' for e in ctx.events))
                self.assertFalse(any(e[0] == 'shell' and 'kill --' in e[1] for e in ctx.events))

    def test_explicit_blocked_rows_have_issue(self):
        for interaction, reason in tray.BLOCKED.items():
            with self.subTest(interaction=interaction):
                r = next(r for r in self.recipes() if interaction in r.interactions)
                ctx = FakeContext()
                with self.assertRaises(RecipeBlocked) as caught:
                    r.function(ctx)
                self.assertEqual(caught.exception.issue, 670)

    def test_notification_actions_block_before_input(self):
        for function in (tray.save_diagnostics, tray.show_in_folder,
                         tray.resident_close, tray.diagnostics_not_saved):
            ctx = FakeContext()
            with self.assertRaises(RecipeBlocked) as caught:
                function(ctx)
            self.assertEqual(caught.exception.issue, 670)
            self.assertFalse(ctx.events)

    def test_shortcuts_read_native_diagnostics_from_the_ui_snapshot(self):
        ctx = FakeContext()
        tray.shortcut_clipboard(ctx)
        self.assertFalse(any(e[0] == 'call' and e[1] == 'diagnostics' for e in ctx.events))
        self.assertIn(('keys', (tray.CHORDS['clipboard'],)), ctx.events)

    def test_missing_shortcut_channel_blocks_without_pressing_the_shortcut(self):
        ctx = FakeContext()
        ctx.shortcut_available = False
        with self.assertRaises(RecipeBlocked):
            tray.shortcut_region(ctx)
        self.assertFalse(any(e[0] == 'keys' and 'Ctrl+Shift+1' in e[1] for e in ctx.events))

    def test_ignored_input_cannot_pass_clipboard_recipe(self):
        ctx = FakeContext()
        ctx.ignored_input = True
        with self.assertRaises(RecipeFailed):
            tray.tray_menu_upload_clipboard(ctx)
        self.assertFalse(ctx.fixture_open)

    def test_ignored_tray_activation_cannot_pass_using_a_hidden_history_inspector(self):
        ctx = FakeContext()
        ctx.ignored_input = True
        with self.assertRaises(RecipeFailed):
            tray.activate(ctx)

    def test_missing_selected_state_cannot_pass_navigation(self):
        ctx = FakeContext()
        original = ctx.accessible
        def without_selection(query):
            nodes = original(query)
            for node in nodes:
                node.pop('selected', None)
            return nodes
        ctx.accessible = without_selection
        with self.assertRaises(RecipeBlocked):
            tray.activate(ctx)

    def test_busy_latch_is_explicitly_blocked_after_local_branch(self):
        ctx = FakeContext()
        with self.assertRaisesRegex(RecipeBlocked, 'proton_account_busy'):
            tray.attention_with_busy_latch(ctx)

    def test_ambiguous_accessibility_target_fails_before_click(self):
        ctx = FakeContext()
        ctx.accessible = lambda query: [{'name': query['name'], 'rect': [1, 2, 3, 4]}] * 2
        with self.assertRaises(RecipeBlocked):
            tray.activate(ctx)
        self.assertFalse(any(e[0] == 'click' for e in ctx.events))

    def test_catalog_reference_is_forwarded_only_when_configured(self):
        ctx = FakeContext()
        ctx.profile['tray']['references'] = {'full': '00-tray-menu'}
        tray.context_menu(ctx)
        self.assertIn(('frame', 'after', None), ctx.events)
        self.assertIn(('note', 'desktop catalog reference=00-tray-menu'), ctx.events)

    def test_hidden_notification_candidates_are_not_pointer_targets(self):
        ctx = FakeContext()
        ctx.accessible = lambda q: [
            {'name': q['name'], 'rect': [0, 0, 0, 0]},
            {'name': q['name'], 'rect': [-2147483648, -2147483648, 50, 20]},
            {'name': q['name'], 'rect': [20, 30, 50, 20]},
        ]
        self.assertEqual(tray._node(ctx, 'Show in folder')['rect'], [20, 30, 50, 20])

    def test_multiple_visible_notification_buttons_block_instead_of_clicking_an_old_operation(self):
        ctx = FakeContext()
        ctx.accessible = lambda q: [{'name': q['name'], 'rect': [20, 30, 50, 20]}] * 2
        with self.assertRaises(RecipeBlocked):
            tray._press(ctx, 'Save diagnostics for this upload')
        self.assertFalse(any(e[0] == 'click' for e in ctx.events))

    def test_icon_selector_is_scoped_to_the_shell(self):
        ctx = FakeContext()
        ctx.profile['tray']['icon'] = None
        with self.assertRaises(RecipeBlocked) as caught:
            tray.context_menu(ctx)
        self.assertEqual(caught.exception.issue, 670)
        self.assertFalse(any(e[0] == 'click' for e in ctx.events))

    def test_fixture_cleanup_is_delegated_to_core(self):
        ctx = FakeContext()
        tray.tray_menu_upload_clipboard(ctx)
        self.assertIn(('fixture', 'app-profile'), ctx.events)
        self.assertIn(('fixture', 'events.clipboard'), ctx.events)
        self.assertFalse(any(e[0] == 'shell' and 'mktemp' in e[1] for e in ctx.events))

    def test_window_capture_focuses_an_independent_synthetic_target(self):
        ctx = FakeContext()
        tray.shortcut_window(ctx)
        source = ctx.events.index(('fixture', 'selector-desktop'))
        focus = ctx.events.index(('focus', 'trayFixture'))
        input_at = ctx.events.index(('keys', (tray.CHORDS['window'],)))
        self.assertLess(source, focus)
        self.assertLess(focus, input_at)

    def test_desktop_node_without_owner_never_receives_a_click(self):
        ctx = FakeContext()
        original = ctx.accessible
        def unowned(query):
            rows = original(query)
            for row in rows:
                row.pop('pid', None)
                row.pop('application', None)
            return rows
        ctx.accessible = unowned
        with self.assertRaises(RecipeBlocked) as caught:
            tray._press(ctx, 'Settings')
        self.assertEqual(caught.exception.issue, 670)
        self.assertFalse(any(e[0] == 'click' for e in ctx.events))

    def test_notify_call_alone_does_not_prove_a_drawn_popup(self):
        ctx = FakeContext()
        ctx.profile['notifications'] = {'accessible': {'application': 'cinnamon*'}}
        ctx.accessible = lambda query: []
        with self.assertRaises(RecipeBlocked):
            tray._drawn_notice(ctx, {'summary': 'Upload complete'})

    def test_catalog_ids_match_published_action_identity_rules(self):
        import hashlib
        for label in ('About put.gd', 'Quit put.gd', 'Edit clipboard image', 'Edit an image file…'):
            identity = 'trayMenuAction-' + hashlib.sha256(label.encode()).hexdigest()[:16]
            self.assertEqual(tray.CONTROLS[identity], label)
        source = (ROOT / 'src/ui/TrayController.cpp').read_text()
        for identity in ('trayOutputSummary', 'trayChangeOutput', 'trayCancelUpload'):
            self.assertIn('setObjectName("' + identity + '")', source)

    def test_about_dialog_is_closed_after_its_evidence_frame(self):
        ctx = FakeContext()
        tray.tray_menu_about(ctx)
        self.assertFalse(ctx.about_open)
        self.assertEqual(ctx.events[-1], ('keys', ('Esc',)))

    def test_navigation_failure_closes_the_open_menu(self):
        ctx = FakeContext()
        def fail_frame(*args, **kwargs):
            raise RecipeFailed('synthetic frame failure')
        ctx.frame = fail_frame
        with self.assertRaises(RecipeFailed):
            tray.tray_menu_about(ctx)
        self.assertFalse(ctx.menu)

    def test_fake_click_targets_coordinates_not_the_last_queried_node(self):
        ctx = FakeContext()
        ctx.accessible({'name': 'Settings'})
        rx, ry, rw, rh = ctx.rects['Settings']
        ctx.accessible({'name': 'History'})
        ctx.click_at(rx + rw // 2, ry + rh // 2)
        self.assertEqual(ctx.events[-1], ('click', 'Settings', 'left'))

    def test_profile_fragments_cover_model_desktops_and_are_lane_scoped(self):
        for desktop in json.loads((ROOT / 'tools/lab/surface_map/model.json').read_text())['desktops']:
            fragment = ROOT / 'tools/lab/surface_driver/profiles' / (desktop['id'] + '.d') / 'tray.json'
            data = json.loads(fragment.read_text())
            self.assertEqual(set(data), {'tray', 'windows'})
            self.assertEqual(set(data['windows']), {'trayFixture'})
            self.assertIn(data['tray']['notification_server'], ('cinnamon', 'gnome-shell', 'Plasma'))


if __name__ == '__main__':
    unittest.main()
