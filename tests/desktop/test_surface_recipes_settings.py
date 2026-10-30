#!/usr/bin/env python3
"""Light recipe contracts; fake input is never graphical acceptance evidence."""
from __future__ import annotations

import copy
import json
import os
import shlex
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import settings as lane

RECIPES = [r for r in REGISTRY if r.function.__module__ == lane.__name__]


def assign(document, path, value):
    keys = path.split('.')
    for key in keys[:-1]:
        document = document.setdefault(key, {})
    document[keys[-1]] = value


def merge(document, patch):
    for key, value in patch.items():
        if isinstance(value, dict):
            merge(document.setdefault(key, {}), value)
        else:
            document[key] = copy.deepcopy(value)


class Fake(Context):
    host = 'boat'
    desktop = 'mint-cinnamon-x11'
    placement = 'boat'
    destination = 'local'
    package = 'source'
    profile = {}

    def __init__(self):
        from tools.lab.surface_driver.profiles import load_profile
        self.profile = load_profile(self.desktop)
        self.targets = {}
        self.pointer = (0, 0)
        self.notes = []
        self.document = {
            'preferences': {'notifications': True, 'launchAtLogin': False,
                            'unlockCredentialsAtStartup': False,
                            'appearance': {'theme': 'system'},
                            'diagnostics': {'debugLogging': False},
                            'uploadClipboardBehavior': 'each_link', 'regionSelectionMode': 'live'},
            'archive': {'keepUploads': True, 'root': '/tmp/private/archive',
                        'folders': {k: None for k in ('images', 'videos', 'text', 'other')}},
            'retention': {'recentActivityLimit': 100, 'historyRetentionDays': None, 'failedArtifactDays': 7},
            'limits': {'maxFiles': 100},
            'naming': {'mode': 'original', 'collisionPolicy': 'rename'},
            'actions': {k: {'shortcut': ''} for k in ('region', 'window', 'desktop', 'clipboard', 'edit', 'record')},
        }
        self.disk = copy.deepcopy(self.document)
        self.section = 0
        self.focused_control = None
        self.recording_action = None
        self.pending_index = 0
        self.text = ''
        self.frames = []
        self.inputs = []
        self.calls = []
        self.clipboard = ''
        self.support_expanded = False
        self.category_expanded = False
        self.available = True
        self.disk_writable = True
        self.command_error = None
        self.pkg = {'open': False}
        self.fixtures = {'diagnostics-upload': {'profileId': 'synthetic', 'loopback': True, 'isolated': True}}
        self.config_path = '/tmp/private/config/putgd/config.json'
        self.about_visible = False
        self.about_expanded = False
        self.confirmation = None
        self.build = {'gitCommit': 'a' * 40, 'version': '26.1029.1-dev+gaaaaaaaa'}
        self.fixtures['settings'] = {}
        self.fixtures['settings-conflict'] = {'external_change': lambda patch: merge(self.disk, patch)}

    def checkpoint(self):
        if self.disk_writable:
            self.disk = copy.deepcopy(self.document)

    def view(self, method, params=None):
        return self.call(method, params)

    def call(self, method, params=None):
        params = params or {}
        self.calls.append((method, copy.deepcopy(params)))
        if method == self.command_error:
            return {'ok': False, 'error': {'code': 'injected'}, 'result': {}}
        if method == 'settings.get':
            return {'document': copy.deepcopy(self.document), 'metadata': {'path': self.config_path, 'observedRevision': 'rev'}}
        if method == 'settings.patch':
            assert params['expectedRevision'] == 'rev'
            merge(self.document, params['patch'])
            self.checkpoint()
            return {}
        if method == 'ui.show':
            if params['view'] == 'about': self.about_visible = True
            return {'shown': params['view']}
        if method == 'ui.viewState':
            theme = self.document['preferences']['appearance']['theme']
            return {'settings': {'section': self.section}, 'appearance': {'theme': theme, 'scheme': 'dark' if theme == 'system' else theme, 'desktop': 'dark'}}
        if method == 'ui.snapshot':
            bindings = {a: {'state': 'recording' if a == self.recording_action else 'bound',
                            'bound': a != self.recording_action,
                            'actual': [v['shortcut']]} for a, v in self.document['actions'].items()}
            return {'build': self.build, 'diagnostics': {'loginStartup': {'enabled': self.document['preferences']['launchAtLogin'], 'applied': True},
                                    'capabilities': {'source': {'shortcuts': {'available': self.available, 'bindings': bindings}}}}}
        if method == 'ui.diagnostics':
            if params['action'] == 'setDesktopDetails':
                self.support_expanded = params['enabled']
            elif params['action'] == 'openPackage':
                self.pkg = {'open': True, 'includeNames': False, 'previewing': False, 'upload': False,
                            'saving': False, 'contents': 'manifest and logs', 'redaction': 'names hashed',
                            'uploadTarget': {'checking': False, 'available': True, 'profileId': 'synthetic'}}
            elif params['action'] == 'closePackage':
                self.pkg = {'open': False}
            elif params['action'] == 'setPackage':
                self.pkg['includeNames'] = params['includeNames']
            return {'package': copy.deepcopy(self.pkg), 'desktopSupport': {'expanded': self.support_expanded},
                    'debugLogging': {'expiresAt': '2026-10-31T00:00:00Z'}}
        if method in ('settings.validate', 'settings.reload'):
            return {}
        if method == 'diagnostics.bundle':
            return {'path': '/tmp/private/putgd-diagnostics-test.zip', 'files': ['manifest']}
        raise AssertionError('unimplemented fake method: ' + method)

    def wait(self, predicate, what, **kwargs):
        for _ in range(3):
            value = predicate()
            if value:
                return value
        raise RecipeFailed(what)

    def click(self, control, window, **kwargs):
        if control == 'closeAbout': self.about_visible = False
        if control == 'buildDetails': self.about_expanded = not self.about_expanded
        if control == 'copyBuildDetails': self.clipboard = json.dumps(self.build)
        if control == 'resetAdvancedDefaults': self.confirmation = 'Reset limits to defaults'
        if control == 'reloadSettings': self.confirmation = 'Reload settings from file'
        if control == 'text:Reset limits':
            self.document['limits'].update(maxFiles=100, pendingOperations=8, concurrentUploads=3, maxFileBytes='auto', maxBatchBytes='auto', pendingArtifactBytes='auto', pendingImageBytes=67108864, clipboardTextBytes=16777216, imagePixels=100000000, imageBytes=536870912)
            self.document['retention'].update(failedArtifactDays=7, recentActivityLimit=100, previewCacheBytes=268435456, historyRetentionDays=None)
            self.checkpoint(); self.confirmation = None
        if control == 'text:Reload from file':
            self.document = copy.deepcopy(self.disk); self.confirmation = None
        self.inputs.append(('click', control, window))
        self.focused_control = control
        if control.startswith('settingsSection'):
            self.section = int(control[-1])
        toggles = {'launchAtLogin': 'preferences.launchAtLogin',
                   'unlockCredentialsAtStartup': 'preferences.unlockCredentialsAtStartup',
                   'notifications': 'preferences.notifications', 'debugLogging': 'preferences.diagnostics.debugLogging',
                   'keepUploadCopies': 'archive.keepUploads'}
        if control in toggles:
            path = toggles[control]
            assign(self.document, path, not lane.at(self.document, path))
            self.checkpoint()
        if control.startswith('record') and control.endswith('Shortcut'):
            action = control[6:-8].lower()
            self.recording_action = None if self.recording_action == action else action
        if control.startswith('clear') and control.endswith('Shortcut'):
            self.document['actions'][control[5:-8].lower()]['shortcut'] = ''
            self.checkpoint()
        if control == 'createDiagnosticsPackage':
            self.call('ui.diagnostics', {'action': 'openPackage'})
        if control == 'includeFileNames':
            self.pkg['includeNames'] = not self.pkg['includeNames']
        if control == 'closePackage':
            self.pkg = {'open': False}
        if control == 'uploadPackage':
            self.pkg['upload'] = not self.pkg['upload']
        if control == 'savePackage':
            self.pkg['saved'] = {'path': '/tmp/private/putgd-diagnostics-test.zip'}
            if self.pkg['upload']:
                self.pkg['saved'].update(link='http://127.0.0.1/synthetic', linkCopied=True)
                self.clipboard = self.pkg['saved']['link']
        if control == 'categoryFoldersToggle':
            self.category_expanded = not self.category_expanded
        if control == 'desktopSupportDetailsToggle':
            self.support_expanded = not self.support_expanded
        if control == 'copyDesktopDetails':
            self.clipboard = '{"capabilities": {}}'

    def keys(self, *sequence):
        self.inputs.append(('keys', sequence))
        for key in sequence:
            if self.confirmation and key in ('Esc', 'Return'):
                self.confirmation = None
                continue
            if self.about_visible and key == 'Esc': self.about_visible = False
            if self.about_visible and key == 'Ctrl+C': self.clipboard = self.build['version'].split('-')[0]
            if self.recording_action:
                action = self.recording_action
                if key != 'Esc':
                    self.document['actions'][action]['shortcut'] = '' if key == 'Backspace' else key
                    self.checkpoint()
                self.recording_action = None
                continue
            if self.focused_control == 'settingsNavigation':
                if key == 'Home': self.section = 0
                if key == 'Down': self.section += 1
                continue
            if key == 'Home': self.pending_index = 0
            if key == 'Down': self.pending_index += 1
            if key == 'Return':
                options = {
                    'appearanceTheme': ('preferences.appearance.theme', ['system', 'dark', 'light']),
                    'uploadClipboardBehavior': ('preferences.uploadClipboardBehavior', ['each_link', 'list']),
                    'regionSelectionMode': ('preferences.regionSelectionMode', ['live', 'screenshot_first']),
                    'historyRetentionChoice': ('retention.historyRetentionDays', [None, 30, 90, 365]),
                    'failedArtifactChoice': ('retention.failedArtifactDays', [0, 1, 3, 7, 14, 30, 90]),
                    'namingMode': ('naming.mode', ['original', 'generated', 'template']),
                    'collisionPolicy': ('naming.collisionPolicy', ['rename', 'skip', 'overwrite']),
                }
                if self.focused_control in options:
                    path, values = options[self.focused_control]
                    assign(self.document, path, values[self.pending_index])
                    self.checkpoint()
            if key == 'Ctrl+V':
                self.text = self.clipboard
            if key == 'Tab' and self.text:
                path = {'recentActivityLimit': 'retention.recentActivityLimit', 'archiveRoot': 'archive.root'}.get(self.focused_control)
                if self.focused_control and self.focused_control.startswith('folder'):
                    path = 'archive.folders.' + self.focused_control[6:].lower()
                if self.focused_control and self.focused_control.endswith('Template'):
                    path = 'naming.' + self.focused_control
                if path:
                    if self.focused_control != 'recentActivityLimit' or self.text.isdigit():
                        assign(self.document, path, int(self.text) if self.focused_control == 'recentActivityLimit' else self.text)
                    self.checkpoint()
                self.text = ''

    def type_text(self, text):
        self.inputs.append(('type', text))
        self.text = text

    def shell(self, script, **kwargs):
        if script.startswith('python3 -c ') and 'XDG_CONFIG_HOME' in script:
            return {'returncode': 0, 'stdout': json.dumps({'path': self.config_path, 'document': self.disk})}
        if script.startswith('printf %s '):
            self.clipboard = shlex.split(script)[2]
            self.inputs.append(('clipboard', self.clipboard))
            return {'returncode': 0, 'stdout': ''}
        if 'private accessibility tree exceeded' in script:
            request = json.loads(shlex.split(script)[-1])
            return {'returncode': 0, 'stdout': json.dumps(self.accessible(request['query']))}
        if 'zipfile' in script:
            return {'returncode': 0, 'stdout': ''}
        if script.endswith(self.profile['settings']['prepare_script']):
            return {'returncode': 0, 'stdout': ''}
        if script.startswith('settings_window='):
            return {'returncode': 0, 'stdout': json.dumps({'pid': 123, 'active': '42', 'focus': '42',
                                                         'x': self.pointer[0], 'y': self.pointer[1]})}
        if script == self.profile['settings']['prepare_script']:
            return {'returncode': 0, 'stdout': ''}
        raise AssertionError('unexpected shell command')

    def geometry(self, window):
        return {'window_id': '42', 'x': 120, 'y': 40, 'width': 1440, 'height': 960, 'scale': 1}

    def windows(self, query=None):
        title = (query or {}).get('title', '')
        return [{'title': title}] if (title == '^About put.gd$' and self.about_visible or title == '^put.gd$' or self.confirmation and title == '^' + self.confirmation + '$') else []

    def focus(self, window, raise_window=True):
        self.inputs.append(('focus', window))
        return {}

    def accessible(self, query):
        if query.get('window') == 'About put.gd':
            if query['name'] == 'Copy build details': return [{'name': query['name']}] if self.about_expanded else []
            if query['name'] == 'Commit': return [{'description': self.build['gitCommit']}]
            return [{'text': self.build['version'].split('-')[0]}]
        control = next(k for k, v in self.profile['settings']['live_controls'].items() if v == query)
        # Status insertion shifts controls; a stale point must not hit a field.
        shift = 50 if self.recording_action or any('Esc' in str(i) for i in self.inputs) else 0
        rectangle = [700, 240 + shift, 180, 36]
        self.targets = {(760, 258 + shift): control}
        return [{'pid': 123, 'rect': rectangle, 'checked': self.category_expanded if control == 'categoryFoldersToggle' else False,
                 'focused': self.focused_control == control}]

    def click_at(self, x, y, **kwargs):
        self.pointer = (x, y)
        self.click(self.targets[(x, y)], 'settings')

    def note(self, text):
        self.notes.append(text)

    def frame(self, label, window=None, reference=None):
        self.frames.append((label, window, reference))

    def use_catalog(self, window, reference):
        pass

    def fixture(self, name):
        if name not in self.fixtures:
            self.blocked('missing fixture: ' + name)
        return self.fixtures[name]

    def clipboard_text(self):
        return self.clipboard

    def cli(self, *args, **kwargs):
        payload = self.call(args[0], json.loads(args[1]) if len(args) > 1 else {})
        return {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': payload})}

    def mcp(self, tool, arguments=None):
        arguments = arguments or {}
        mapping = {'patch_configuration': 'settings.patch', 'reload_configuration': 'settings.reload',
                   'settings_diagnostics': 'ui.diagnostics', 'create_diagnostics_bundle': 'diagnostics.bundle'}
        translated = { {'expected_revision': 'expectedRevision', 'include_names': 'includeNames'}.get(k, k): v for k, v in arguments.items() }
        return {'ok': True, 'result': self.call(mapping[tool], translated)}


class Contracts(unittest.TestCase):
    def test_every_owned_id_once_and_no_foreign_ids(self):
        owners = json.loads((ROOT/'tools/lab/surface_driver/ownership.json').read_text())['owners']
        expected = {key for key, owner in owners.items() if owner == 'settings'}
        ids = [identifier for rec in RECIPES for identifier in rec.interactions]
        self.assertEqual(set(ids), expected)
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(len(expected), 69)

    def test_declared_blockers_raise_with_specific_reason(self):
        for rec in RECIPES:
            if rec.interactions[0] in lane.BLOCKED:
                with self.subTest(id=rec.interactions[0]), self.assertRaises(RecipeBlocked) as caught:
                    rec.function(Fake())
                self.assertEqual(caught.exception.reason, lane.BLOCKED[rec.interactions[0]])

    def test_runnable_recipes_use_input_and_referenced_frames(self):
        partial = {'cli.settings', 'cli.ui', 'cli.diagnostics', 'mcp.configuration', 'mcp.diagnostics'}
        for rec in RECIPES:
            if rec.interactions[0] in lane.BLOCKED:
                continue
            with self.subTest(recipe=rec.name):
                ctx = Fake()
                if rec.interactions[0] in partial:
                    with self.assertRaisesRegex(RecipeBlocked, 'Partial'):
                        rec.function(ctx)
                else:
                    rec.function(ctx)
                self.assertTrue(ctx.inputs)
                self.assertGreaterEqual(len(ctx.frames), 2)
                self.assertTrue(all(ref in (None, *lane.REFERENCES, '07-settings-general-light', '58-diagnostics-package', 'about', 'about-build-details', 'confirm-reset-limits', '53-reload-confirm') for _, _, ref in ctx.frames))
                self.assertEqual(ctx.disk, ctx.document)
                self.assertTrue(any(method == 'ui.viewState' for method, _ in ctx.calls))

    def test_channel_probe_restores_notifications_on_save_failure(self):
        for channel, function in (('cli', lane.cli_settings), ('mcp', lane.mcp_configuration)):
            with self.subTest(channel=channel):
                ctx = Fake()
                original = ctx.document['preferences']['notifications']
                invoke = getattr(ctx, channel)
                def fail_disk(method, *args, **kwargs):
                    if method in ('settings.patch', 'patch_configuration'):
                        ctx.disk_writable = False
                    return invoke(method, *args, **kwargs)
                setattr(ctx, channel, fail_disk)
                with self.assertRaisesRegex(RecipeFailed, 'persisted'):
                    function(ctx)
                self.assertEqual(ctx.document['preferences']['notifications'], original)

    def test_first_launch_helper_reopens_after_accessibility_enablement(self):
        from tools.lab.surface_driver.recipes.settings_input import ensure_accessible
        ctx = Fake()
        accessible, shell = ctx.accessible, ctx.shell
        queries = []
        def probe(query):
            queries.append(query)
            return [] if len(queries) == 1 else accessible(query)
        ctx.accessible = probe
        ctx.shell = lambda script, **kwargs: ({'returncode': 0, 'stdout': ''}
                    if script == 'test ! -d /proc/123' else shell(script, **kwargs))
        ensure_accessible(ctx)
        self.assertIn(('keys', ('Alt+F4',)), ctx.inputs)
        self.assertIn(('ui.show', {'view': 'settings'}), ctx.calls)
        self.assertIn('accessibility-before-reopen', [f[0] for f in ctx.frames])
        self.assertIn('accessibility-after-reopen', [f[0] for f in ctx.frames])

    def test_theme_reference_tracks_observed_scheme(self):
        ctx = Fake()
        lane.appearance(ctx)
        refs = {label: ref for label, _, ref in ctx.frames}
        self.assertEqual(refs['after-light'], '07-settings-general-light')
        self.assertEqual(refs['after-dark'], '07-settings-general')
        self.assertEqual(refs['after-system'], '07-settings-general')

    def test_obstructed_input_is_withheld(self):
        ctx = Fake()
        shell = ctx.shell
        def obstructed(script, **kwargs):
            if script.startswith('settings_window='):
                return {'returncode': 1, 'stdout': ''}
            return shell(script, **kwargs)
        ctx.shell = obstructed
        from tools.lab.surface_driver.recipes.settings_input import click
        with self.assertRaises(RecipeBlocked) as caught:
            click(ctx, 'archiveRoot')
        self.assertEqual(caught.exception.issue, 699)
        self.assertFalse(ctx.inputs)

    def test_scrolling_requeries_live_geometry(self):
        ctx = Fake()
        accessible = ctx.accessible
        scrolled = []
        def query(control):
            if not scrolled:
                return [{'pid': 123, 'rect': [700, 1100, 180, 36]}]
            return accessible(control)
        ctx.accessible = query
        ctx.scroll = lambda x, y, clicks: scrolled.append((x, y, clicks))
        from tools.lab.surface_driver.recipes.settings_input import click
        click(ctx, 'archiveRoot')
        self.assertEqual(scrolled, [(1536, 520, -3)])
        self.assertEqual(ctx.focused_control, 'archiveRoot')

    def test_unavailable_clipboard_writer_withholds_input(self):
        ctx = Fake()
        ctx.shell = lambda script, **kwargs: {'returncode': 127, 'stdout': ''}
        with self.assertRaisesRegex(RecipeBlocked, 'clipboard writer unavailable'):
            lane.replace(ctx, 'fileTemplate', 'surface-{name}.{ext}')
        self.assertFalse(ctx.inputs)

    def test_template_clipboard_is_ready_before_selection(self):
        ctx = Fake()
        lane.replace(ctx, 'fileTemplate', 'surface-{name}.{ext}')
        clipboard = next(i for i, event in enumerate(ctx.inputs) if event[0] == 'clipboard')
        click = next(i for i, event in enumerate(ctx.inputs) if event[0] == 'click')
        select = ctx.inputs.index(('keys', ('Ctrl+A',)))
        paste = ctx.inputs.index(('keys', ('Ctrl+V',)))
        self.assertLess(clipboard, click)
        self.assertLess(click, select)
        self.assertEqual(paste, select + 1)
        self.assertEqual(ctx.document['naming']['fileTemplate'], 'surface-{name}.{ext}')

    def test_transient_geometry_gap_is_observed_before_input(self):
        ctx = Fake()
        geometry, calls = ctx.geometry, []
        def remapping(window):
            calls.append(window)
            if len(calls) == 1:
                raise RecipeBlocked('window is remapping')
            return geometry(window)
        ctx.geometry = remapping
        from tools.lab.surface_driver.recipes.settings_input import click
        click(ctx, 'archiveRoot')
        self.assertGreaterEqual(len(calls), 2)
        self.assertEqual(ctx.focused_control, 'archiveRoot')

    def test_zero_height_control_cannot_receive_a_click(self):
        ctx = Fake()
        ctx.accessible = lambda query: [{'pid': 123, 'rect': [700, 240, 180, 0]}]
        from tools.lab.surface_driver.recipes.settings_input import click
        with self.assertRaisesRegex(RecipeBlocked, 'missing or ambiguous'):
            click(ctx, 'settingsSection0')
        self.assertFalse(ctx.inputs)

    def test_wrong_owner_accessibility_is_withheld(self):
        ctx = Fake()
        ctx.accessible = lambda query: [{'pid': 999, 'rect': [700, 240, 180, 36]}]
        from tools.lab.surface_driver.recipes.settings_input import click
        with self.assertRaisesRegex(RecipeBlocked, 'missing or ambiguous'):
            click(ctx, 'archiveRoot')
        self.assertFalse(ctx.inputs)

    def test_disk_divergence_fails_even_when_resident_changed(self):
        ctx = Fake()
        ctx.disk_writable = False
        rec = next(r for r in RECIPES if r.interactions == ('settings.notifications',))
        with self.assertRaisesRegex(RecipeFailed, 'persisted'):
            rec.function(ctx)

    def test_optional_field_waits_for_asynchronous_acceptance(self):
        ctx = Fake()
        del ctx.document['preferences']['diagnostics']
        del ctx.disk['preferences']['diagnostics']
        polls = []
        def delayed_wait(predicate, what, **kwargs):
            if 'accepted by resident' in what:
                polls.append(predicate())
                ctx.document['preferences']['diagnostics'] = {'debugLogging': True}
                ctx.checkpoint()
            value = predicate()
            ctx.expect(value, what)
            return value
        ctx.wait = delayed_wait
        lane.saved(ctx, 'preferences.diagnostics.debugLogging', True, 0)
        self.assertEqual(polls, [False])

    def test_blocked_category_recipe_restores_its_disclosure(self):
        ctx = Fake()
        click = ctx.click
        def missing_inventory(control, *args, **kwargs):
            if control == 'folderImages':
                ctx.blocked('missing category field inventory')
            return click(control, *args, **kwargs)
        ctx.click = missing_inventory
        with self.assertRaisesRegex(RecipeBlocked, 'missing category field'):
            lane.category_folders(ctx)
        self.assertFalse(ctx.category_expanded)
        lane.naming(ctx)
        self.assertFalse(ctx.category_expanded)

    def test_blocked_package_does_not_poison_next_recipe(self):
        ctx = Fake()
        ctx.call('ui.diagnostics', {'action': 'openPackage'})
        click = ctx.click
        def require_unblocked_page(*args, **kwargs):
            ctx.expect(not ctx.pkg['open'], 'modal dialog blocks real page input')
            return click(*args, **kwargs)
        ctx.click = require_unblocked_page
        lane.opened(ctx, 1)
        self.assertFalse(ctx.pkg['open'])
        self.assertEqual(ctx.section, 1)

    def test_resident_error_is_not_a_pass(self):
        ctx = Fake()
        ctx.command_error = 'settings.patch'
        with self.assertRaisesRegex(RecipeFailed, 'injected'):
            next(r for r in RECIPES if r.interactions == ('settings.notifications',)).function(ctx)

    def test_binding_configuration_is_not_desktop_proof(self):
        ctx = Fake()
        ctx.available = False
        with self.assertRaisesRegex(RecipeBlocked, 'no global shortcut channel'):
            lane.shortcut_recording(ctx)
        self.assertIsNone(ctx.recording_action)
        self.assertEqual(ctx.document['actions']['region']['shortcut'], '')

    def test_live_s3_is_blocked_on_boat(self):
        ctx = Fake()
        ctx.destination = 's3'
        with self.assertRaisesRegex(RecipeBlocked, 'live S3 is forbidden'):
            lane.package_upload(ctx)
        self.assertFalse(ctx.inputs)

    def test_non_loopback_fixture_refused(self):
        ctx = Fake()
        ctx.fixtures['diagnostics-upload']['loopback'] = False
        with self.assertRaisesRegex(RecipeFailed, 'loopback S3'):
            lane.package_upload(ctx)

    def test_private_profile_checked_before_first_input(self):
        ctx = Fake()
        ctx.shell = lambda *a, **k: {'returncode': 1, 'stdout': ''}
        with self.assertRaisesRegex(RecipeBlocked, 'refusing the normal user profile'):
            lane.appearance(ctx)
        self.assertEqual(ctx.inputs, [])

    def test_cinnamon_fragment_keeps_layout_policy_out_of_recipes(self):
        from tools.lab.surface_driver.profiles import load_profile
        profile = load_profile('mint-cinnamon-x11')
        self.assertEqual(profile['window_titles']['diagnostics'], '^Create diagnostics package$')
        self.assertEqual(profile['catalog_windows']['settings'], list(lane.REFERENCES))
        self.assertIn('1440 960', profile['settings']['prepare_script'])
        self.assertNotIn('mint-cinnamon', Path(lane.__file__).read_text())

    def test_actual_private_config_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            folder = root/'putgd'
            folder.mkdir(mode=0o700)
            config = folder/'config.json'
            config.write_text('{}')
            env = dict(os.environ, XDG_CONFIG_HOME=directory)
            run = lambda: subprocess.run([sys.executable, '-c', lane.PRIVATE_CONFIG], env=env,
                                         text=True, capture_output=True)
            self.assertEqual(run().returncode, 0)
            folder.chmod(0o755)
            self.assertNotEqual(run().returncode, 0)
            folder.chmod(0o700)
            config.unlink()
            external = root/'external.json'
            external.write_text('{}')
            config.symlink_to(external)
            self.assertNotEqual(run().returncode, 0)

    def test_metadata_path_must_match_shell(self):
        ctx = Fake()
        ctx.shell = lambda *a, **k: {'returncode': 0, 'stdout': json.dumps({'path': '/other/config.json', 'document': {}})}
        with self.assertRaisesRegex(RecipeFailed, 'same private configuration'):
            lane.persisted(ctx)


if __name__ == '__main__':
    unittest.main()
