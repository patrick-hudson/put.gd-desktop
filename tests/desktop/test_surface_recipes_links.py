#!/usr/bin/env python3
"""Lightweight contracts; these do not establish graphical acceptance."""
import contextlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import unittest.mock
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.lab.surface_driver import browser
from tools.lab.surface_driver.api import Context, Frame, RecipeBlocked, RecipeFailed
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

    def frame(self, label, window=None, reference=None, *, desktop_safe=None):
        self.events.append(('frame', label, window))
        self.declarations = getattr(self, 'declarations', []) + [desktop_safe]
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


class MimeTransactions(unittest.TestCase):
    def test_symlink_refused_without_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'original'
            target.write_bytes(b'original')
            link = Path(temporary) / 'mimeapps.list'
            link.symlink_to(target)
            with self.assertRaises(RuntimeError):
                browser._snapshot([link])
            self.assertEqual(target.read_bytes(), b'original')


def _real_setup(home, configs, originals, private_data=None):
    """Patches for running the real target helper in-process under a fake home."""
    private_data = private_data or home / 'isolated/data'
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
                                            'XDG_DATA_HOME': str(private_data)}),
            patch.object(browser.pwd, 'getpwuid', return_value=SimpleNamespace(pw_dir=str(home))),
            patch.object(browser.subprocess, 'Popen', return_value=process),
            patch.object(browser, '_Marionette', return_value=client),
            patch.object(browser, '_firefox_mains', return_value=[]),
            patch.object(browser.subprocess, 'run', side_effect=mime),
            patch.object(Path, 'read_text', read)]


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

    def test_stray_firefox_found_at_restore_fails_a_passing_run(self):
        # Validator N1: one started after the observation is still stopped and named.
        ctx, events = FakeContext(), []
        with patch.object(browser, '_remote', self.remote(events, restore={'restored': True, 'foreign': 1})):
            with self.assertRaisesRegex(RecipeFailed, 'no Firefox started outside the private profile'):
                with browser.default_browser(ctx):
                    browser.open_and_verify(ctx, lambda: None, ctx.upload['expect'])


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


class CreatedDirectoriesExact(unittest.TestCase):
    def test_restore_never_removes_what_setup_did_not_record(self):
        # Validator N2: an empty directory another program made inside a
        # created directory, and a created ~/snap that now holds other snaps.
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary)
            configs = [home / '.config', home / 'isolated/config']
            with contextlib.ExitStack() as stack:
                for item in _real_setup(home, configs, [b'a', b'b']):
                    stack.enter_context(item)
                stack.enter_context(patch.object(browser, '_terminate'))
                state = browser._prepare({'profile_root': '~/snap/firefox/common'})
                created = json.loads((Path(state['directory']) / 'transaction.json').read_text())['created']
                self.assertEqual(created[:3], [str(home / p) for p in ('snap', 'snap/firefox', 'snap/firefox/common')])
                (home / 'snap/firefox/common/Crash Reports').mkdir()
                (home / 'snap/firefox/1234').mkdir()  # snapd's revision data.
                (home / 'snap/chromium/common').mkdir(parents=True)  # Another snap.
                (home / 'isolated/data/applications/other-empty').mkdir()
                browser._restore(state)
            self.assertFalse((home / 'snap/firefox/common').exists())
            self.assertTrue((home / 'snap/firefox/1234').is_dir())
            self.assertTrue((home / 'snap/chromium/common').is_dir())
            self.assertTrue((home / 'isolated/data/applications/other-empty').is_dir())
            self.assertFalse(list(home.rglob('putgdsurface*.desktop')))


class RecipeContracts(unittest.TestCase):
    def test_physical_desktops_and_boat_heads_rejected(self):
        ctx = FakeContext()
        for placement in ('native', 'boat'):
            ctx.placement = placement
            with self.assertRaises(RecipeBlocked):
                links.history_open_link(ctx)
        self.assertFalse(ctx.events)


if __name__ == '__main__':
    unittest.main()
