#!/usr/bin/env python3
"""Lightweight contracts; these do not establish graphical acceptance."""
import base64
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import subprocess
import socket
import threading
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import browser
from tools.lab.surface_driver.api import Context, Frame, RecipeBlocked, RecipeFailed, REGISTRY
from tools.lab.surface_driver.recipes import links

# /proc/PID/stat with a command name containing a space and ')' and starttime 7.
FAKE_STAT = '999999999 (fire fox)) S ' + ' '.join(['0'] * 18) + ' 7 0'


class FakeContext(Context):
    profile = {}
    placement = 'guest'
    destination = 's3'

    def __init__(self):
        self.events = []
        self.link = 'http://127.0.0.1/private-token/fixture.txt'
        self.upload = {'history_id': 'history-1', 'name': 'fixture.txt', 'link': self.link,
                       'expect': {'kind': 's3', 'name': 'fixture.txt', 'preview': True}}

    def fixture(self, name):
        self.events.append(('fixture', name))
        return {'expect': {'kind': 'local', 'name': 'put.gd', 'preview': False}} if name == 'release' else self.upload

    def view(self, method, params=None):
        self.events.append(('view', method, params))
        return {}

    def click(self, control, window, **kwargs):
        self.events.append(('click', control, window))

    def keys(self, *keys):
        self.events.append(('keys', keys))

    def click_at(self, x, y, **kwargs):
        self.events.append(('click_at', x, y))

    def clipboard_text(self):
        return self.link

    def notifications(self):
        return [{'body': 'Uploaded. Link copied to the clipboard. Click to open in browser.'}]

    def accessible(self, query):
        return [{'bounds': {'x': 10, 'y': 20, 'width': 100, 'height': 40}}]

    def mcp(self, tool, arguments=None):
        self.events.append(('mcp', tool, arguments))
        return {}

    def wait(self, predicate, what, **kwargs):
        result = predicate()
        self.expect(result, what)
        return result

    def note(self, text):
        self.events.append(('note', text))

    def review(self, reason):
        self.events.append(('review', reason))

    def frame(self, label, window=None, reference=None):
        self.events.append(('frame', label, window))
        return Frame(label, label + '.png', window)


class BrowserContracts(unittest.TestCase):
    def setUp(self):
        self.context = FakeContext()

    def remote(self, ctx, action, values):
        ctx.events.append(('remote', action))
        if action == 'prepare':
            return {'directory': '/private/firefox', 'port': 1234}
        if action == 'restore':
            return {'restored': True}
        if action == 'observe':
            return {'url_host': '127.0.0.1', 'title': 'fixture.txt', 'matched': True}
        return {'blank': True, 'safe': True}

    def test_content_receipt_and_full_desktop(self):
        with patch.object(browser, '_remote', self.remote):
            with browser.default_browser(self.context, 'firefox'):
                receipt = browser.open_and_verify(self.context, lambda: self.context.events.append(('trigger',)),
                                                  self.context.upload['expect'])
        self.assertEqual(set(receipt), {'url_host', 'title', 'matched', 'frames'})
        json.dumps(receipt)  # public helper result is directly serializable
        self.assertNotIn('private-token', repr(receipt) + repr(self.context.events))
        self.assertIn(('frame', 'browser-content', None), self.context.events)
        self.assertLess(self.context.events.index(('remote', 'blank')), self.context.events.index(('trigger',)))
        self.assertIn(('remote', 'observe'), self.context.events)
        self.assertIn(('remote', 'restore'), self.context.events)
        self.assertNotIn(id(self.context), browser._ACTIVE)

    def test_trigger_exception_restores_and_saves_safe_failure(self):
        def fail():
            raise RecipeFailed('test failure')
        with patch.object(browser, '_remote', self.remote):
            with self.assertRaises(RecipeFailed):
                with browser.default_browser(self.context):
                    browser.open_and_verify(self.context, fail, self.context.upload['expect'])
        self.assertEqual(sum(event == ('remote', 'blank') for event in self.context.events), 2)
        self.assertIn(('frame', 'browser-failure-safe-desktop', None), self.context.events)
        self.assertIn(('remote', 'restore'), self.context.events)

    def test_failure_frame_withheld_when_another_browser_owns_foreground(self):
        def remote(ctx, action, values):
            if action == 'blank':
                return {'blank': True, 'safe': False}
            return self.remote(ctx, action, values)
        def fail():
            self.context.events.append(('trigger',))
            raise RecipeFailed('test failure')
        with patch.object(browser, '_remote', remote):
            with self.assertRaises(RecipeFailed):
                with browser.default_browser(self.context):
                    browser.open_and_verify(self.context, fail, self.context.upload['expect'])
        self.assertNotIn(('trigger',), self.context.events)
        self.assertFalse(any(event[0] == 'frame' for event in self.context.events))
        self.assertTrue(any(event[0] == 'review' for event in self.context.events))

    def test_dispatch_to_another_firefox_fails_closed_without_frames(self):
        seen = []
        def remote(ctx, action, values):
            if action == 'observe':
                seen.append(action)
                return {'matched': False, 'foreign': 1}
            if action == 'blank' and seen:
                return {'blank': True, 'safe': False}
            return self.remote(ctx, action, values)
        with patch.object(browser, '_remote', remote):
            with self.assertRaisesRegex(RecipeFailed, 'external dispatch reached the private Marionette Firefox'):
                with browser.default_browser(self.context):
                    browser.open_and_verify(self.context, lambda: None, self.context.upload['expect'])
        self.assertFalse(any(event[0] == 'frame' for event in self.context.events))
        self.assertTrue(any(event[0] == 'review' for event in self.context.events))
        self.assertNotIn(id(self.context), browser._ACTIVE)

    def test_stale_or_mismatched_page_cannot_pass(self):
        def remote(ctx, action, values):
            return {} if action == 'observe' else self.remote(ctx, action, values)
        with patch.object(browser, '_remote', remote):
            with self.assertRaises(RecipeFailed):
                with browser.default_browser(self.context):
                    browser.open_and_verify(self.context, lambda: None, self.context.upload['expect'])
        self.assertNotIn(('frame', 'browser-content', None), self.context.events)

    def test_requires_transaction_and_valid_expectation(self):
        with self.assertRaises(RuntimeError):
            browser.open_and_verify(self.context, lambda: None, self.context.upload['expect'])
        with patch.object(browser, '_remote', self.remote):
            with browser.default_browser(self.context):
                with self.assertRaises(ValueError):
                    browser.open_and_verify(self.context, lambda: None, {'kind': 's3'})


