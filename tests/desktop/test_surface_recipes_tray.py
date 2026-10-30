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
        # Exercise recipe logic with a capable fake; never desktop evidence.
        self.profile['tray'].update(notification_prepare_cli=False, menu_prepare_background=False)
        self.profile['session']='x11'
        self.profile['tray']['icon'] = {'name': 'put.gd', 'application': 'cinnamon'}
        self.profile.setdefault('notifications', {})['accessible'] = {'application': 'cinnamon*'}
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
        if name == 'tray-attention':
            self.obstruction = True
            self._invoke('clipboard')
            self.obstruction = False
            op = copy.deepcopy(self.operations[-1])
            return Fixture(operation_id=op['id'], state=op,
                           retry=lambda: self.call('operation.retryLocalSave', {'id': op['id']}))
        if name == 'upload':
            self.notices.append({'id': 1, 'summary': 'Upload complete'})
            return Fixture(state={'state': 'completed'})
        if name == 'tray-proton-busy':
            raise RecipeBlocked('proton_account_busy fixture unavailable', 670)
        raise AssertionError(name)

    def click_accessible(self, query, button='left'):
        assert any(k in query for k in ('pid', 'application', 'owned'))
        nodes = self.accessible(query)
        assert len(nodes) == 1
        self.click_at(*tray._point(self, nodes[0]), button=button)
        return nodes[0]

    def double_click_accessible(self, query, button='left'):
        self.events.append(('double_click_accessible', query, button))
        return self.click_accessible(query, button)

    def scroll_accessible(self, query, clicks):
        self.events.append(('scroll_accessible', query, clicks))

    def hover_accessible(self, query):
        self.events.append(('hover', query))
        return self.accessible(query)[0]

    def focus(self, window, raise_window=True):
        self.events.append(('focus', window))

    def view(self, method, params=None):
        self.events.append(('view', method, params))
        assert method == 'ui.show'
        self.target = params['view'].title()
        return {'shown': params['view']}

    def call(self, method, params=None):
        self.events.append(('call', method, params))
        if method == 'action.invoke':
            self._invoke(params['action'])
            return {'operationId': self.operations[-1]['id']}
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
            names.add(n['summary'])
            names.update(n.get('buttons', ()))
        if name == '*':
            return [node for value in sorted(names) for node in self.accessible(dict(query, name=value))]
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
                self.menu = not self.menu if self.profile['tray'].get('menu_prepare_background') else True
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
            self.target = {'Change output…': 'Settings'}.get(name, name)
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
                'buttons': ['Save diagnostics for this upload'] if failed else [],
                'hints': {'resident': True}, 'timeout': 0})

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
        elif "'Opened on the host'" in script:
            output = 'opened'
        elif 'zipfile' in script:
            output = 'putgd-diagnostics-20261029-000000Z.zip'
        return {'returncode': 0, 'stdout': output, 'stderr': ''}

    def frame(self, label, window=None, reference=None, **options):
        self.events.append(('frame', label, reference))

    def clipboard_text(self):
        return self.clipboard

    def notification_action(self, identity, action='default'):
        self.events.append(('notification_action', identity, action))
        notice = next(n for n in self.notices if n['id'] == identity)
        notice.setdefault('action_events', []).append({'action': action})
        notice['closed'] = {'reason': 3}
        if action == 'save-diagnostics':
            self.click_at(*tray._point(self, self.accessible({'name': 'Save diagnostics for this upload'})[0]))

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

    def test_desktop_safe_frames_require_a_disposable_local_guest(self):
        for placement, destination, guest, safe in [('boat', 'local', 'cinnamon-x11', True),
                                                   ('guest', 'local', 'gnome-wayland', True),
                                                   ('boat', 'local', None, False),
                                                   ('physical', 'local', 'cinnamon-x11', False),
                                                   ('boat', 'proton', 'gnome-wayland', False)]:
            with self.subTest(placement=placement, destination=destination, guest=guest):
                ctx = FakeContext()
                ctx.placement, ctx.destination = placement, destination
                ctx.profile['guest'] = guest
                frames = []
                ctx.frame = lambda label, **options: frames.append(options)
                tray._frame(ctx, 'proof')
                self.assertEqual(bool(frames[0].get('desktop_safe')), safe)

    def test_wayland_focus_preserves_the_read_only_witness(self):
        for session, raises in [('wayland', False), ('x11', True)]:
            ctx = FakeContext()
            ctx.profile['session'] = session
            calls = []
            ctx.focus = lambda window, **options: calls.append((window, options))
            tray._focus(ctx, 'settings')
            self.assertEqual(calls, [('settings', {'raise_window': raises})])

    def test_main_page_uses_live_nested_selection_states(self):
        for states in ({'selected': True, 'checked': False}, {'selected': False, 'checked': True}):
            with self.subTest(states=states):
                ctx = FakeContext()
                ctx.accessible = lambda query: [{'name': 'History', 'role': 'page tab',
                                                'states': states, 'rect': [10, 10, 80, 24]}]
                tray._page(ctx, 'History')

    def test_rejection_assigns_a_real_shortcut_without_changing_output_policy(self):
        ctx = FakeContext()
        ctx.fixture = lambda name, **options: {}
        def stop_before_input(*args, **options):
            raise RecipeBlocked('stop after binding setup')
        ctx.view = stop_before_input
        with self.assertRaises(RecipeBlocked):
            tray.action_rejected(ctx)
        patches = [e[2]['patch'] for e in ctx.events if e[:2] == ('call', 'settings.patch')]
        self.assertEqual(patches[-1], {'actions': {'clipboard': {'shortcut': tray.CHORDS['clipboard']}}})

    def test_available_scenarios_resolve_the_core_fixture(self):
        cases = [(tray.recording_icon, 'tray-active-recording'), (tray.cancel_uploads, 'tray-two-uploads'),
                 (tray.update_available, 'tray-update-available'), (tray.quit_resident, 'resident-lifecycle'),
                 (tray.qt_fallback, 'tray-qt-fallback'), (tray.other_client_menu, 'shortcut-other-menu'),
                 (tray.recorder_ignores_shortcut, 'shortcut-settings-recorder'),
                 (tray.recorder_releases_grabs, 'shortcut-settings-recorder'),
                 (tray.keyboard_layout_change, 'shortcut-keyboard-layout'),
                 (tray.action_rejected, 'settings-no-output'), (tray.upload_failed_notice, 'tray-failed')]
        for function, name in cases:
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                fixture = ctx.fixture
                def unavailable(requested, **options):
                    if requested == name:
                        ctx.events.append(('fixture', requested))
                        raise RecipeBlocked('fixture unavailable in contract fake', 670)
                    return fixture(requested, **options)
                ctx.fixture = unavailable
                with self.assertRaises(RecipeBlocked):
                    function(ctx)
                self.assertIn(('fixture', name), ctx.events)

    def test_automatic_cleanup_notice_has_restart_and_delay_budget(self):
        entry = next(r for r in self.recipes() if 'notification.incomplete-uploads' in r.interactions)
        self.assertGreaterEqual(entry.timeout, 240)

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

    def test_gnome_notification_preparation_avoids_unneeded_qt_input(self):
        ctx = FakeContext()
        ctx.profile['tray']['notification_prepare_cli'] = True
        tray.save_diagnostics(ctx)
        self.assertFalse(any(e[0] in ('view', 'keys') for e in ctx.events))
        self.assertIn(('call', 'action.invoke', {'action': 'clipboard'}), ctx.events)
        self.assertTrue(any(e[0] == 'notification_action' and e[2] == 'save-diagnostics' for e in ctx.events))

    def test_background_menu_avoids_qt_window_and_closes_via_shell(self):
        ctx = FakeContext()
        ctx.profile['tray']['menu_prepare_background'] = True
        tray.context_menu(ctx)
        self.assertFalse(any(e[0] in ('view', 'keys') for e in ctx.events))
        self.assertEqual([e for e in ctx.events if e[0] == 'click'],
                         [('click', 'put.gd', 'right'), ('click', 'put.gd', 'right')])
        self.assertFalse(ctx.menu)

    def test_failed_menu_open_receives_no_cleanup_input(self):
        ctx = FakeContext()
        def refused(*args, **kwargs):
            raise RecipeFailed('desktop input is obscured')
        ctx.click_accessible = refused
        with self.assertRaises(RecipeFailed):
            tray.context_menu(ctx)
        self.assertFalse(any(e[0] in ('keys', 'click') for e in ctx.events))

    def test_menu_action_preparation_keeps_background_and_uses_real_click(self):
        ctx = FakeContext()
        ctx.profile['tray'].update(menu_prepare_background=True, hover_before_click=True)
        tray.tray_menu_upload_clipboard(ctx)
        self.assertFalse(any(e[0] in ('view', 'keys') for e in ctx.events))
        self.assertFalse(any(e[0] == 'call' and e[1] == 'action.invoke' for e in ctx.events))
        self.assertIn(('click', 'Upload clipboard', 'left'), ctx.events)

    def test_named_menu_label_uses_its_unique_owned_parent_enabled_state(self):
        for parent_enabled in (True, False):
            with self.subTest(parent_enabled=parent_enabled):
                ctx = FakeContext()
                ctx.profile['tray']['menu'] = {'role': 'label', 'enabled_from_parent': True}
                accessible = ctx.accessible
                def nodes(query):
                    if query.get('role') == 'menu item':
                        return [{'name': '', 'role': 'menu item', 'rect': [10, 20, 200, 30],
                                 'pid': 4242, 'visible': True, 'showing': True, 'enabled': parent_enabled}]
                    rows = accessible(query)
                    for row in rows:
                        row['enabled'] = not parent_enabled
                        row['ancestors'] = [{'role': 'menu item', 'rect': [10, 20, 200, 30]}]
                    return rows
                ctx.accessible = nodes
                if parent_enabled:
                    tray._press(ctx, 'History', 'menu item')
                    self.assertIn(('click', 'History', 'left'), ctx.events)
                else:
                    with self.assertRaises(RecipeFailed):
                        tray._press(ctx, 'History', 'menu item')
                    self.assertFalse(any(e[0] == 'click' for e in ctx.events))

    def test_ambiguous_menu_parent_blocks_before_input(self):
        ctx = FakeContext()
        node = {'pid': 4242, 'ancestors': [{'role': 'menu item', 'rect': [1, 2, 3, 4]}]}
        ctx.accessible = lambda query: []
        with self.assertRaises(RecipeBlocked):
            tray._menu_parent_state(ctx, node)
        self.assertEqual(ctx.events, [])

    def test_capture_navigation_cannot_pass_from_a_stale_section_inspector(self):
        ctx = FakeContext()
        ctx.profile['tray']['menu_prepare_background'] = True
        ctx.target = 'History'
        click = ctx.click_at
        def ignored_navigation(x, y, button='left'):
            for name, rect in ctx.rects.items():
                rx, ry, rw, rh = rect
                if name == 'Change output…' and rx <= x < rx + rw and ry <= y < ry + rh:
                    return
            click(x, y, button)
        ctx.click_at = ignored_navigation
        with self.assertRaises(RecipeFailed):
            tray.tray_menu_change_output(ctx)

    def test_desktop_gestures_use_public_guarded_apis(self):
        ctx = FakeContext()
        ctx.profile['tray']['activation_clicks'] = 2
        tray.activate(ctx)
        self.assertTrue(any(e[0] == 'double_click_accessible' for e in ctx.events))
        tray.scroll_noop(ctx)
        self.assertEqual([e[2] for e in ctx.events if e[0] == 'scroll_accessible'], [2, -2])

    def test_hover_is_guarded_before_the_gnome_icon_click(self):
        ctx = FakeContext()
        ctx.profile['tray']['hover_before_click'] = True
        tray._icon_input(ctx, 'right')
        self.assertLess(next(i for i,e in enumerate(ctx.events) if e[0] == 'hover'),
                        next(i for i,e in enumerate(ctx.events) if e[0] == 'click'))

    def test_explicit_blocked_rows_have_issue(self):
        for interaction, reason in tray.BLOCKED.items():
            with self.subTest(interaction=interaction):
                r = next(r for r in self.recipes() if interaction in r.interactions)
                ctx = FakeContext()
                with self.assertRaises(RecipeBlocked) as caught:
                    r.function(ctx)
                self.assertEqual(caught.exception.issue, 670)

    def test_upload_complete_clicks_exact_id_before_framing_or_host_observation(self):
        ctx = FakeContext()
        tray.upload_complete(ctx)
        action = ctx.events.index(('notification_action', 1, 'default'))
        self.assertLess(action, next(i for i,e in enumerate(ctx.events) if e[0]=='shell'))
        self.assertLess(action, ctx.events.index(('frame', 'after', None)))

    def test_notification_actions_use_core_input_and_correlate_the_id(self):
        for function in (tray.save_diagnostics, tray.show_in_folder,
                         tray.resident_close, tray.diagnostics_not_saved):
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                function(ctx)
                self.assertIn(('notification_action', 1, 'save-diagnostics'), ctx.events)
                self.assertIn(('frame', 'after', None), ctx.events)

    def test_notify_without_server_reply_is_not_a_click_identity(self):
        ctx = FakeContext()
        ctx.notices = [{'id': None, 'summary': 'Upload complete'}]
        with self.assertRaises(RecipeFailed):
            tray._notice(ctx, 'Upload complete')

    def test_uncorrelated_action_signal_cannot_pass(self):
        ctx = FakeContext()
        ctx.notices = [{'id': 4, 'summary': 'Upload complete', 'action_events': [{'action':'other'}]}]
        ctx.notification_action = lambda *a: None
        with self.assertRaises(RecipeFailed):
            tray._action(ctx, ctx.notices[0], 'default')

    def test_profile_restart_waits_for_journal_readiness_before_input(self):
        ctx = FakeContext()
        original = ctx.call
        ready = iter((False, True, False, True))
        def call(method, params=None):
            result = original(method, params)
            if method == 'ui.snapshot':
                result['operationsReady'] = next(ready)
            return result
        def wait(predicate, what, **kwargs):
            for _ in range(3):
                value = predicate()
                if value:
                    return value
            raise RecipeFailed(what)
        ctx.call, ctx.wait = call, wait
        tray.activate(ctx)
        self.assertIn(('click', 'put.gd', 'left'), ctx.events)

    def test_shortcuts_read_native_diagnostics_from_the_ui_snapshot(self):
        ctx = FakeContext()
        tray.shortcut_clipboard(ctx)
        self.assertFalse(any(e[0] == 'call' and e[1] == 'diagnostics' for e in ctx.events))
        self.assertIn(('keys', (tray.CHORDS['clipboard'],)), ctx.events)

    def test_outdated_helper_omitting_binding_blocks_before_input(self):
        ctx = FakeContext()
        original = ctx.call
        def snapshot(method, params=None):
            result = original(method, params)
            if method == 'ui.snapshot':
                d = result['diagnostics']
                d['shortcuts']['bindings']['edit'] = {'requested': tray.CHORDS['edit']}
                d['capabilities'] = {'source': {'helper': {'outdated': True, 'extensionVersion': 1,
                                                         'packagedExtensionVersion': 15}}}
            return result
        ctx.call = snapshot
        with self.assertRaisesRegex(RecipeBlocked, 'outdated'):
            tray.shortcut_edit(ctx)
        self.assertFalse(any(e[0] == 'keys' for e in ctx.events))

    def test_cancelled_selector_gets_only_one_escape(self):
        ctx = FakeContext()
        tray.shortcut_region(ctx)
        self.assertEqual(sum(e == ('keys', ('Esc',)) for e in ctx.events), 1)

    def test_record_toggle_adopts_active_recording_after_selector_cancel(self):
        ctx = FakeContext()
        fixture = ctx.fixture
        def unavailable(name, **options):
            if name == 'tray-active-recording':
                ctx.events.append(('fixture', name))
                raise RecipeBlocked('synthetic recording unavailable', 672)
            return fixture(name, **options)
        ctx.fixture = unavailable
        with self.assertRaises(RecipeBlocked) as error:
            tray.shortcut_record(ctx)
        self.assertEqual(error.exception.issue, 672)
        self.assertIn(('fixture', 'tray-active-recording'), ctx.events)

    def test_refused_recording_selector_guard_does_not_retry_keyboard_cleanup(self):
        ctx = FakeContext()
        def refused(window, raise_window=True):
            raise RecipeFailed("input target 'recsel' is obscured by 'put.gd' (window 42)")
        ctx.focus = refused
        with self.assertRaises(RecipeFailed):
            tray.shortcut_record(ctx)
        self.assertEqual([e for e in ctx.events if e[0] == 'keys'],
                         [('keys', (tray.CHORDS['record'],))])

    def test_refused_native_grab_is_not_relabelled_as_fixed_issue_802(self):
        ctx = FakeContext()
        def refused(*args, **kwargs):
            raise RecipeFailed('override-redirect target does not take keyboard focus')
        ctx.focus = refused
        with self.assertRaises(RecipeFailed):
            tray.shortcut_region(ctx)
        self.assertEqual([e for e in ctx.events if e[0] == 'keys'], [('keys', ('Ctrl+Shift+1',))])

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

    def test_catalog_hint_keeps_the_full_desktop_frame(self):
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
        with self.assertRaises(RecipeFailed):
            tray._drawn_notice(ctx, {'summary': 'Upload complete'})

    def test_catalog_ids_match_published_action_identity_rules(self):
        import hashlib
        for label in ('About put.gd', 'Quit put.gd', 'Edit clipboard image', 'Edit an image file…'):
            identity = 'trayMenuAction-' + hashlib.sha256(label.encode()).hexdigest()[:16]
            self.assertEqual(tray.CONTROLS[identity], label)
        source = (ROOT / 'src/ui/TrayController.cpp').read_text()
        for identity in ('trayOutputSummary', 'trayChangeOutput', 'trayCancelUpload'):
            self.assertIn('setObjectName("' + identity + '")', source)

    def test_file_chooser_cleanup_focuses_its_owned_dialog(self):
        ctx = FakeContext()
        tray.edit_file(ctx)
        self.assertIn(('focus', 'fileDialog'), ctx.events)
        focus = ctx.events.index(('focus', 'fileDialog'))
        self.assertGreater(next(i for i, e in enumerate(ctx.events) if e == ('keys', ('Esc',))), focus)

    def test_about_dialog_is_closed_after_its_evidence_frame(self):
        ctx = FakeContext()
        tray.tray_menu_about(ctx)
        self.assertFalse(ctx.about_open)
        self.assertEqual(ctx.events[-1], ('keys', ('Esc',)))

    def test_about_failure_after_open_closes_the_dialog(self):
        ctx = FakeContext()
        frame = ctx.frame
        def fail_frame(label, *args, **kwargs):
            if label == 'after':
                raise RecipeFailed('synthetic frame failure')
            return frame(label, *args, **kwargs)
        ctx.frame = fail_frame
        with self.assertRaises(RecipeFailed):
            tray.tray_menu_about(ctx)
        self.assertFalse(ctx.about_open)
        self.assertIn(('focus', 'trayAbout'), ctx.events)

    def test_idle_observes_absent_menu_rows_in_one_bounded_inventory(self):
        ctx = FakeContext()
        ctx.profile['tray']['menu_prepare_background'] = True
        accessible = ctx.accessible
        queries = []
        def observed(query):
            queries.append(query)
            return accessible(query)
        ctx.accessible = observed
        tray.idle(ctx)
        self.assertEqual(sum(q.get('name') == '*' for q in queries), 1)
        self.assertFalse(any(q.get('name') in ('Cancel upload', 'Action needs attention') for q in queries))

    def test_completed_navigation_does_not_send_unscoped_escape(self):
        for function in (tray.tray_menu_history, tray.tray_menu_destinations,
                         tray.tray_menu_change_output):
            with self.subTest(recipe=function.__name__):
                ctx = FakeContext()
                ctx.profile['tray']['menu_prepare_background'] = True
                function(ctx)
                self.assertFalse(any(e[0] == 'keys' for e in ctx.events))

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
            self.assertEqual(set(data) - {'notifications'}, {'tray', 'windows'})
            self.assertEqual(set(data['windows']), {'trayFixture', 'trayAbout'})
            self.assertIn(data['tray']['notification_server'], ('cinnamon', 'gnome-shell', 'Plasma'))


if __name__ == '__main__':
    unittest.main()
