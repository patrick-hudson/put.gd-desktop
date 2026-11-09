#!/usr/bin/env python3
"""ScreenCast consent ownership/refusals (#791), private media retention (#796)
and the GTK sink preflight (#784). Seconds-long; no GUI, VM or session bus."""
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import consent, media
from tools.lab.surface_driver.fixtures.base import Scope
from tools.lab.surface_driver.probe import portal_request_path
from tools.lab.surface_driver.profiles import load_profile

OURS = ':1.80'
FRONTEND = ':1.20'
BACKEND = ':1.45'
TOKEN = 'putgd_record_4242_3'
HANDLE = portal_request_path(OURS, TOKEN)
FOREIGN_HANDLE = portal_request_path(':1.99', 'obs_7')


def public_start(sender=OURS, token=TOKEN):
    return {'kind': 'public', 'member': 'Start', 'sender': sender, 'token': token,
            'handle': portal_request_path(sender, token), 'at': 1.0}


def impl_start(handle=HANDLE, exe='xdg-desktop-portal-gnome', pid=4545, sender=FRONTEND, app=''):
    return {'kind': 'impl', 'interface': 'ScreenCast', 'member': 'Start', 'sender': sender,
            'destination': BACKEND, 'handle': handle, 'app_id': app, 'backend_pid': pid,
            'backend_exe': exe, 'at': 2.0}


def response(handle=HANDLE, code=0, sender=FRONTEND):
    return {'kind': 'response', 'sender': sender, 'destination': OURS, 'handle': handle, 'code': code, 'at': 3.0}


GNOME = load_profile('ubuntu-gnome-wayland')
POLICY = consent.policy(GNOME)


class Ownership(unittest.TestCase):
    def test_dialog_from_a_different_request_is_refused(self):
        kind, reason = consent.decision(consent.analyse([public_start(), impl_start(FOREIGN_HANDLE, app='com.obsproject.Studio')]), POLICY)
        self.assertEqual(kind, 'blocked')
        self.assertIn('different request', reason)
        # Even alongside our own pending request.
        kind, reason = consent.decision(consent.analyse([public_start(), impl_start(), impl_start(FOREIGN_HANDLE)]), POLICY)
        self.assertEqual(kind, 'blocked')

    def test_response_counts_only_from_the_forwarding_frontend(self):
        spoofed = consent.analyse([public_start(), impl_start(), response(sender=':1.66')])
        self.assertEqual(consent.decision(spoofed, POLICY)[0], 'dialog')
        real = consent.analyse([public_start(), impl_start(), response()])
        self.assertEqual(consent.decision(real, POLICY), ('answered', {HANDLE: 0}))

    def test_x11_and_undeclared_wayland_profiles_refuse_any_dialog(self):
        state = consent.analyse([public_start(), impl_start()])
        none = consent.policy(load_profile('mint-cinnamon-x11'))
        self.assertEqual(none['kind'], 'none')
        self.assertIn('declares no ScreenCast consent', consent.decision(state, none)[1])
        self.assertIn('no recording.consent', consent.decision(state, None)[1])
        self.assertIsNone(consent.policy({'session': 'wayland'}))


class FakeTarget:
    root = '/tmp/sd-consent'; socket = root + '/runtime/putgd/session.sock'; physical = None
    def __init__(self):
        self.pointer_events = []
    def screenshot(self, path, head=0):
        Path(path).write_bytes(b'png')
    def capture_image(self, head=0):
        from PIL import Image
        return Image.new('RGB', (1280, 800), 'white')
    def pointer(self, x, y, w, h, down=None, button='left', head=0):
        self.pointer_events.append((x, y, down))
    def cli(self, *args, timeout=60):
        return {'returncode': 0, 'stdout': '{"ok":true,"result":{}}', 'stderr': ''}
    def execute(self, script, timeout=60):
        return {'returncode': 0, 'stdout': '', 'stderr': ''}


DIALOG = {'id': 77, 'pid': 4545, 'title': 'Share Screen', 'frame': [343, 166, 660, 500], 'minimized': False}
SHARE = {'pid': 4545, 'window_id': 77, 'rect': [923, 172, 74, 34], 'enabled': True, 'role': 'push button', 'name': 'Share'}


