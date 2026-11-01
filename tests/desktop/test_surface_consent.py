#!/usr/bin/env python3
"""ScreenCast consent ownership/refusals (#791), private media retention (#796)
and the GTK sink preflight (#784). Seconds-long; no GUI, VM or session bus."""
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import consent, media, recordings
from tools.lab.surface_driver.fixtures.base import Scope
from tools.lab.surface_driver.probe import PortalFilter, portal_request_path
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


class Filter(unittest.TestCase):
    def setUp(self):
        self.filter = PortalFilter(lambda sender: sender == OURS, lambda name: (4545, 'xdg-desktop-portal-gnome'))

    def test_owned_public_start_records_request_path_without_options(self):
        block = ('method call time=10.5 sender=:1.80 -> destination=org.freedesktop.portal.Desktop serial=12 '
                 'path=/org/freedesktop/portal/desktop; interface=org.freedesktop.portal.ScreenCast; member=Start\n'
                 '   object path "/org/freedesktop/portal/desktop/session/1_80/s"\n   string ""\n   array [\n'
                 '      dict entry(\n         string "handle_token"\n         variant             string "' + TOKEN + '"\n      )\n'
                 '      dict entry(\n         string "restore_token"\n         variant             string "SECRET-ish"\n      )\n   ]\n')
        row = self.filter.record(block)
        self.assertEqual(row['handle'], HANDLE)
        self.assertNotIn('SECRET-ish', json.dumps(row))
        self.assertIsNone(self.filter.record(block.replace('sender=:1.80', 'sender=:1.99')))

    def test_impl_start_carries_backend_process_and_response_code(self):
        block = ('method call time=11 sender=:1.20 -> destination=:1.45 serial=99 path=/org/freedesktop/portal/desktop; '
                 'interface=org.freedesktop.impl.portal.ScreenCast; member=Start\n'
                 '   object path "' + HANDLE + '"\n   object path "/org/freedesktop/portal/desktop/session/1_80/s"\n'
                 '   string "putgd-record"\n   string ""\n   array [\n   ]\n')
        row = self.filter.record(block)
        self.assertEqual((row['handle'], row['backend_exe'], row['app_id']), (HANDLE, 'xdg-desktop-portal-gnome', 'putgd-record'))
        signal = ('signal time=12 sender=:1.20 -> destination=:1.80 serial=5 path=' + HANDLE +
                  '; interface=org.freedesktop.portal.Request; member=Response\n   uint32 0\n   array [\n   ]\n')
        self.assertEqual(self.filter.record(signal)['code'], 0)
        marker = ('signal time=1 sender=:1.99 -> destination=(null destination) serial=2 path=/org/putgd/LabPortalMonitor; '
                  'interface=org.putgd.LabPortalMonitor; member=Marker\n   string "abc"\n')
        # gdbus emit exits before its pid is readable: the random token is the proof.
        self.assertEqual(self.filter.record(marker)['token'], 'abc')


