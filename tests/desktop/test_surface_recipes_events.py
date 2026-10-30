#!/usr/bin/env python3
"""Recipe contracts only. Fake Context is never graphical acceptance evidence."""
import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Context, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import events


class Fake(Context):
    placement = 'guest'
    host = 'boat'
    desktop = 'mint-cinnamon-x11'
    destination = 's3'
    profile = {'events': {'clipboardWrite': 'xclip -selection clipboard'}}

    def __init__(self):
        self.actions = []
        self.fixtures = {}
        self.status = {'build': {'supported': True}, 'channel': 'stable', 'checking': False,
                       'attemptedAt': None, 'result': {}, 'notifies': True}
        self.op = {'id': 'op-1', 'items': [{'uploadStatus': 'uploading'}], 'state': 'uploading'}
        self.epoch = 'same-resident'
        self.pid = 42
        self.text = ''
        self.notices = []
        self.shell_fail = None
        self.shell_hook = None
        self.drop_packets = 3
        self.settings = {'document': {'preferences': {'launchAtLogin': False}, 'actions': {'clipboard': {'shortcut': 'Ctrl+Alt+C'}}},
                         'metadata': {'path': '/private/config/putgd/config.json',
                                      'observedRevision': 'old', 'acceptedRevision': 'old', 'validation': {'ok': True}}}

    def call(self, method, params=None):
        self.actions.append(('call', method, params))
        if method == 'ui.snapshot':
            return {'residentEpoch': self.epoch, 'operationsReady': True,
                    'diagnostics': {'shortcuts': {'bindings': {'clipboard': {'requested': self.settings['document']['actions']['clipboard']['shortcut'], 'bound': True}}}, 'ui': {'pid': self.pid}, 'loginStartup': {'enabled': self.settings['document']['preferences']['launchAtLogin']}}}
        if method == 'settings.get': return copy.deepcopy(self.settings)
        if method == 'settings.patch':
            
            for key, value in params['patch'].items():
                self.settings['document'].setdefault(key, {}).update(value)
            return {}
        if method == 'operation.inspect': return copy.deepcopy(self.op)
        if method == 'update.status': return copy.deepcopy(self.status)
        if method == 'update.check':
            self.status.update(attemptedAt='new', result={'latest': '99.1231.9999', 'updateAvailable': True})
            return copy.deepcopy(self.status)
        if method == 'ui.viewState': return {}
        raise AssertionError('unexpected resident method: ' + method)

    def view(self, method, params=None):
        self.actions.append(('view', method, params))
        if self.pid == 0: self.pid = 43
        return {}

    def shell(self, script, timeout=60):
        self.actions.append(('shell', script))
        if self.shell_hook: return self.shell_hook(script)
        if self.shell_fail and self.shell_fail in script:
            return {'returncode': 1, 'stdout': '', 'stderr': 'fixture failure'}
        if 'PYCONFIG' in script:
            self.settings['document']['preferences']['launchAtLogin'] = True
            self.settings['document']['actions']['clipboard']['shortcut'] = 'Ctrl+Alt+Shift+F12'
            self.settings['metadata'].update(observedRevision='new', acceptedRevision='new')
        if 'PYKILL' in script:
            self.pid = 0
            self.notices.append({'summary': 'Window closed unexpectedly'})
        if ' | xclip' in script or ' | wl-copy' in script:
            self.text = 'putgd-events-new-clipboard-owner'
            self.op.update(copyStatus='suppressed', state='completed', items=[{'uploadStatus': 'uploaded'}])
            self.notices.append({'summary': 'Clipboard changed'})
        if 'sudo -n nft -f' in script:
            self.op.update(state='failed', items=[{'uploadStatus': 'failed', 'error': {'code': 'upload_timeout'}}])
            self.notices.append({'summary': 'Upload failed'})
        stdout = json.dumps({'nftables': [{'rule': {'expr': [{'counter': {'packets': self.drop_packets}}]}}]}) if 'nft -j' in script else ''
        return {'returncode': 0, 'stdout': stdout, 'stderr': ''}

    def wait(self, predicate, what, timeout=10, interval=.2):
        result = predicate()
        self.expect(result, what)
        return result

    def geometry(self, window): return {'x': 0, 'y': 0, 'width': 1280, 'height': 800}
    def frame(self, label, window=None, reference=None): self.actions.append(('frame', label, window))
    def click(self, control, window, **kw):
        self.actions.append(('click', control, window))
        if control == 'checkForUpdates': self.call('update.check')
    def fixture(self, name):
        if name == 'events.controllers':
            def stall(address, port):
                self.expect(address == '127.0.0.1', 'only loopback fixture endpoint')
                self.actions.append(('controller', 'network_stall', address, port))
                self.op.update(state='failed', items=[{'uploadStatus': 'failed', 'error': {'code': 'upload_timeout'}}])
                self.notices.append({'summary': 'Upload failed'})
                return 'sd_owned_table'
            return {'network_stall': stall}
        if name == 'events.clipboard':
            self.text = 'putgd synthetic clipboard'
            self.op.update(copyStatus='suppressed', state='completed', items=[{'uploadStatus': 'uploaded'}])
            self.notices.append({'summary': 'Clipboard changed'})
            self.actions.append(('fixture', name))
            return {'text': self.text}
        return self.fixtures[name]
    def notifications(self): return self.notices
    def clipboard_text(self): return self.text
    def note(self, text): self.actions.append(('note', text))
    def review(self, reason): self.actions.append(('review', reason))
    def cli(self, *args, **kw):
        reply = self.call(args[0], json.loads(args[1]) if len(args) > 1 else None)
        envelope = reply if 'ok' in reply else {'ok': True, 'result': reply}
        return {'returncode': 0 if envelope['ok'] else 1, 'stdout': json.dumps(envelope)}
    def mcp(self, method, arguments=None):
        self.actions.append(('mcp', method))
        return self.call('update.check' if method == 'check_for_updates' else 'update.status')

    def upload(self):
        self.fixtures['events.slow-upload'] = {'operationId': 'op-1', 'destination': 's3',
                                               'address': '127.0.0.1', 'port': 19447, 'timeoutSeconds': 30}
        return self

    def feed(self):
        self.fixtures['events.update-feed'] = {'url': 'http://127.0.0.1:19448/feed/', 'root': '/private/state/feed',
                                               'residentPid': 40, 'version': '99.1231.9999', 'channel': 'stable'}
        return self


