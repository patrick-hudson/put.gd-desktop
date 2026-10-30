#!/usr/bin/env python3
"""Lightweight contracts; these do not establish graphical acceptance."""
import base64
import contextlib
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import socket
import threading
import sys
import tempfile
import time
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

    def remote(self, ctx, action, values, **kwargs):
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
        def remote(ctx, action, values, **kwargs):
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
        def remote(ctx, action, values, **kwargs):
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
        def remote(ctx, action, values, **kwargs):
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
                    for i in range(1, 6):
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
                            # Firefox's own timeouts answer by name before the socket's.
                            self.assertEqual(command[2:], ['WebDriver:SetTimeouts', {'script': 20000, 'pageLoad': 20000}])
                            send([1, i, None, {'value': None}])
                        elif i == 3:
                            self.assertEqual(command[2], 'WebDriver:GetWindowHandles')
                            send([1, i, None, {'value': ['tab-1', 'tab-2']}])
                        elif i == 4:
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
        client = SimpleNamespace(broken=False)
        def call(method, params=None):
            if method == 'WebDriver:GetWindowHandles':
                return ['closed', 'good']
            if params['handle'] == 'closed':
                raise browser._TargetError('Firefox protocol command failed')
        client.call = call
        def script(source, args=(), asynchronous=False):
            return True if asynchronous else {'matched': True, 'url_host': '127.0.0.1', 'title': 'fixture.txt'}
        client.script = script
        client.close = lambda: None
        with patch.object(browser, '_Marionette', return_value=client), patch.object(browser.time, 'sleep'):
            result = browser._target('observe', {**self.values, 'expect': {'name': 'fixture.txt'}})
        self.assertTrue(result['matched'])

    def test_inspection_never_selects_tabs_and_waits_for_the_page_paint(self):
        # Scanning with focus=True switched tabs; the frame then caught the
        # newly selected tab before it painted (blank guest frames, #693).
        calls = []
        client = SimpleNamespace(close=lambda: None, broken=False)
        client.call = lambda method, params=None: (calls.append((method, params)),
                                                   ['blank', 'page'] if method.endswith('GetWindowHandles') else None)[1]
        selected = []
        def script(source, args=(), asynchronous=False):
            if asynchronous:
                calls.append(('painted', None))
                return True
            handle = [p for m, p in calls if m == 'WebDriver:SwitchToWindow'][-1]['handle']
            return {'matched': handle == 'page', 'url_host': '127.0.0.1', 'title': 'fixture.txt'}
        client.script = script
        with patch.object(browser, '_Marionette', return_value=client), patch.object(browser.time, 'sleep'):
            self.assertTrue(browser._target('observe', {**self.values, 'expect': {'name': 'fixture.txt'}})['matched'])
        switches = [p for m, p in calls if m == 'WebDriver:SwitchToWindow']
        self.assertEqual(switches, [{'handle': 'blank', 'focus': False}, {'handle': 'page', 'focus': False}])
        self.assertEqual(calls[-1], ('painted', None))

    def test_unpainted_or_background_match_does_not_pass(self):
        client = SimpleNamespace(close=lambda: None, broken=False, call=lambda *args: ['page'] if args[0].endswith('GetWindowHandles') else None,
                                 script=lambda source, args=(), asynchronous=False:
                                     False if asynchronous else {'matched': True})
        with patch.object(browser, '_Marionette', return_value=client):
            self.assertEqual(browser._target('observe', {**self.values, 'expect': {}}), {})

    def test_non_mapping_browser_reply_does_not_pass(self):
        client = SimpleNamespace(broken=False, call=lambda *args: ['good'] if args[0].endswith('GetWindowHandles') else None,
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
        client = SimpleNamespace(broken=False, call=lambda *args: ['tab'] if args[0].endswith('GetWindowHandles') else None,
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
            client = SimpleNamespace(broken=False, call=lambda *args: ['tab'] if args[0].endswith('GetWindowHandles') else None,
                                     script=lambda *args: True, close=lambda: None)
            def remote(ctx, action, values, **kwargs):
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
            self.assertFalse((home / '.mozilla').exists())
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
            self.assertFalse((home / '.mozilla').exists())

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


def _real_setup(home, configs, originals):
    """Patches for running the real target helper in-process under a fake home."""
    for config, original in zip(configs, originals):
        config.mkdir(parents=True, exist_ok=True)
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
    client = SimpleNamespace(broken=False, close=lambda: None,
                             call=lambda *args: ['tab'] if args[0].endswith('GetWindowHandles') else None,
                             script=lambda *args, **kwargs: True)
    return [patch.dict(browser.os.environ, {'XDG_CONFIG_HOME': str(configs[1]),
                                            'XDG_DATA_HOME': str(home / 'isolated/data')}),
            patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))),
            patch.object(browser.subprocess, 'Popen', return_value=process),
            patch.object(browser, '_Marionette', return_value=client),
            patch.object(browser, '_firefox_mains', return_value=[]),
            patch.object(browser.subprocess, 'run', side_effect=mime),
            patch.object(Path, 'read_text', read)]