class ProtocolContracts(unittest.TestCase):
    def test_framed_real_protocol_envelopes_and_error_redaction(self):
        server = socket.socket()
        server.bind(('127.0.0.1', 0))
        server.listen(1)
        failures = []
        def serve():
            try:
                conn, _ = server.accept()
                with conn, conn.makefile('rb') as stream:
                    def send(value):
                        data = json.dumps(value).encode()
                        conn.sendall(str(len(data)).encode() + b':' + data)
                    send({'marionetteProtocol': 3, 'applicationType': 'gecko'})
                    for i in range(1, 5):
                        header = b''
                        while True:
                            char = stream.read(1)
                            if char == b':':
                                break
                            if not char:
                                raise AssertionError('client disconnected')
                            header += char
                        command = json.loads(stream.read(int(header)))
                        self.assertEqual(command[:2], [0, i])
                        if i == 1:
                            send([1, i, None, {'sessionId': 'fake', 'capabilities': {}}])
                        elif i == 2:
                            self.assertEqual(command[2], 'WebDriver:GetWindowHandles')
                            send([1, i, None, {'value': ['tab-1', 'tab-2']}])
                        elif i == 3:
                            send([1, i, {'message': 'https://example.invalid/SECRET-TOKEN'}, None])
                        else:
                            send([1, i, None, {'value': None}])
            except BaseException as error:
                failures.append(error)
            finally:
                server.close()
        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        client = browser._Marionette(server.getsockname()[1])
        try:
            self.assertEqual(client.call('WebDriver:GetWindowHandles'), ['tab-1', 'tab-2'])
            with self.assertRaisesRegex(RuntimeError, '^Firefox protocol command failed$'):
                client.call('WebDriver:ExecuteScript')
        finally:
            client.close()
        thread.join(timeout=5)
        self.assertFalse(thread.is_alive())
        if failures:
            raise failures[0]