class EventRecipes(unittest.TestCase):
    def test_window_mapping_retries_only_absent_windows(self):
        ctx = Fake()
        def absent(window): raise RecipeBlocked('live window match is missing or ambiguous (0 matches)')
        ctx.geometry = absent
        self.assertIsNone(events._window_ready(ctx, 'history'))
        def ambiguous(window): raise RecipeBlocked('live window match is missing or ambiguous (2 matches)')
        ctx.geometry = ambiguous
        with self.assertRaises(RecipeBlocked): events._window_ready(ctx, 'history')

    def test_exact_ownership_once(self):
        owners = json.loads((ROOT / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        actual = [i for r in REGISTRY if r.function.__module__ == events.__name__ for i in r.interactions]
        self.assertEqual(set(actual), {i for i, lane in owners.items() if lane == 'events'})
        self.assertEqual(len(actual), len(set(actual)))

    def test_blocked_declarations_are_truthful(self):
        for identity, reason in events.BLOCKED.items():
            ctx = Fake()
            function = next(r.function for r in REGISTRY if identity in r.interactions)
            with self.subTest(identity=identity), self.assertRaisesRegex(RecipeBlocked, reason.replace('(', r'\(').replace(')', r'\)')):
                function(ctx)
            self.assertEqual(ctx.actions, [])

    def test_missing_upload_blocks_before_mutation(self):
        ctx = Fake()
        with self.assertRaises(RecipeBlocked): events.network_stall(ctx)
        self.assertEqual(ctx.actions, [])

    def test_physical_disruption_blocked(self):
        for function in (events.config_changed, events.single_instance, events.ui_helper_crash, events.network_stall):
            ctx = Fake().upload()
            ctx.placement = 'native'
            with self.subTest(function=function.__name__), self.assertRaises(RecipeBlocked): function(ctx)
            self.assertEqual(ctx.actions, [])

    def test_single_instance_checks_epoch(self):
        ctx = Fake()
        events.single_instance(ctx)
        self.assertIn(('shell', 'putgd-launcher --background'), ctx.actions)
        snapshots = [a for a in ctx.actions if a[:2] == ('call', 'ui.snapshot')]
        self.assertEqual(len(snapshots), 2)

    def test_helper_kills_exact_pid_and_recovers(self):
        ctx = Fake()
        events.ui_helper_crash(ctx)
        script = next(a[1] for a in ctx.actions if a[0] == 'shell')
        self.assertIn('pid = 42', script)
        self.assertIn('os.pidfd_open', script)
        self.assertIn("name == 'putgd-ui'", script)
        self.assertIn('signal.SIGKILL', script)
        self.assertNotIn('pkill', script)
        self.assertEqual(ctx.pid, 43)
        self.assertEqual([a[1] for a in ctx.actions if a[0] == 'frame'], ['before', 'after-crash', 'recovered'])

    def test_config_uses_external_atomic_edit_and_restores(self):
        ctx = Fake()
        events.config_changed(ctx)
        script = next(a[1] for a in ctx.actions if a[0] == 'shell')
        self.assertIn('os.replace(name, p)', script)
        self.assertIn('os.O_EXCL', script)
        self.assertIn('os.fsync', script)
        self.assertFalse(ctx.settings['document']['preferences']['launchAtLogin'])
        self.assertFalse(any(a[:2] == ('call', 'settings.reload') for a in ctx.actions))

    def test_config_script_runs_on_private_temp_document(self):
        # Execute just the generated external-writer Python, not a UI/app driver.
        import os
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            p = root / 'config.json'
            p.write_text('{"preferences":{"launchAtLogin":false},"actions":{"clipboard":{"shortcut":"Ctrl+Alt+C"}},"preserve":17}')
            p.chmod(0o600)
            import hashlib
            ctx = Fake()
            ctx.settings['metadata'].update(path=str(p), observedRevision=hashlib.sha256(p.read_bytes()).hexdigest())
            captured = []
            def execute(script):
                captured.append(script)
                run = subprocess.run(['sh', '-c', script], env={**os.environ, 'XDG_CONFIG_HOME': directory}, capture_output=True, text=True)
                ctx.settings['document'] = json.loads(p.read_text())
                ctx.settings['metadata']['acceptedRevision'] = 'new'
                return {'returncode': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr}
            ctx.shell_hook = execute
            events.config_changed(ctx)
            self.assertEqual(json.loads(p.read_text())['preserve'], 17)
            self.assertFalse(pathlib.Path(str(p) + '.lock').exists())

    def test_nft_uses_owned_core_controller_and_packet_witness(self):
        ctx = Fake().upload()
        ctx.destination = 'local'
        ctx.fixtures['events.slow-upload']['destination'] = 'local'
        events.network_stall(ctx)
        self.assertIn(('controller', 'network_stall', '127.0.0.1', 19447), ctx.actions)
        scripts = [a[1] for a in ctx.actions if a[0] == 'shell']
        self.assertEqual(scripts, [
            'sudo -n /usr/sbin/nft add rule inet sd_owned_table output ip saddr 127.0.0.1 tcp sport 19447 counter drop',
            'sudo -n /usr/sbin/nft -j list table inet sd_owned_table'])
        self.assertTrue(any(a[0] == 'note' and 'cleanup registered' in a[1] for a in ctx.actions))

    def test_nft_requires_actual_dropped_packets(self):
        ctx = Fake().upload()
        ctx.drop_packets = 0
        with self.assertRaisesRegex(RecipeFailed, 'actually dropped'):
            events.network_stall(ctx)
        self.assertTrue(any(a[0] == 'note' and 'cleanup registered' in a[1] for a in ctx.actions))

    def test_boat_live_destinations_block(self):
        for address, destination in [('192.0.2.1', 's3'), ('127.0.0.1', 'proton')]:
            ctx = Fake().upload()
            ctx.destination = destination
            ctx.fixtures['events.slow-upload'].update(address=address, destination=destination)
            with self.assertRaises(RecipeBlocked): events.network_stall(ctx)
            self.assertFalse(any(a[0] == 'shell' for a in ctx.actions))

    def test_completed_upload_cannot_prove_event(self):
        ctx = Fake().upload()
        ctx.op['items'][0]['uploadStatus'] = 'uploaded'
        with self.assertRaisesRegex(RecipeFailed, 'still in flight'): events.clipboard_changed(ctx)

    def test_clipboard_is_real_tool_and_suppression_asserted(self):
        ctx = Fake().upload()
        events.clipboard_changed(ctx)
        self.assertIn(('fixture', 'events.clipboard'), ctx.actions)
        self.assertEqual(ctx.text, 'putgd synthetic clipboard')


    def test_feed_guard_precedes_any_check(self):
        ctx = Fake().feed()
        events.update_found(ctx)
        shell = next(a[1] for a in ctx.actions if a[0] == 'shell')
        self.assertIn("env[b'PUTGD_UPDATE_FEED_URL']", shell)
        self.assertIn("== 'http://127.0.0.1:19448/feed/'", shell)
        guard_index = next(i for i, a in enumerate(ctx.actions) if a[0] == 'shell')
        check_index = next(i for i, a in enumerate(ctx.actions) if a[:2] == ('call', 'update.check'))
        self.assertLess(guard_index, check_index)

    def test_feed_guard_failure_never_checks_real_feed(self):
        ctx = Fake().feed()
        ctx.shell_fail = 'PYFEED'
        with self.assertRaises(RecipeFailed): events.update_found(ctx)
        self.assertFalse(any(a[:2] == ('call', 'update.check') for a in ctx.actions))

    def test_nonlocal_feed_rejected(self):
        ctx = Fake().feed()
        ctx.fixtures['events.update-feed']['url'] = 'https://app.put.gd/update/v1/'
        with self.assertRaises(RecipeFailed): events.update_found(ctx)
        self.assertEqual(ctx.actions, [])

    def test_untagged_package_blocks(self):
        ctx = Fake().feed()
        ctx.status['build'] = {'supported': False, 'reason': 'development_build'}
        with self.assertRaisesRegex(RecipeBlocked, 'development_build'): events.update_found(ctx)
        self.assertFalse(any(a[:2] == ('call', 'update.check') for a in ctx.actions))

    def test_menu_uses_input(self):
        ctx = Fake().feed()
        events.app_menu_check_updates(ctx)
        self.assertEqual([a[1] for a in ctx.actions if a[0] == 'click'], ['applicationMenu', 'checkForUpdates'])

    def test_partial_cli_and_mcp_are_not_accepted_as_complete(self):
        for function in (events.cli_update, events.mcp_updates):
            with self.subTest(function=function.__name__), self.assertRaises(RecipeBlocked): function(Fake().feed())

    def test_disk_record_fills_real_mount_and_cleans_on_error(self):
        ctx = Fake()
        ctx.settings['effectiveLimits'] = {'disk': {'availableBytes': 512 * 1024**2}}
        ctx.fixtures['events.record-storage'] = {'mountPath': '/private/data/volume'}
        original_call = ctx.call
        def call(method, params=None):
            if method == 'recording.prepare':
                self.assertEqual(params['target']['rect'], [0, 0, 320, 240])
                return {'ok': False, 'error': {'code': 'disk_space_insufficient', 'details': {'freeBytes': 100, 'reserveBytes': 1024, 'requiredBytes': 500}}}
            return original_call(method, params)
        ctx.call = call
        events.disk_full_record(ctx)
        scripts = [a[1] for a in ctx.actions if a[0] == 'shell']
        self.assertIn("mount[0]['fstype'] == 'tmpfs'", scripts[0])
        self.assertIn('os.write(fd, block)', scripts[0])
        self.assertIn('os.O_EXCL', scripts[0])
        self.assertIn('p.unlink()', scripts[-1])

    def test_disk_failure_cleans_and_wrong_error_is_not_space_proof(self):
        ctx = Fake()
        ctx.settings['effectiveLimits'] = {'disk': {'availableBytes': 512 * 1024**2}}
        ctx.fixtures['events.record-storage'] = {'mountPath': '/private/data/volume'}
        original_call = ctx.call
        ctx.call = lambda method, params=None: ({'ok': False, 'error': {'code': 'invalid_request'}}
                                                if method == 'recording.prepare' else original_call(method, params))
        with self.assertRaisesRegex(RecipeFailed, 'disk admission'):
            events.disk_full_record(ctx)
        self.assertIn('p.unlink()', ctx.actions[-1][1])

    def test_disk_already_full_cannot_prove_new_event(self):
        ctx = Fake()
        ctx.settings['effectiveLimits'] = {'disk': {'availableBytes': 0}}
        ctx.fixtures['events.record-storage'] = {'mountPath': '/private/data/volume'}
        with self.assertRaisesRegex(RecipeFailed, 'before filling'):
            events.disk_full_record(ctx)
        self.assertFalse(any(a[0] == 'shell' for a in ctx.actions))

    def test_panel_theme_restores_on_missing_response(self):
        ctx = Fake()
        ctx.profile = {'events': {'panelTheme': {'read': ['gsettings', 'get', 'schema', 'key'], 'write': ['gsettings', 'set', 'schema', 'key'],
                                                'light': 'Synthetic Light', 'dark': 'Synthetic Dark'}}}
        original_call = ctx.call
        def call(method, params=None):
            result = original_call(method, params)
            if method == 'ui.snapshot':
                result['diagnostics']['trayTint'] = {'panel': 'dark', 'source': 'cinnamon-theme', 'color': '#fff'}
            return result
        ctx.call = call
        scripts = []
        theme = ['Original Theme']
        def shell(script):
            import shlex
            scripts.append(script)
            if ' set ' in script: theme[0] = shlex.split(script)[-1]
            return {'returncode': 0, 'stdout': repr(theme[0]) if ' get ' in script else '', 'stderr': ''}
        ctx.shell_hook = shell
        with self.assertRaisesRegex(RecipeFailed, 'tone change'):
            events.panel_theme_changed(ctx)
        self.assertIn("'Original Theme'", scripts[-2])
        self.assertIn("'Synthetic Light'", scripts[1])
        self.assertTrue(any(a[:2] == ('frame', 'failure-before-cleanup') for a in ctx.actions))

    def _theme_ctx(self, follows):
        """A resident whose tray follows the theme only through the shared dconf link (#702)."""
        import shlex
        ctx = Fake()
        ctx.profile = {'events': {'panelTheme': {'read': ['gsettings', 'get', 'schema', 'key'], 'write': ['gsettings', 'set', 'schema', 'key'],
                                                'light': 'Synthetic Light', 'dark': 'Synthetic Dark', 'sessionDconf': '.config/dconf'}}}
        state = {'theme': 'Synthetic Dark', 'linked': False}
        original_call = ctx.call
        def call(method, params=None):
            result = original_call(method, params)
            if method == 'ui.snapshot':
                light = follows and state['linked'] and 'Light' in state['theme']
                result['diagnostics']['trayTint'] = {'panel': 'light' if light else 'dark', 'source': 'cinnamon-theme', 'color': '#383834' if light else '#d6d6d2'}
            return result
        ctx.call = call
        scripts = []
        def shell(script):
            scripts.append(script)
            if 'ln -s' in script: state['linked'] = True
            if 'rm "$link"' in script and 'ln -s' not in script: state['linked'] = False
            if ' set ' in script: state['theme'] = shlex.split(script)[-1]
            return {'returncode': 0, 'stdout': repr(state['theme']) if ' get ' in script else '', 'stderr': ''}
        ctx.shell_hook = shell
        return ctx, scripts, state

    def test_panel_theme_shares_session_dconf_only_for_the_event(self):
        ctx, scripts, state = self._theme_ctx(follows=True)
        events.panel_theme_changed(ctx)
        self.assertIn('ln -sT "$HOME"/.config/dconf "$link"', scripts[0])
        self.assertIn('link="$XDG_CONFIG_HOME/dconf"', scripts[0])
        self.assertIn("'Synthetic Light'", scripts[2])
        self.assertIn("'Synthetic Dark'", scripts[-3])
        self.assertIn('rm "$link"', scripts[-1])
        self.assertFalse(state['linked'])
        self.assertTrue(any(a[:2] == ('frame', 'after') for a in ctx.actions))

    def test_panel_theme_unlinks_session_dconf_after_a_miss(self):
        ctx, scripts, state = self._theme_ctx(follows=False)
        with self.assertRaisesRegex(RecipeFailed, 'tone change'):
            events.panel_theme_changed(ctx)
        self.assertIn("'Synthetic Dark'", scripts[-3])
        self.assertIn('rm "$link"', scripts[-1])
        self.assertFalse(state['linked'])

    def test_panel_theme_stops_when_session_dconf_is_missing(self):
        ctx, scripts, state = self._theme_ctx(follows=True)
        hook = ctx.shell_hook
        def shell(script):
            if 'ln -s' in script:
                scripts.append(script)
                return {'returncode': 1, 'stdout': '', 'stderr': 'no session dconf directory'}
            return hook(script)
        ctx.shell_hook = shell
        with self.assertRaisesRegex(RecipeFailed, 'share the session dconf'):
            events.panel_theme_changed(ctx)
        # Every precondition is its own command, so set -e stops before ln.
        lines = scripts[0].splitlines()
        self.assertLess(lines.index('test -d "$HOME"/.config/dconf'), lines.index('ln -sT "$HOME"/.config/dconf "$link"'))
        self.assertFalse(any('&&' in line for line in lines))
        self.assertEqual(len(scripts), 1)
        self.assertFalse(state['linked'])

    def test_panel_theme_refuses_dconf_path_outside_home(self):
        for path in ('/home/lab/.config/dconf', '../other/dconf', '.config/../../dconf', '', '.config//dconf'):
            ctx, scripts, _ = self._theme_ctx(follows=True)
            ctx.profile['events']['panelTheme']['sessionDconf'] = path
            with self.assertRaisesRegex(RecipeFailed, 'relative to the home'):
                events.panel_theme_changed(ctx)
            self.assertEqual(scripts, [])

    def test_dconf_guard_removes_link_when_post_link_verification_fails(self):
        """Execute the actual target script and force a failure after ln succeeds."""
        import os
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            home, config = root / 'home', root / 'private'
            (home / '.config/dconf').mkdir(parents=True)
            config.mkdir()
            ctx = Fake()
            scripts = []
            ctx.shell_hook = lambda script: (scripts.append(script) or {'returncode': 0, 'stdout': ''})
            events._share_session_dconf(ctx, '.config/dconf')
            script = scripts[0].replace('test -d "$link"', 'false # forced verification failure')
            result = subprocess.run(['sh', '-c', script], env={**os.environ, 'HOME': str(home), 'XDG_CONFIG_HOME': str(config)},
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((config / 'dconf').is_symlink())
            self.assertEqual(list(config.iterdir()), [])
            self.assertTrue((home / '.config/dconf').is_dir())

    def test_theme_baseline_waits_for_shared_dconf_read(self):
        ctx, scripts, state = self._theme_ctx(follows=True)
        state['theme'] = 'Synthetic Light'
        real_call, real_wait = ctx.call, ctx.wait
        observed = []
        stale = [True]
        def call(method, params=None):
            result = real_call(method, params)
            if method == 'ui.snapshot' and stale[0]:
                result['diagnostics']['trayTint'] = {'panel': 'dark', 'source': 'default', 'color': '#d6d6d2'}
            return result
        def wait(predicate, what, **kwargs):
            if 'before baseline' in what:
                observed.append(predicate())
                stale[0] = False
            return real_wait(predicate, what, **kwargs)
        ctx.call, ctx.wait = call, wait
        events.panel_theme_changed(ctx)
        self.assertEqual(observed, [None])
        self.assertIn("'Synthetic Dark'", scripts[2])
        self.assertEqual(state['theme'], 'Synthetic Light')
        self.assertFalse(state['linked'])

    def test_cinnamon_profile_shares_session_dconf(self):
        from tools.lab.surface_driver.profiles import load_profile
        theme = load_profile('mint-cinnamon-x11')['events']['panelTheme']
        self.assertEqual(theme['sessionDconf'], '.config/dconf')
        self.assertIn('#702', theme['sessionDconfNote'])

    def test_budget_stop_does_not_send_user_stop_or_upload(self):
        ctx = Fake()
        ctx.settings['document']['limits'] = {'maxFileBytes': 'auto'}
        ctx.fixtures['events.recording-source'] = {'synthetic': True, 'prepare': {'target': {'kind': 'region', 'rect': [0,0,640,480]}, 'format': 'mp4'}}
        ctx.notices = [{'summary': 'Recording stopped'}]
        original_call = ctx.call
        state = ['ready', 'reviewing']
        methods = []
        def call(method, params=None):
            methods.append(method)
            if method == 'recording.prepare': return {'recordingId': 'op-1'}
            if method == 'recording.inspect': return {'state': state.pop(0), 'stopReason': 'budget', 'stopDetails': {'reason': 'max_file_bytes'}, 'durationUs': 1000000}
            if method in ('recording.start', 'recording.review', 'recording.cancel'): return {}
            if method == 'operation.inspect': return {'id': 'op-1', 'items': []}
            return original_call(method, params)
        ctx.call = call
        ctx.note = lambda text: None
        events.recording_budget_stop(ctx)
        self.assertNotIn('recording.stop', methods)
        self.assertNotIn('recording.upload', methods)
        self.assertIn('recording.cancel', methods)
        self.assertEqual(ctx.settings['document']['limits']['maxFileBytes'], 'auto')

    def test_multipart_checks_exact_item_not_any_failure(self):
        ctx = Fake()
        ctx.fixtures['events.incomplete-multipart'] = {'address': '127.0.0.1', 'operationId': 'op-1', 'itemId': 'item-1'}
        ctx.op['items'] = [{'artifact': {'id': 'item-1'}}]
        original_call = ctx.call
        ctx.call = lambda method, params=None: ({'incompleteUploads': [{'operationId': 'OTHER', 'itemId': 'item-1', 'error': {'code': 'denied'}, 'reported': True}]}
                                                if method == 'cache.clear' else original_call(method, params))
        with self.assertRaisesRegex(RecipeFailed, 'exact item'):
            events.incomplete_uploads_kept(ctx)


if __name__ == '__main__':
    unittest.main()