def _run_helper(script):
    """What the target shell does with _remote's script, in-process."""
    _, _, action, payload = shlex.split(script.split('\n', 1)[0])[:4]
    try:
        answer = browser._target(action, json.loads(base64.b64decode(payload)))
    except BaseException as error:
        answer = {'error': str(error) if isinstance(error, browser._TargetError) else type(error).__name__}
    return {'returncode': 0, 'stdout': json.dumps(answer), 'stderr': ''}


class DeadlineRestore(unittest.TestCase):
    def test_deadline_inside_transaction_still_restores_everything(self):
        # Validator B1: Context.shell refuses after the recipe deadline, which
        # skipped restore and left the handler, profile and Firefox behind.
        from tools.lab.surface_driver.context import DriverContext
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary) / 'home'
            configs = [home / '.config', home / 'isolated/config']
            originals = [b'[Default Applications]\r\nx-scheme-handler/https=recorder.desktop;\r\n', b'# isolated\n']
            calls = []
            target = SimpleNamespace(heartbeat=lambda: None, physical=None, run=Path(temporary) / 'run')
            def execute(script, timeout=60):
                calls.append((shlex.split(script.split('\n', 1)[0])[2], timeout, ctx._deadline - time.monotonic()))
                return _run_helper(script)
            target.execute = execute
            environment = dict(desktop='mint-cinnamon-x11', host='boat', placement='guest', package='deb',
                               destination='local')
            ctx = DriverContext(target, environment, {}, temporary, Path(temporary) / 'out')
            ctx._deadline = time.monotonic() + 60
            def trigger():
                ctx._deadline = time.monotonic() - 1  # The recipe overran inside the transaction.
            patches = _real_setup(home, configs, originals)
            with contextlib.ExitStack() as stack:
                for item in patches:
                    stack.enter_context(item)
                terminate = stack.enter_context(patch.object(browser, '_terminate'))
                owned = stack.enter_context(patch('tools.lab.lab.verify_owned_guest'))
                with self.assertRaisesRegex(RecipeFailed, '^recipe deadline exceeded$'):
                    with browser.default_browser(ctx, 'firefox'):
                        browser.open_and_verify(ctx, trigger, {'kind': 'local', 'name': 'f.txt', 'preview': False})
            self.assertEqual([c[0] for c in calls], ['prepare', 'blank', 'restore'])
            action, timeout, remaining = calls[-1]
            self.assertLess(remaining, 0)  # Ran after the deadline...
            self.assertEqual(timeout, browser._TIMEOUTS['cleanup'])  # ...with its own, unclipped budget.
            for config, original in zip(configs, originals):
                path = config / 'mimeapps.list'
                self.assertEqual(path.read_bytes(), original)
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), hashlib.sha256(original).hexdigest())
                self.assertEqual(path.stat().st_mode & 0o777, 0o640)
            self.assertFalse(list(home.rglob('putgdsurface*.desktop')))
            self.assertFalse((home / '.mozilla').exists())
            self.assertFalse((home / '.local/share/applications').exists())
            terminate.assert_called_once_with(999999999, '7')
            owned.assert_called_once_with(target.run)  # Context.shell(cleanup=True)'s guest check.
            self.assertFalse(ctx.frames)
            self.assertTrue(any('could not be blanked' in review for review in ctx.reviews))
            self.assertNotIn(id(ctx), browser._ACTIVE)