class ObservationContracts(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        (self.directory / 'transaction.json').write_text(json.dumps(
            {'directory': str(self.directory), 'since': 100, 'foreign': []}))
        self.values = {'directory': str(self.directory), 'port': 1234}
        mains = patch.object(browser, '_firefox_mains', return_value=[])
        self.mains = mains.start()
        self.addCleanup(mains.stop)

    def test_closed_tab_does_not_hide_a_later_matching_page(self):
        client = SimpleNamespace()
        def call(method, params=None):
            if method == 'WebDriver:GetWindowHandles':
                return ['closed', 'good']
            if params['handle'] == 'closed':
                raise browser._TargetError('Firefox protocol command failed')
        client.call = call
        client.script = lambda *args: {'matched': True, 'url_host': '127.0.0.1', 'title': 'fixture.txt'}
        client.close = lambda: None
        with patch.object(browser, '_Marionette', return_value=client):
            result = browser._target('observe', {**self.values, 'expect': {'name': 'fixture.txt'}})
        self.assertTrue(result['matched'])

    def test_non_mapping_browser_reply_does_not_pass(self):
        client = SimpleNamespace(call=lambda *args: ['good'] if args[0].endswith('GetWindowHandles') else None,
                                 script=lambda *args: None, close=lambda: None)
        with patch.object(browser, '_Marionette', return_value=client):
            self.assertEqual(browser._target('observe', {**self.values, 'expect': {}}), {})

    def test_dispatch_to_firefox_outside_private_profile_is_named_and_recorded(self):
        # #693 root cause: an unguarded MIME profile launched the stock
        # firefox.desktop, a separate default-profile instance Marionette
        # could not see. Pre-existing and own-profile parents are not foreign.
        self.mains.return_value = [
            (10, '50', [b'/usr/lib/firefox/firefox', b'about:blank']),
            (11, '150', [b'/usr/lib/firefox/firefox', b'http://127.0.0.1/private-token/fixture.txt']),
            (12, '160', [b'/usr/lib/firefox/firefox', b'--profile', str(self.directory).encode(), b'--kiosk'])]
        client = SimpleNamespace(call=lambda *args: [] if args[0].endswith('GetWindowHandles') else None,
                                 script=lambda *args: True, close=lambda: None)
        with patch.object(browser, '_Marionette', return_value=client):
            result = browser._target('observe', {**self.values, 'expect': {'name': 'fixture.txt'}})
            self.assertEqual(result, {'matched': False, 'foreign': 1})
            self.assertNotIn('private-token', json.dumps(result))
            stored = json.loads((self.directory / 'transaction.json').read_text())
            self.assertEqual(stored['foreign'], [[11, '150']])
            # Its window may show the link: the failure frame is not safe.
            self.assertFalse(browser._target('blank', self.values)['safe'])

    def test_restore_stops_exact_owned_and_foreign_identities_only(self):
        backups = []
        state = {'directory': str(self.directory), 'desktops': [], 'backups': backups,
                 'pid': 20, 'pid_start': '120', 'since': 100, 'foreign': [[11, '150']]}
        (self.directory / 'transaction.json').write_text(json.dumps(state))
        self.mains.return_value = [
            (10, '50', [b'/usr/lib/firefox/firefox']),
            (12, '160', [b'/usr/lib/firefox/firefox', b'--profile', str(self.directory).encode(), b'--marionette'])]
        with patch.object(browser, '_terminate') as terminate:
            self.assertTrue(browser._restore({'directory': str(self.directory)})['restored'])
        self.assertEqual([c.args for c in terminate.call_args_list], [(20, '120'), (12, '160'), (11, '150')])
        self.assertFalse(self.directory.exists())


class DispatchFixes(unittest.TestCase):
    def test_handler_starts_an_attachable_instance_and_pins_new_tabs(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            process = SimpleNamespace(pid=999999999, poll=lambda: None)
            real_read = Path.read_text
            def read(path, *args, **kwargs):
                return FAKE_STAT if str(path) == '/proc/999999999/stat' else real_read(path, *args, **kwargs)
            def mime(args, **kwargs):
                data = Path(kwargs['env']['XDG_DATA_HOME'])
                desktop = next((data / 'applications').glob('putgdsurface*.desktop'))
                return subprocess.CompletedProcess(args, 0, desktop.name + '\n', '')
            with patch.dict(browser.os.environ, {'XDG_CONFIG_HOME': str(home / 'isolated/config'),
                                                 'XDG_DATA_HOME': str(home / 'isolated/data')}), \
                 patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))), \
                 patch.object(browser.subprocess, 'Popen', return_value=process), \
                 patch.object(browser, '_Marionette'), \
                 patch.object(browser, '_firefox_mains', return_value=[]), \
                 patch.object(browser, '_terminate'), \
                 patch.object(browser.subprocess, 'run', side_effect=mime), \
                 patch.object(Path, 'read_text', read):
                state = browser._prepare({'command': ['snap', 'run', 'firefox']})
                directory = Path(state['directory'])
                entry = next((home / '.local/share/applications').glob('putgdsurface*.desktop')).read_text()
                self.assertIn('"--profile" "%s" "--kiosk" "--marionette" %%u' % directory, entry)
                prefs = (directory / 'user.js').read_text()
                self.assertIn('user_pref("browser.link.open_newwindow.override.external", 3);', prefs)
                self.assertIn('user_pref("datareporting.policy.firstRunURL", "");', prefs)
                transaction = json.loads((directory / 'transaction.json').read_text())
                self.assertEqual((transaction['pid_start'], transaction['foreign']), ('7', []))
                self.assertIsInstance(transaction['since'], int)
                browser._restore(state)

    def test_trigger_failure_after_real_setup_restores_mime_bytes(self):
        # The whole failure path with the real target code: prepare, blank,
        # a failing trigger, the safe failure blank and restore.
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            configs = [home / '.config', home / 'isolated/config']
            originals = [b'[Default Applications]\r\nx-scheme-handler/https=recorder.desktop;\r\n', b'# isolated\n']
            for config, original in zip(configs, originals):
                config.mkdir(parents=True)
                (config / 'mimeapps.list').write_bytes(original)
                (config / 'mimeapps.list').chmod(0o640)
            process = SimpleNamespace(pid=999999999, poll=lambda: None)
            real_read = Path.read_text
            def read(path, *args, **kwargs):
                return FAKE_STAT if str(path) == '/proc/999999999/stat' else real_read(path, *args, **kwargs)
            def mime(args, **kwargs):
                config = Path(kwargs['env']['XDG_CONFIG_HOME'])
                data = Path(kwargs['env']['XDG_DATA_HOME'])
                desktop = next((data / 'applications').glob('putgdsurface*.desktop'))
                if args[1] == 'default':
                    (config / 'mimeapps.list').write_bytes(b'x-scheme-handler/https=' + desktop.name.encode())
                return subprocess.CompletedProcess(args, 0, desktop.name + '\n', '')
            client = SimpleNamespace(call=lambda *args: [] if args[0].endswith('GetWindowHandles') else None,
                                     script=lambda *args: True, close=lambda: None)
            def remote(ctx, action, values):
                ctx.events.append(('remote', action))
                return json.loads(json.dumps(browser._target(action, json.loads(json.dumps(values)))))
            def trigger():
                for config in configs:
                    self.assertNotEqual((config / 'mimeapps.list').read_bytes()[:9], b'[Default ')
                raise RecipeFailed('entry point did not dispatch')
            ctx = FakeContext()
            with patch.dict(browser.os.environ, {'XDG_CONFIG_HOME': str(configs[1]),
                                                 'XDG_DATA_HOME': str(home / 'isolated/data')}), \
                 patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))), \
                 patch.object(browser.subprocess, 'Popen', return_value=process), \
                 patch.object(browser, '_Marionette', return_value=client), \
                 patch.object(browser, '_firefox_mains', return_value=[]), \
                 patch.object(browser, '_terminate') as terminate, \
                 patch.object(browser.subprocess, 'run', side_effect=mime), \
                 patch.object(browser, '_remote', remote), \
                 patch.object(Path, 'read_text', read):
                with self.assertRaisesRegex(RecipeFailed, 'entry point did not dispatch'):
                    with browser.default_browser(ctx, 'firefox'):
                        browser.open_and_verify(ctx, trigger, ctx.upload['expect'])
            for config, original in zip(configs, originals):
                path = config / 'mimeapps.list'
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), hashlib.sha256(original).hexdigest())
                self.assertEqual(path.stat().st_mode & 0o777, 0o640)
            self.assertFalse(list(home.rglob('putgdsurface*.desktop')))
            self.assertFalse(list((home / '.mozilla/firefox').iterdir()))
            terminate.assert_called_once_with(999999999, '7')
            self.assertEqual([e for e in ctx.events if e[0] == 'remote'],
                             [('remote', a) for a in ('prepare', 'blank', 'blank', 'restore')])
            self.assertIn(('frame', 'browser-failure-safe-desktop', None), ctx.events)
            self.assertNotIn(id(ctx), browser._ACTIVE)