FOREIGN_DIALOG = dict(DIALOG, id=88, frame=[393, 216, 660, 500])
FOREIGN_SHARE = dict(SHARE, window_id=88, rect=[973, 222, 74, 34])
OUR_PROMPT = {'pid': 4545, 'window_id': 77, 'rect': [400, 240, 500, 20],
              'name': "put.gd screen recorder wants to share your screen. Choose what you'd like to share."}
FOREIGN_PROMPT = {'pid': 4545, 'window_id': 88, 'rect': [450, 290, 500, 20],
                  'name': "An app wants to share your screen. Choose what you'd like to share."}
SETTLE = 'waiting for the consent dialog to settle'


class Drive(unittest.TestCase):
    """drive() against a fake bus log, compositor and accessibility tree."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.path = Path(self.temp.name)
        env = dict(desktop='ubuntu-gnome-wayland', host='boat', placement='boat', guest='ubuntu-gnome', package='deb', destination='local')
        profile = dict(GNOME, screens=[{'x': 0, 'y': 0, 'width': 1280, 'height': 800, 'head': 0}])
        self.target = FakeTarget()
        self.ctx = DriverContext(self.target, env, profile, self.path, self.path)
        self.windows = [DIALOG]
        self.shares = [SHARE]
        self.prompts = [OUR_PROMPT]
        self.pick = {'ok': True, 'obscuring_window': None}
        self.on_pick = None
        self.probes = []
        def probe(name, query):
            self.probes.append((name, query))
            if name == 'compositor_windows':
                return {'windows': list(self.windows)}
            if name == 'accessible':
                rows = self.shares if query.get('name') == 'Share' else self.prompts
                return [r for r in rows if fnmatch.fnmatchcase(r['name'], query['name'])]
            if name == 'foreground':
                if self.on_pick:
                    self.on_pick()
                return self.pick
            if name == 'clear_screen':
                return {'stopped': []}
            raise AssertionError(name)
        self.ctx._probe = probe
        press = self.target.pointer
        def pointer(x, y, w, h, down=None, button='left', head=0):
            press(x, y, w, h, down, button, head)
            if down is False:
                self.windows = []  # the backend closes its dialog on Share
        self.target.pointer = pointer
        self.request = impl_start(app='putgd-record')
        self.rows = [public_start(), self.request]
        self.watch = self.make_watch(offset=0)
        names = patch.object(consent, 'app_name', lambda ctx, app: {'putgd-record': 'put.gd screen recorder'}.get(app), create=True)
        names.start(); self.addCleanup(names.stop)
    def make_watch(self, offset):
        watch = consent.Watch(SimpleNamespace(ctx=self.ctx), POLICY, '/dev/null', baseline={1, 2}, offset=offset)
        watch.rows = lambda: list(self.rows)
        return watch
    def tearDown(self):
        self.temp.cleanup()

    def clicks(self):
        return [e for e in self.target.pointer_events if e[2] is True]

    def drive(self, watch=None, request=None):
        """drive() twice, the second observation 1 s later (the settle rule)."""
        watch, request = watch or self.watch, request or self.request
        reason = consent.drive(watch, request)
        if reason == SETTLE:
            watch.stable = watch.stable[:3] + (watch.stable[3] - 1,)
            reason = consent.drive(watch, request)
        return reason

    def test_foreign_or_preexisting_dialog_window_is_not_used(self):
        self.windows = [dict(DIALOG, pid=999)]  # same title, another process
        self.assertIn('found 0', self.drive())
        self.windows = [dict(DIALOG, id=2)]  # backend's window, but present before prepare
        self.assertIn('found 0', self.drive())
        self.shares = [dict(SHARE, window_id=2)]
        self.windows = [DIALOG]  # accept button drawn in a different window of the backend
        self.assertIn('not uniquely drawn', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_physical_desktop_is_refused(self):
        self.target.physical = object()
        with self.assertRaisesRegex(RecipeBlocked, 'disposable lab guests'):
            self.drive()
        self.assertEqual(self.target.pointer_events, [])

    def test_missing_compositor_witness_blocks(self):
        self.ctx.profile = dict(self.ctx.profile); self.ctx.profile.pop('compositor')
        with self.assertRaisesRegex(RecipeBlocked, 'compositor witness'):
            self.drive()
        self.assertEqual(self.target.pointer_events, [])

    def test_ready_after_click_requires_the_portal_to_have_accepted(self):
        scope = SimpleNamespace(ctx=SimpleNamespace(call=lambda m, p: {'state': 'ready'}))
        watch = SimpleNamespace(clicked=True, consent=POLICY, state=lambda: consent.analyse([public_start(), impl_start()]))
        with self.assertRaisesRegex(RecipeBlocked, 'without the portal accepting'):
            consent.await_ready(scope, 'rec', watch)
        watch.state = lambda: consent.analyse([public_start(), impl_start(), response()])
        self.assertEqual(consent.await_ready(scope, 'rec', watch)['state'], 'ready')


class LocalContext:
    def __init__(self, root, out):
        self.target = SimpleNamespace(root=root); self.out = out; self.notes = []
        self.recipe = SimpleNamespace(name='review.playback'); self._deadline = time.monotonic() + 120
    def shell(self, script, timeout=60):
        p = subprocess.run(script, shell=True, capture_output=True, text=True, timeout=timeout)
        return dict(returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)
    def note(self, text):
        self.notes.append(text)


class Retention(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='sd-media-'); base = Path(self.temp.name)
        self.root = base / 'target'; self.root.mkdir(mode=0o700)
        self.out = base / 'run'; self.out.mkdir(mode=0o700)
        self.ctx = LocalContext(str(self.root), self.out)
        self.scope = Scope(self.ctx); self.scope.initialize()
    def tearDown(self):
        self.scope.close(); self.temp.cleanup()

    def test_media_is_copied_hashed_and_private_before_cleanup(self):
        clip = os.urandom(40 * 1024 + 7)
        self.scope.write(self.scope.root + '/working/rec.mkv', clip)
        self.scope.write(self.scope.root + '/out/review-check.mp4', b'mp4 bytes')
        self.scope.write(self.scope.root + '/cover.py', b'not media')
        os.symlink('/etc/passwd', self.scope.root + '/out/link.mp4')
        media.register(self.scope)
        with patch.object(media, 'CHUNK', 16 * 1024):  # several chunks per clip
            self.assertEqual(self.scope.close(), [])
        self.assertFalse(Path(self.scope.root).exists())
        [manifest_path] = (self.out / 'private-media' / 'review.playback').glob('*/manifest.json')
        manifest = json.loads(manifest_path.read_text())
        self.assertEqual([f['path'] for f in manifest['files']], ['out/review-check.mp4', 'working/rec.mkv'])
        copy = manifest_path.parent / 'working' / 'rec.mkv'
        self.assertEqual(hashlib.sha256(copy.read_bytes()).hexdigest(), hashlib.sha256(clip).hexdigest())
        self.assertEqual(manifest['files'][1]['sha256'], hashlib.sha256(clip).hexdigest())
        self.assertEqual(copy.stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.out / 'private-media').stat().st_mode & 0o777, 0o700)

    def test_caps_hash_but_do_not_copy_oversized_media(self):
        self.scope.write(self.scope.root + '/working/big.mkv', b'x' * 2048)
        with patch.object(media, 'FILE_CAP', 1024):
            manifest = media.retain(self.scope)
        [row] = manifest['files']
        self.assertFalse(row['retained']); self.assertIn('per-file cap', row['reason'])
        self.assertEqual(row['sha256'], hashlib.sha256(b'x' * 2048).hexdigest())
        self.ctx._retained_media_bytes = media.RUN_CAP
        manifest = media.retain(self.scope)
        self.assertIn('per-run cap', manifest['files'][0]['reason'])

    def test_every_created_directory_level_is_private(self):
        self.scope.write(self.scope.root + '/out/videos/clip.mp4', b'mp4')
        os.umask(old := os.umask(0o002))
        try:
            manifest = media.retain(self.scope)
        finally:
            os.umask(old)
        self.assertTrue(manifest['files'][0]['retained'])
        path = self.out / manifest['files'][0]['host_path']
        level = path.parent
        while level != self.out:
            self.assertEqual(level.stat().st_mode & 0o777, 0o700, level)
            level = level.parent

    def test_unignored_run_directory_inside_checkout_is_refused(self):
        ctx = SimpleNamespace(out=ROOT / 'tools' / 'lab' / 'never-a-run')
        directory, reason = media.private_root(ctx)
        self.assertIsNone(directory)
        self.assertIn('not git-ignored', reason)


if __name__ == '__main__':
    unittest.main()