class FailurePath(unittest.TestCase):
    def remote(self, events, **overrides):
        def remote(ctx, action, values, timeout=None, cleanup=False):
            events.append((action, cleanup))
            if action in overrides:
                value = overrides[action]
                if isinstance(value, BaseException):
                    raise value
                return value(events) if callable(value) else value
            return {'prepare': {'directory': '/private/firefox', 'port': 1234}, 'restore': {'restored': True},
                    'observe': {'url_host': '127.0.0.1', 'title': 'fixture.txt', 'matched': True},
                    'recover': {'recovered': 0}}.get(action, {'blank': True, 'safe': True})
        return remote

    def test_failure_blank_error_keeps_the_original_exception(self):
        # CodeRabbit (browser.py:125) / validator N2.
        ctx, events = FakeContext(), []
        def blank(events):
            if sum(e[0] == 'blank' for e in events) > 1:
                raise RuntimeError('Target browser helper: Firefox did not answer WebDriver:Navigate within 30 s')
            return {'blank': True, 'safe': True}
        def fail():
            raise RecipeFailed('entry point did not dispatch')
        with patch.object(browser, '_remote', self.remote(events, blank=blank)):
            with self.assertRaisesRegex(RecipeFailed, '^entry point did not dispatch$'):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, fail, ctx.upload['expect'])
        self.assertFalse(any(e[0] == 'frame' for e in ctx.events))
        self.assertTrue(any(e[0] == 'review' and 'WebDriver:Navigate' in e[1] for e in ctx.events))
        self.assertEqual(events[-1], ('restore', True))

    def test_keyboard_interrupt_skips_the_frame_and_still_restores(self):
        ctx, events = FakeContext(), []
        def interrupt():
            raise KeyboardInterrupt
        with patch.object(browser, '_remote', self.remote(events)):
            with self.assertRaises(KeyboardInterrupt):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, interrupt, ctx.upload['expect'])
        self.assertEqual(events, [('prepare', False), ('blank', False), ('restore', True)])
        self.assertNotIn(id(ctx), browser._ACTIVE)

    def test_failure_frame_error_keeps_the_original_exception(self):
        ctx, events = FakeContext(), []
        def fail():
            raise RecipeFailed('entry point did not dispatch')
        with patch.object(browser, '_remote', self.remote(events)), \
             patch.object(ctx, 'frame', side_effect=RecipeFailed('recipe deadline exceeded')):
            with self.assertRaisesRegex(RecipeFailed, '^entry point did not dispatch$'):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, fail, ctx.upload['expect'])
        self.assertTrue(any(e[0] == 'review' and 'Failure frame unavailable' in e[1] for e in ctx.events))

    def test_interrupted_setup_is_recovered_outside_the_deadline(self):
        # Validator N3: the transport failed, but the target step may still finish.
        ctx, events = FakeContext(), []
        with patch.object(browser, '_remote', self.remote(events, prepare=RecipeBlocked('target session command unavailable'))):
            with self.assertRaises(RecipeBlocked):
                with browser.default_browser(ctx):
                    self.fail('body must not run')
        self.assertEqual(events, [('prepare', False), ('recover', True)])

    def test_stray_firefox_found_at_restore_fails_a_passing_run(self):
        # Validator N1: one started after the observation is still stopped and named.
        ctx, events = FakeContext(), []
        with patch.object(browser, '_remote', self.remote(events, restore={'restored': True, 'foreign': 1})):
            with self.assertRaisesRegex(RecipeFailed, 'no Firefox started outside the private profile'):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, lambda: None, ctx.upload['expect'])

    def test_named_pending_reason_reaches_the_page_timeout(self):
        ctx, events = FakeContext(), []
        with patch.object(browser, '_remote', self.remote(events, observe={'pending': 'Firefox WebDriver:ExecuteAsyncScript failed: script timeout'})):
            with self.assertRaisesRegex(RecipeFailed, 'last Firefox answer: Firefox WebDriver:ExecuteAsyncScript failed: script timeout'):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, lambda: None, ctx.upload['expect'])

    def test_profile_timeouts_are_validated_and_used(self):
        ctx, timeouts = FakeContext(), []
        ctx.profile = {'browser': {'timeouts': {'protocol': 30, 'page': 60}}}
        def remote(ctx, action, values, timeout=None, cleanup=False):
            timeouts.append((action, timeout))
            return self.remote([])(ctx, action, values)
        with patch.object(browser, '_remote', remote):
            with browser.default_browser(ctx):
                browser.open_and_verify(ctx, lambda: None, ctx.upload['expect'])
        self.assertEqual(timeouts[0], ('prepare', 120 + 60))
        self.assertEqual(timeouts[-1], ('restore', 180))
        for bad in ({'protocol': 0}, {'protocol': 'slow'}, {'typo': 5}):
            ctx.profile = {'browser': {'timeouts': bad}}
            with self.assertRaises(ValueError):
                with browser.default_browser(ctx):
                    pass