class MimeTransactions(unittest.TestCase):
    def test_byte_mode_hash_restore_existing_missing_and_desktop_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config, data = root / 'config', root / 'data'
            config.mkdir()
            (data / 'applications').mkdir(parents=True)
            original = b'# keep CRLF and unknown recorder\r\n[Default Applications]\r\nx-scheme-handler/https=guarded-recorder.desktop;\r\n'
            path = config / 'mimeapps.list'
            path.write_bytes(original)
            path.chmod(0o640)
            desktop_path = config / 'cinnamon-mimeapps.list'
            desktop_path.write_bytes(b'\x00custom desktop bytes\xff')
            with patch.dict('os.environ', {'XDG_CURRENT_DESKTOP': 'X-Cinnamon:Cinnamon'}):
                backups = browser._snapshot(browser._mime_paths(config, data))
            for item in backups:
                Path(item['path']).write_bytes(b'changed')
            browser._restore_mime(backups)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(path.stat().st_mode & 0o777, 0o640)
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), hashlib.sha256(original).hexdigest())
            self.assertEqual(desktop_path.read_bytes(), b'\x00custom desktop bytes\xff')
            self.assertFalse((data / 'applications/mimeapps.list').exists())
            self.assertFalse((config / 'x-cinnamon-mimeapps.list').exists())

    def test_symlink_refused_without_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'original'
            target.write_bytes(b'original')
            link = Path(temporary) / 'mimeapps.list'
            link.symlink_to(target)
            with self.assertRaises(RuntimeError):
                browser._snapshot([link])
            self.assertEqual(target.read_bytes(), b'original')

    def test_partial_prepare_failure_restores_bytes_and_preserves_recorder(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            config = home / '.config'
            config.mkdir()
            mime = config / 'mimeapps.list'
            original = b'[Default Applications]\nx-scheme-handler/https=recorder.desktop;\n'
            mime.write_bytes(original)
            applications = home / '.local/share/applications'
            applications.mkdir(parents=True)
            recorder = applications / 'recorder.desktop'
            recorder.write_bytes(b'never delete me')
            process = SimpleNamespace(pid=999999999, poll=lambda: None)
            # Avoid a real browser and /proc access; the MIME operation really
            # changes bytes before failing, as a half-completed xdg-mime can.
            real_read = Path.read_text
            def read(path, *args, **kwargs):
                return FAKE_STAT if str(path) == '/proc/999999999/stat' else real_read(path, *args, **kwargs)
            def fail(*args, **kwargs):
                # GLib rejects a quoted program token in Exec, even though
                # xdg-mime default exits zero. Exercise its actual parser.
                try:
                    from gi.repository import Gio
                except ImportError:
                    Gio = None
                if Gio is not None:
                    candidate = next(p for p in applications.glob('putgdsurface*.desktop'))
                    self.assertIsNotNone(Gio.DesktopAppInfo.new_from_filename(str(candidate)))
                mime.write_bytes(b'partially modified')
                raise subprocess.CalledProcessError(1, 'xdg-mime')
            with patch.dict(browser.os.environ, {'XDG_CONFIG_HOME': str(home / 'isolated/config'),
                                                 'XDG_DATA_HOME': str(home / 'isolated/data')}), \
                 patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))), \
                 patch.object(browser.subprocess, 'Popen', return_value=process), \
                 patch.object(browser, '_Marionette') as protocol, \
                 patch.object(browser, '_firefox_mains', return_value=[]), \
                 patch.object(browser, '_terminate') as terminate, \
                 patch.object(browser.subprocess, 'run', side_effect=fail), \
                 patch.object(Path, 'read_text', read):
                with self.assertRaises(subprocess.CalledProcessError):
                    browser._prepare({})
            self.assertEqual(mime.read_bytes(), original)
            self.assertEqual(recorder.read_bytes(), b'never delete me')
            self.assertEqual(list(applications.iterdir()), [recorder])
            self.assertEqual(list((home / '.mozilla/firefox').iterdir()), [])

    def test_normal_restore_covers_portal_and_isolated_app_mime_profiles(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            configs = [home / '.config', home / 'isolated/config']
            originals = [b'# real portal\r\n', b'# isolated app\n']
            for config, original in zip(configs, originals):
                config.mkdir(parents=True)
                (config / 'mimeapps.list').write_bytes(original)
            process = SimpleNamespace(pid=999999999, poll=lambda: None)
            real_read = Path.read_text
            def read(path, *args, **kwargs):
                return FAKE_STAT if str(path) == '/proc/999999999/stat' else real_read(path, *args, **kwargs)
            calls = []
            def mime(args, **kwargs):
                config = Path(kwargs['env']['XDG_CONFIG_HOME'])
                data = Path(kwargs['env']['XDG_DATA_HOME'])
                desktop = next((data / 'applications').glob('putgdsurface*.desktop'))
                calls.append((args[1], str(config)))
                if args[1] == 'default':
                    (config / 'mimeapps.list').write_bytes(b'changed by MIME setup')
                return subprocess.CompletedProcess(args, 0, desktop.name + '\n', '')
            with patch.dict(browser.os.environ, {'XDG_CONFIG_HOME': str(configs[1]),
                                                 'XDG_DATA_HOME': str(home / 'isolated/data')}), \
                 patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))), \
                 patch.object(browser.subprocess, 'Popen', return_value=process), \
                 patch.object(browser, '_Marionette'), \
                 patch.object(browser, '_firefox_mains', return_value=[]), \
                 patch.object(browser, '_terminate') as terminate, \
                 patch.object(browser.subprocess, 'run', side_effect=mime), \
                 patch.object(Path, 'read_text', read):
                state = browser._prepare({})
                self.assertEqual(len(calls), 8)
                self.assertTrue(browser._restore(state)['restored'])
                terminate.assert_called_once_with(999999999, '7')
            for config, original in zip(configs, originals):
                self.assertEqual((config / 'mimeapps.list').read_bytes(), original)
            self.assertFalse(list(home.rglob('putgdsurface*.desktop')))


