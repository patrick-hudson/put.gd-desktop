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
        return {'blank': True}

    def test_content_receipt_and_full_desktop(self):
        with patch.object(browser, '_remote', self.remote):
            with browser.default_browser(self.context, 'firefox'):
                receipt = browser.open_and_verify(self.context, lambda: self.context.events.append(('trigger',)),
                                                  self.context.upload['expect'])
        self.assertEqual(set(receipt), {'url_host', 'title', 'matched', 'frames'})
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
                return ' '.join(['0'] * 22) if str(path) == '/proc/999999999/stat' else real_read(path, *args, **kwargs)
            def fail(*args, **kwargs):
                mime.write_bytes(b'partially modified')
                raise subprocess.CalledProcessError(1, 'xdg-mime')
            with patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))), \
                 patch.object(browser.subprocess, 'Popen', return_value=process), \
                 patch.object(browser, '_Marionette') as protocol, \
                 patch.object(browser.subprocess, 'run', side_effect=fail), \
                 patch.object(Path, 'read_text', read):
                with self.assertRaises(subprocess.CalledProcessError):
                    browser._prepare({})
            self.assertEqual(mime.read_bytes(), original)
            self.assertEqual(recorder.read_bytes(), b'never delete me')
            self.assertEqual(list(applications.iterdir()), [recorder])
            self.assertEqual(list((home / '.mozilla/firefox').iterdir()), [])


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

    def test_ambiguous_accessibility_is_not_clicked(self):
        ctx = FakeContext()
        with patch.object(ctx, 'accessible', return_value=[{}, {}]):
            with self.assertRaises(RecipeFailed):
                links._accessible_click(ctx, {'name': 'Open in browser'})
        self.assertFalse(any(e[0] == 'click_at' for e in ctx.events))


if __name__ == '__main__':
    unittest.main()