class Timeouts(unittest.TestCase):
    def serve(self, answers):
        """A Marionette peer answering command N with answers[N-1] (None: stay silent)."""
        server = socket.socket()
        server.bind(('127.0.0.1', 0))
        server.listen(1)
        seen = []
        def run():
            try:
                conn, _ = server.accept()
                with conn, conn.makefile('rb') as stream:
                    def send(value):
                        data = json.dumps(value).encode()
                        conn.sendall(str(len(data)).encode() + b':' + data)
                    send({'marionetteProtocol': 3})
                    for i, answer in enumerate(answers, 1):
                        header = b''
                        while (char := stream.read(1)) not in (b':', b''):
                            header += char
                        if not header:
                            return
                        seen.append(json.loads(stream.read(int(header)))[2])
                        if answer is None:
                            stream.read(1)  # Silent until the client hangs up.
                            return
                        send([1, i] + answer)
            finally:
                server.close()
        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 5)
        return server.getsockname()[1], seen

    def test_silent_firefox_is_a_named_timeout_and_close_does_not_reread(self):
        # Validator B2: close() read the timed-out socket and became a bare OSError.
        port, seen = self.serve([[None, {}], [None, {}], None])
        with patch.object(browser._Marionette, 'grace', 0.1):
            client = browser._Marionette(port, timeout=0.2)
            with self.assertRaisesRegex(browser._Timeout, r'^Firefox did not answer WebDriver:Navigate within 0\.3 s$'):
                client.call('WebDriver:Navigate', {'url': 'about:blank'})
            self.assertTrue(client.broken)
            client.close()
        self.assertEqual(seen, ['WebDriver:NewSession', 'WebDriver:SetTimeouts', 'WebDriver:Navigate'])

    def test_firefox_timeout_error_is_named_without_its_message(self):
        port, seen = self.serve([[None, {}], [None, {}],
                                 [{'error': 'timeout', 'message': 'https://example.invalid/SECRET-TOKEN'}, None],
                                 [None, {}]])
        client = browser._Marionette(port)
        with self.assertRaises(browser._Timeout) as error:
            client.call('WebDriver:Navigate', {'url': 'about:blank'})
        self.assertEqual(str(error.exception), 'Firefox WebDriver:Navigate failed: timeout')
        self.assertFalse(client.broken)
        client.close()
        self.assertEqual(seen[-1], 'WebDriver:DeleteSession')

    def test_slow_blank_navigation_is_checked_by_the_page_itself(self):
        calls = []
        def call(method, params=None):
            calls.append(method)
            if method.endswith('GetWindowHandles'):
                return ['first', 'dispatched']
            if method.endswith('Navigate'):
                raise browser._Timeout('Firefox WebDriver:Navigate failed: timeout')
        answers = iter([False, True])
        client = SimpleNamespace(broken=False, call=call, script=lambda *args: next(answers))
        with patch.object(browser.time, 'sleep'):
            self.assertTrue(browser._blank(client, 5))
        self.assertEqual(calls.count('WebDriver:CloseWindow'), 1)

    def test_broken_tab_check_is_pending_not_an_aborted_observe(self):
        client = SimpleNamespace(broken=False, call=lambda *args: ['a', 'b'] if args[0].endswith('GetWindowHandles') else None)
        def script(source, args=(), asynchronous=False):
            if asynchronous:
                client.broken = True
                raise browser._Timeout('Firefox did not answer WebDriver:ExecuteAsyncScript within 30 s')
            return {'matched': True}
        client.script = script
        self.assertEqual(browser._observe(client, {}),
                         {'pending': 'Firefox did not answer WebDriver:ExecuteAsyncScript within 30 s'})

    def test_slow_tab_does_not_hide_a_later_match(self):
        tabs = []
        def call(method, params=None):
            if method.endswith('GetWindowHandles'):
                return ['slow', 'page']
            tabs.append(params['handle'])
        def script(source, args=(), asynchronous=False):
            if tabs[-1] == 'slow':
                raise browser._Timeout('Firefox WebDriver:ExecuteScript failed: script timeout')
            return True if asynchronous else {'matched': True, 'url_host': '127.0.0.1', 'title': 'f.txt'}
        client = SimpleNamespace(broken=False, call=call, script=script)
        with patch.object(browser.time, 'sleep'):
            self.assertTrue(browser._observe(client, {})['matched'])