class RecipeContracts(unittest.TestCase):
    @contextmanager
    def guarded(self, ctx, name):
        ctx.events.append(('guard', name))
        yield

    def verify(self, ctx, trigger, expect):
        self.assertIn('name', expect)
        trigger()
        ctx.events.append(('verified',))
        return {'matched': True}

    def test_every_owned_id_and_real_entrypoint(self):
        owners = json.loads((Path(__file__).resolve().parents[2] / 'tools/lab/surface_driver/ownership.json').read_text())['owners']
        expected = {key for key, owner in owners.items() if owner == 'links'}
        registered = [recipe for recipe in REGISTRY if recipe.function.__module__ == links.__name__]
        self.assertEqual({key for recipe in registered for key in recipe.interactions}, expected)
        ctx = FakeContext()
        with patch.object(links, 'default_browser', self.guarded), patch.object(links, 'open_and_verify', self.verify):
            for recipe in registered:
                recipe.function(ctx)
        self.assertIn(('click', 'openHistoryLink', 'history'), ctx.events)
        self.assertIn(('click', 'appVersionLink', 'history'), ctx.events)
        self.assertIn(('mcp', 'open_history_link', {'id': 'history-1'}), ctx.events)
        self.assertIn(('keys', ('Shift+F10',)), ctx.events)
        self.assertIn(('keys', ('Ctrl+L', 'Ctrl+V', 'Return')), ctx.events)
        self.assertEqual(sum(e == ('verified',) for e in ctx.events), 8)

    def test_physical_desktops_and_boat_heads_rejected(self):
        ctx = FakeContext()
        for placement in ('native', 'boat'):
            ctx.placement = placement
            with self.assertRaises(RecipeBlocked):
                links.history_open_link(ctx)
        self.assertFalse(ctx.events)

    def test_core_local_save_fixture_blocks_before_browser_guard(self):
        ctx = FakeContext()
        ctx.upload = {'history_id': 'local-save', 'link': ''}
        with patch.object(links, 'default_browser') as guard:
            with self.assertRaises(RecipeBlocked) as result:
                links.history_copy_link(ctx)
            self.assertEqual(result.exception.issue, 691)
            guard.assert_not_called()

    def test_core_at_spi_rect_is_used_for_real_input(self):
        ctx = FakeContext()
        with patch.object(ctx, 'accessible', return_value=[{'rect': [5, 10, 30, 20]}]):
            links._accessible_click(ctx, {'name': 'Open in browser'})
        self.assertIn(('click_at', 20, 20), ctx.events)

    def test_ambiguous_accessibility_is_not_clicked(self):
        ctx = FakeContext()
        with patch.object(ctx, 'accessible', return_value=[{}, {}]):
            with self.assertRaises(RecipeFailed):
                links._accessible_click(ctx, {'name': 'Open in browser'})
        self.assertFalse(any(e[0] == 'click_at' for e in ctx.events))


if __name__ == '__main__':
    unittest.main()