class Ownership(unittest.TestCase):
    def test_our_pending_request_to_declared_backend_may_be_driven(self):
        kind, request = consent.decision(consent.analyse([public_start(), impl_start()]), POLICY)
        self.assertEqual((kind, request['backend_pid']), ('dialog', 4545))

    def test_dialog_from_a_different_request_is_refused(self):
        kind, reason = consent.decision(consent.analyse([public_start(), impl_start(FOREIGN_HANDLE, app='com.obsproject.Studio')]), POLICY)
        self.assertEqual(kind, 'blocked')
        self.assertIn('different request', reason)
        # Even alongside our own pending request.
        kind, reason = consent.decision(consent.analyse([public_start(), impl_start(), impl_start(FOREIGN_HANDLE)]), POLICY)
        self.assertEqual(kind, 'blocked')

    def test_foreign_backend_process_is_refused(self):
        kind, reason = consent.decision(consent.analyse([public_start(), impl_start(exe='evil-portal')]), POLICY)
        self.assertEqual(kind, 'blocked')
        self.assertIn('not a declared portal backend', reason)

    def test_two_pending_requests_of_our_own_are_refused(self):
        second = 'putgd_record_4242_9'
        rows = [public_start(), public_start(token=second), impl_start(),
                impl_start(portal_request_path(OURS, second))]
        kind, reason = consent.decision(consent.analyse(rows), POLICY)
        self.assertEqual(kind, 'blocked')
        self.assertIn('more than one', reason)

    def test_foreign_request_before_the_offset_is_still_foreign(self):
        rows = [impl_start(FOREIGN_HANDLE), public_start(), impl_start()]
        self.assertEqual(consent.analyse(rows, offset=1)['foreign'][0]['handle'], FOREIGN_HANDLE)
        # An earlier capture's own answered request is not pending.
        earlier = portal_request_path(OURS, 'putgd_record_4242_1')
        rows = [public_start(token='putgd_record_4242_1'), impl_start(earlier), response(earlier), public_start(), impl_start()]
        self.assertEqual(consent.decision(consent.analyse(rows, offset=3), POLICY)[0], 'dialog')

    def test_token_from_another_program_is_not_ours(self):
        rows = [public_start(token='other_app_1'), impl_start(portal_request_path(OURS, 'other_app_1'))]
        self.assertEqual(consent.decision(consent.analyse(rows), POLICY)[0], 'blocked')

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

    def test_every_profile_declares_consent_behaviour(self):
        for path in (ROOT / 'tools/lab/surface_driver/profiles').glob('*-*.json'):
            if path.stem == 'control-names':
                continue
            profile = load_profile(path.stem)
            declared = profile['recording']['consent']
            self.assertEqual(declared['kind'], 'none' if profile['session'] == 'x11' else 'portal-dialog', path.stem)
            self.assertTrue(declared['reason'])

    def test_window_targets_keep_the_portal_when_native_capture_is_available(self):
        for native in ('kde-screencast', 'mutter-screencast'):
            caps = {'acquisition': native}
            self.assertEqual(consent.acquisition(caps, {'kind': 'window'}), 'portal')
            for kind in ('region', 'monitor', 'desktop'):
                self.assertEqual(consent.acquisition(caps, {'kind': kind}), native)
        self.assertEqual(consent.acquisition({'acquisition': 'x11'}, {'kind': 'window'}), 'x11')

    def test_start_records_effective_acquisition_without_changing_portal_policy(self):
        for kind, selected in (('region', 'kde-screencast'), ('window', 'portal')):
            notes = []
            ctx = SimpleNamespace(profile={'session': 'wayland', 'recording': {'consent': POLICY}},
                                  call=lambda method: {'acquisition': 'kde-screencast'},
                                  shell=lambda script: {'stdout': ''}, note=notes.append)
            scope = SimpleNamespace(ctx=ctx, values={'_portal_monitor': {'log': '/private/monitor'}})
            watch = consent.start(scope, target={'kind': kind})
            self.assertEqual(watch.acquisition, selected)
            self.assertEqual(watch.consent, POLICY)
            self.assertEqual(notes, ['recording acquisition=' + selected])

    def test_native_ready_needs_no_dialog_and_never_drives_one(self):
        for acquisition in ('kde-screencast', 'mutter-screencast'):
            with self.subTest(acquisition=acquisition):
                scope = SimpleNamespace(ctx=SimpleNamespace(call=lambda m, p: {'state': 'ready'}))
                watch = SimpleNamespace(clicked=False, acquisition=acquisition, consent=POLICY,
                                        state=lambda: consent.analyse([]))
                with patch.object(consent, 'drive', side_effect=AssertionError('native consent click')):
                    self.assertEqual(consent.await_ready(scope, 'rec', watch)['state'], 'ready')
                    # Even a ready session with an already-answered portal
                    # request violates the native no-dialog contract.
                    for state in ('preparing', 'ready'):
                        scope.ctx.call = lambda m, p: {'state': state}
                        for rows in ([public_start(), impl_start()],
                                     [public_start(), impl_start(), response()]):
                            watch.state = lambda: consent.analyse(rows)
                            with self.assertRaisesRegex(RecipeFailed, 'unexpected.*ScreenCast portal'):
                                consent.await_ready(scope, 'rec', watch, timeout=0)