class ForeignAfterMatch(unittest.TestCase):
    def test_stray_firefox_beside_a_matching_tab_fails_without_frame_and_is_stopped(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'transaction.json').write_text(json.dumps(
                {'directory': str(directory), 'since': 100, 'foreign': [], 'desktops': [], 'backups': [],
                 'pid': None}))
            stray = (11, '150', [b'/usr/lib/firefox/firefox', b'http://127.0.0.1/private-token/f.txt'])
            client = SimpleNamespace(broken=False, close=lambda: None,
                                     call=lambda *args: ['page'] if args[0].endswith('GetWindowHandles') else None,
                                     script=lambda source, args=(), asynchronous=False:
                                         True if asynchronous else {'matched': True, 'url_host': 'h', 'title': 't'})
            with patch.object(browser, '_Marionette', return_value=client), patch.object(browser.time, 'sleep'), \
                 patch.object(browser, '_firefox_mains', return_value=[stray]):
                result = browser._target('observe', {'directory': str(directory), 'port': 1, 'expect': {}})
                self.assertEqual(result, {'matched': False, 'foreign': 1})
                with patch.object(browser, '_terminate') as terminate:
                    self.assertEqual(browser._restore({'directory': str(directory)}), {'restored': True, 'foreign': 1})
                terminate.assert_called_once_with(11, '150')

    def test_restore_scan_stops_an_unrecorded_stray(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'putgd-surface-x'
            directory.mkdir()
            (directory / 'transaction.json').write_text(json.dumps(
                {'directory': str(directory), 'since': 100, 'foreign': [], 'desktops': [], 'backups': [], 'pid': None}))
            mains = [(10, '50', [b'firefox']), (11, '150', [b'firefox', b'http://x/private-token'])]
            with patch.object(browser, '_firefox_mains', return_value=mains), \
                 patch.object(browser, '_terminate') as terminate:
                self.assertEqual(browser._restore({'directory': str(directory)})['foreign'], 1)
            terminate.assert_called_once_with(11, '150')
            self.assertFalse(directory.exists())


class Recovery(unittest.TestCase):
    def test_setup_first_restores_a_transaction_an_interrupted_run_left(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            configs = [home / '.config', home / 'isolated/config']
            originals = [b'# real portal\r\n', b'# isolated app\n']
            with contextlib.ExitStack() as stack:
                for item in _real_setup(home, configs, originals):
                    stack.enter_context(item)
                terminate = stack.enter_context(patch.object(browser, '_terminate'))
                abandoned = Path(browser._prepare({})['directory'])  # Its controller vanished.
                stored = json.loads((abandoned / 'transaction.json').read_text())
                stored['helper'] = [999999998, '1']  # That helper has exited.
                (abandoned / 'transaction.json').write_text(json.dumps(stored))
                state = browser._prepare({})
                self.assertEqual(state['recovered'], 1)
                self.assertFalse(abandoned.exists())
                # The new backup is of the originals, not the abandoned handler.
                current = json.loads((Path(state['directory']) / 'transaction.json').read_text())
                self.assertEqual({base64.b64decode(b['bytes']) for b in current['backups'] if b['bytes']}, set(originals))
                self.assertTrue(browser._restore(state)['restored'])
            self.assertEqual(terminate.call_count, 2)
            for config, original in zip(configs, originals):
                self.assertEqual((config / 'mimeapps.list').read_bytes(), original)
            self.assertFalse(list(home.rglob('putgdsurface*.desktop')))
            self.assertFalse((home / '.mozilla').exists())

    def test_interrupted_restore_is_resumed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / 'putgd-surface-x'
            (directory / 'cache').mkdir(parents=True)
            mime = root / 'mimeapps.list'
            mime.write_bytes(b'handler')
            (directory / 'transaction.json').write_text(json.dumps(
                {'directory': str(directory), 'since': 0, 'foreign': [], 'desktops': [], 'pid': None,
                 'backups': [{'path': str(mime), 'bytes': base64.b64encode(b'original').decode(),
                              'sha256': hashlib.sha256(b'original').hexdigest(), 'mode': 0o600}]}))
            with patch.object(browser, '_firefox_mains', return_value=[]):
                self.assertEqual(browser._target('recover', {'profile_root': str(root)}), {'recovered': 1})
                self.assertEqual(browser._target('recover', {'profile_root': str(root)}), {'recovered': 0})
                # A controller retrying restore after recovery finds nothing left to do.
                self.assertTrue(browser._restore({'directory': str(directory)})['restored'])
            self.assertEqual(mime.read_bytes(), b'original')
            self.assertFalse(directory.exists())

    def test_dropped_session_hangup_does_not_stop_recovery(self):
        # Validator N3: the helper died by SIGHUP with the SSH session. Run the
        # real helper process, hang it up mid-step, drop its stdout, and check
        # that it still restores. It waits on a test-owned "setup helper".
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / 'putgd-surface-x'
            directory.mkdir()
            mime, desktop = root / 'mimeapps.list', root / 'putgdsurfacex.desktop'
            mime.write_bytes(b'x-scheme-handler/https=putgdsurfacex.desktop')
            desktop.write_bytes(b'[Desktop Entry]')
            setup = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])
            self.addCleanup(setup.wait)
            self.addCleanup(setup.kill)
            start = Path('/proc/%d/stat' % setup.pid).read_text().rsplit(')', 1)[1].split()[19]
            # since is beyond any real start time: the helper scans this host's
            # /proc, and nothing here may be mistaken for a stray Firefox.
            (directory / 'transaction.json').write_text(json.dumps(
                {'directory': str(directory), 'since': 10 ** 18, 'foreign': [], 'desktops': [str(desktop)],
                 'pid': None, 'helper': [setup.pid, start],
                 'backups': [{'path': str(mime), 'bytes': base64.b64encode(b'original').decode(),
                              'sha256': hashlib.sha256(b'original').hexdigest(), 'mode': 0o640}]}))
            payload = base64.b64encode(json.dumps({'profile_root': str(root), 'wait': 30}).encode()).decode()
            helper = subprocess.Popen([sys.executable, browser.__file__, 'recover', payload],
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            self.addCleanup(helper.wait, 5)
            deadline = time.monotonic() + 20
            while True:
                status = Path('/proc/%d/status' % helper.pid).read_text()
                ignored = int(re.search(r'^SigIgn:\s*([0-9a-f]+)$', status, re.M)[1], 16)
                if ignored & 1 and os.getpgid(helper.pid) != os.getpgid(0):
                    break
                self.assertLess(time.monotonic(), deadline, 'helper never detached from the session')
                time.sleep(.05)
            os.kill(helper.pid, signal.SIGHUP)
            helper.stdout.close()  # The SSH channel is gone.
            time.sleep(.3)
            self.assertIsNone(helper.poll())
            self.assertEqual(mime.read_bytes(), b'x-scheme-handler/https=putgdsurfacex.desktop')
            setup.kill()  # The setup step ends (an unreaped zombie); recovery proceeds.
            helper.wait(timeout=20)
            self.assertEqual(mime.read_bytes(), b'original')
            self.assertEqual(mime.stat().st_mode & 0o777, 0o640)
            self.assertFalse(desktop.exists())
            self.assertFalse(directory.exists())


class CreatedDirectories(unittest.TestCase):
    def test_restore_removes_only_directories_setup_created(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            (home / '.local/share').mkdir(parents=True)  # Pre-existing parent stays.
            configs = [home / '.config', home / 'isolated/config']
            with contextlib.ExitStack() as stack:
                for item in _real_setup(home, configs, [b'a', b'b']):
                    stack.enter_context(item)
                stack.enter_context(patch.object(browser, '_terminate'))
                state = browser._prepare({})
                created = json.loads((Path(state['directory']) / 'transaction.json').read_text())['created']
                self.assertEqual(created, [str(home / p) for p in
                                           ('.mozilla', '.mozilla/firefox', '.local/share/applications',
                                            'isolated/data', 'isolated/data/applications')])
                (home / 'isolated/data/applications/user-file.desktop').write_bytes(b'not ours')
                # What a real first Firefox start adds outside its --profile.
                (home / '.mozilla/extensions').mkdir()
                (home / '.mozilla/firefox/Crash Reports').mkdir()
                (home / '.mozilla/firefox/Crash Reports/InstallTime20251217121356').write_bytes(b'1761786005')
                browser._restore(state)
            self.assertTrue((home / '.local/share').exists())
            self.assertFalse((home / '.local/share/applications').exists())
            self.assertFalse((home / '.mozilla').exists())
            self.assertEqual((home / 'isolated/data/applications/user-file.desktop').read_bytes(), b'not ours')


class ProcessTree(unittest.TestCase):
    def test_restore_also_stops_children_that_outlive_firefox(self):
        # Firefox's crash helper wrote into the profile root after its parent
        # exited (Mint guest, Firefox 146). Use test-owned processes only.
        parent = subprocess.Popen([sys.executable, '-c',
                                   'import subprocess,sys,time\n'
                                   'subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])\n'
                                   'time.sleep(60)'])
        self.addCleanup(parent.wait)
        self.addCleanup(parent.kill)
        start = browser._start_ticks(parent.pid)
        deadline = time.monotonic() + 10
        while not browser._descendants(parent.pid, start):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.05)
        [(child, child_start)] = browser._descendants(parent.pid, start)
        self.addCleanup(lambda: browser._terminate(child, child_start))
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'putgd-surface-x'
            directory.mkdir()
            (directory / 'transaction.json').write_text(json.dumps(
                {'directory': str(directory), 'since': 10 ** 18, 'foreign': [], 'desktops': [], 'backups': [],
                 'pid': parent.pid, 'pid_start': start}))
            browser._restore({'directory': str(directory)})
        parent.wait(timeout=10)
        self.assertFalse(browser._alive([child, child_start]))
        self.assertEqual(browser._descendants(parent.pid, start), [])

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