class ProbeQuery(unittest.TestCase):
    def test_coordinates_is_a_placement_input_not_a_row_filter(self):
        from tools.lab.surface_driver.probe import matches
        self.assertTrue(matches({'name': 'Share'}, {'name': 'Share', 'coordinates': 'window', 'compositor': {}}))


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

    def test_proven_dialog_gets_one_guarded_click_on_share_with_frames(self):
        self.assertIsNone(self.drive())
        self.assertEqual(self.clicks(), [(960, 189, True)])
        guard = [q for n, q in self.probes if n == 'foreground'][0]
        self.assertEqual(guard['geometry']['window_id'], 77)
        self.assertEqual(guard['point'], (960.0, 189.0))
        self.assertEqual([f.label for f in self.ctx.frames], ['screencast-consent-before', 'screencast-consent-after'])
        self.assertEqual(self.windows, [])  # after-frame waited for the dialog to close
        self.assertTrue(self.watch.clicked)

    def test_first_observation_only_waits_for_the_dialog_to_settle(self):
        self.assertEqual(consent.drive(self.watch, self.request), SETTLE)
        self.assertEqual(consent.drive(self.watch, self.request), SETTLE)  # < 0.25 s later
        self.assertEqual(self.target.pointer_events, [])

    def test_drive_never_clicks_twice(self):
        self.assertIsNone(self.drive())
        self.windows = [DIALOG]
        with self.assertRaisesRegex(RecipeBlocked, 'never twice'):
            consent.drive(self.watch, self.request)
        self.assertEqual(len(self.clicks()), 1)

    def test_covered_share_button_is_refused_without_input(self):
        self.pick = {'ok': False, 'obscuring_window': 'Terminal', 'obscuring_window_id': 9}
        with self.assertRaisesRegex(RecipeFailed, 'obscured by'):
            self.drive()
        self.assertEqual(self.target.pointer_events, [])
        self.assertFalse(self.watch.clicked)

    def test_foreign_or_preexisting_dialog_window_is_not_used(self):
        self.windows = [dict(DIALOG, pid=999)]  # same title, another process
        self.assertIn('found 0', self.drive())
        self.windows = [dict(DIALOG, id=2)]  # backend's window, but present before prepare
        self.assertIn('found 0', self.drive())
        self.shares = [dict(SHARE, window_id=2)]
        self.windows = [DIALOG]  # accept button drawn in a different window of the backend
        self.assertIn('not uniquely drawn', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_two_new_backend_windows_of_any_title_are_refused(self):
        self.windows = [DIALOG, dict(FOREIGN_DIALOG, title='Something else')]
        self.assertIn('exactly one new window', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_prompt_must_name_this_requests_application(self):
        self.prompts = [dict(FOREIGN_PROMPT, window_id=77)]  # our window id, another app's prompt
        self.assertIn('declared prompt', self.drive())
        self.request = dict(self.request, app_id='')  # no app id: cannot be told apart
        self.assertIn('cannot be told', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_race_foreign_request_logged_before_the_watch_offset(self):
        # Validator race 1 (#817): a foreign Start logged before this capture's
        # offset; only its dialog is drawn yet. 00ac5624 clicked it.
        self.rows = [impl_start(FOREIGN_HANDLE, app='com.obsproject.Studio'), public_start(), self.request]
        watch = self.make_watch(offset=1)
        self.assertEqual(consent.decision(watch.state(), POLICY)[0], 'blocked')
        self.windows, self.shares, self.prompts = [FOREIGN_DIALOG], [FOREIGN_SHARE], [FOREIGN_PROMPT]
        reason = self.drive(watch)
        self.assertIsNotNone(reason)
        self.assertEqual(self.clicks(), [])

    def test_race_foreign_request_after_the_decision(self):
        # Validator race 2 (#817): the decision saw only ours; a foreign request
        # and its dialog arrive before ours is drawn. 00ac5624 clicked it.
        kind, request = consent.decision(consent.analyse(self.rows), POLICY)
        self.assertEqual(kind, 'dialog')
        self.rows.append(impl_start(FOREIGN_HANDLE))
        self.windows, self.shares, self.prompts = [FOREIGN_DIALOG], [FOREIGN_SHARE], [FOREIGN_PROMPT]
        self.assertIsNotNone(self.drive(request=request))
        self.assertEqual(self.clicks(), [])

    def test_race_foreign_request_between_the_guard_and_the_click(self):
        # A foreign Start lands after the compositor pick, before the press.
        def arrive():
            self.rows.append(impl_start(FOREIGN_HANDLE))
        self.on_pick = arrive
        with self.assertRaisesRegex(RecipeBlocked, 'changed before the click'):
            self.drive()
        self.assertEqual(self.clicks(), [])

    def test_new_backend_window_between_the_guard_and_the_click(self):
        def draw():
            self.windows = [DIALOG, FOREIGN_DIALOG]
        self.on_pick = draw
        with self.assertRaisesRegex(RecipeBlocked, 'windows changed'):
            self.drive()
        self.assertEqual(self.clicks(), [])

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

    def test_gtk4_sensitive_button_with_window_coordinates_is_driven(self):
        self.shares = [dict(SHARE, enabled=False, states={'sensitive': True})]
        self.assertIsNone(self.drive())
        query = [q for n, q in self.probes if n == 'accessible' and q.get('name') == 'Share'][0]
        self.assertEqual(query['coordinates'], 'window')  # declared by the GNOME profile

    def test_button_too_small_for_declared_tolerance_is_refused(self):
        self.shares = [dict(SHARE, rect=[923, 172, 74, 16])]
        self.assertIn('placement tolerance', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_disabled_share_or_missing_prompt_is_refused(self):
        self.shares = [dict(SHARE, enabled=False, states={'sensitive': False})]
        self.assertIn('disabled', self.drive())
        self.shares = [SHARE]; self.prompts = []
        self.assertIn('declared prompt', self.drive())
        self.assertEqual(self.target.pointer_events, [])

    def test_backend_window_open_before_prepare_is_refused(self):
        with self.assertRaisesRegex(RecipeBlocked, 'already shows a window'):
            consent.backend_idle(4545, [DIALOG])
        consent.backend_idle(4545, [dict(DIALOG, pid=1)])
        consent.backend_idle(None, [DIALOG])

    def test_ready_wait_reports_an_explicit_blocked_reason_not_a_timeout(self):
        scope = SimpleNamespace(ctx=SimpleNamespace(call=lambda m, p: {'state': 'preparing'}))
        watch = SimpleNamespace(clicked=False, consent=POLICY, state=lambda: consent.analyse([public_start()]))
        with self.assertRaisesRegex(RecipeBlocked, "ready not reached in 0s: this resident's ScreenCast Start was sent"):
            consent.await_ready(scope, 'rec', watch, timeout=0)
        watch.state = lambda: consent.analyse([public_start(), impl_start(), response(code=1)])
        with self.assertRaisesRegex(RecipeBlocked, 'response 1'):
            consent.await_ready(scope, 'rec', watch, timeout=5)
        watch.state = lambda: consent.analyse([public_start(), impl_start(FOREIGN_HANDLE)])
        with self.assertRaisesRegex(RecipeBlocked, 'different request'):
            consent.await_ready(scope, 'rec', watch, timeout=5)

    def test_await_ready_drives_a_request_only_once(self):
        scope = SimpleNamespace(ctx=SimpleNamespace(call=lambda m, p: {'state': 'preparing'}, note=lambda t: None))
        watch = SimpleNamespace(clicked=True, consent=POLICY, refusals=[],
                                state=lambda: consent.analyse([public_start(), impl_start()]))
        with patch.object(consent, 'drive', side_effect=AssertionError('second drive')):
            with self.assertRaisesRegex(RecipeBlocked, 'clicked once but the backend dialog stayed pending'):
                consent.await_ready(scope, 'rec', watch, timeout=0)

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

    def test_retention_has_its_own_budget_and_returns_the_rest(self):
        self.scope.write(self.scope.root + '/working/a.mkv', b'a' * 10)
        self.ctx._deadline = time.monotonic() + 100
        with patch.object(media, 'BUDGET_SECONDS', 0):
            manifest = media.retain(self.scope)
        self.assertIn('time budget', manifest['files'][0]['reason'])
        self.assertGreater(self.ctx._deadline - time.monotonic(), 99)  # later finalizers keep their time

    def test_unignored_run_directory_inside_checkout_is_refused(self):
        ctx = SimpleNamespace(out=ROOT / 'tools' / 'lab' / 'never-a-run')
        directory, reason = media.private_root(ctx)
        self.assertIsNone(directory)
        self.assertIn('not git-ignored', reason)


class ChannelFollow(unittest.TestCase):
    def test_monitor_exits_when_the_probe_channel_process_is_gone(self):
        parent = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
        code = ('import sys,time; sys.path.insert(0, %r)\n'
                'from tools.lab.surface_driver.probe import follow_channel, process_start\n'
                'follow_channel({"parent": {"pid": %d, "start": process_start(%d)}})\n'
                'time.sleep(20)\n') % (str(ROOT), parent.pid, parent.pid)
        monitor = subprocess.Popen([sys.executable, '-c', code])
        try:
            time.sleep(1.5)
            self.assertIsNone(monitor.poll())  # channel alive: keeps running
            parent.kill(); parent.wait()
            self.assertEqual(monitor.wait(timeout=5), -signal.SIGTERM)
        finally:
            for process in (parent, monitor):
                if process.poll() is None:
                    process.kill(); process.wait()

    def test_server_pid_names_this_process(self):
        from tools.lab.surface_driver.probe import server_pid, process_start
        self.assertEqual(server_pid({}), {'pid': os.getpid(), 'start': process_start(os.getpid())})


class SinkPreflight(unittest.TestCase):
    def test_missing_gtksink_blocks_with_the_overlay_staging_path(self):
        scope = SimpleNamespace(ctx=SimpleNamespace(shell=lambda script, timeout=60: {'returncode': 3, 'stdout': '', 'stderr': ''}))
        with self.assertRaisesRegex(RecipeBlocked, 'gstreamer1.0-gtk3.*--recording-overlay') as raised:
            recordings.media_preflight(scope)
        self.assertEqual(raised.exception.issue, 784)

    def test_overlay_mode_stages_gtk_sink_outside_the_golden_tool_list(self):
        script = (ROOT / 'tools/lab/surface_driver/profiles/provision-probes.sh').read_text()
        overlay, golden = script.split('exit 0\nfi\n', 1)
        self.assertIn('--recording-overlay', overlay)
        self.assertIn('gstreamer1.0-gtk3', overlay)
        self.assertIn("find('gtksink')", overlay)
        self.assertNotIn('gstreamer1.0-gtk3', golden)


if __name__ == '__main__':
    unittest.main()
