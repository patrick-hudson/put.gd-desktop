"""Live-destination evidence rules (PR 726 validation F1-F7, N-a..N-d). Synthetic only.

Every value here is fake. Tests marked real_ocr need tesseract (boat/guest);
everything else runs anywhere in seconds.
"""
import hashlib
import json
from types import SimpleNamespace
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).parent))
from surface_live_fixture import BUILD, COMMIT, EVIDENCE, FakeDesktop, admitted, control, owned_window, pinned, write_catalog
from tools.lab.surface_driver import redact, run as runner
from tools.lab.surface_driver.api import Recipe, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.redact import Redactor, Sensitive, detected, ocr_detected, save_frame
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = ImageDraw = None

SHARE = 'https://put.gd/Fk4Rw8Lq2Zn7Hc5T.png'
SHARE_TOKEN = 'Fk4Rw8Lq2Zn7Hc5T'
B2 = 'https://f005.backblazeb2.com/file/sd-fixture-bkt/Bx3Nq9Wt5Ky2Pm8D.png'
B2_TOKEN = 'Bx3Nq9Wt5Ky2Pm8D'
PRESIGNED = ('https://sd-fixture-bkt.s3.us-west-004.backblazeb2.com/Ps7Lk2Jd9Rf4Gh6Q.png?X-Amz-Algorithm=AWS4-HMAC-SHA256'
             '&X-Amz-Credential=004fixturekey%2F20261030%2Fus-west-004%2Fs3%2Faws4_request'
             '&X-Amz-Security-Token=FwoGZXIvYXdzEFixtureSessionToken&X-Amz-Signature=0a1b2c3d4e5f60718293a4b5c6d7e8f9')
CUSTOM = 'https://objects.sd-fixture.internal:9000/bkt/Cu5Tm0Ep4Nt8Kz1W.png'
CUSTOM_TOKEN = 'Cu5Tm0Ep4Nt8Kz1W'
REAL_OCR = redact.ocr_text
TESSERACT = shutil.which('tesseract')
real_ocr = unittest.skipUnless(shutil.which('tesseract') and Image, 'needs tesseract and python3-pil')


class Target:
    root = '/tmp/sd-live-evidence'
    socket = root+'/runtime/putgd/session.sock'
    physical = None
    monitor_pid = None

    def __init__(self, probe=None):
        self.probe_result = probe

    def cli(self, *args, **kwargs):
        return {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': {'state': 'ok'}}), 'stderr': ''}

    def execute(self, *args, **kwargs):
        return {'returncode': 0, 'stdout': '', 'stderr': ''}

    def capture_image(self, head=0):
        return Image.new('RGB', (120, 80), 'white')

    def probe(self, name, query, source, timeout=15):
        result = self.probe_result(name, query) if callable(self.probe_result) else self.probe_result
        return {'ok': True, 'result': result}


class Base(unittest.TestCase):
    destination = 's3'
    admit = True  # unit contexts skip the real gate; gate tests turn this off

    def setUp(self):
        if Image is None:
            self.skipTest('requires python3-pil')
        patcher = patch.object(redact, 'ocr_text', return_value='')
        patcher.start(); self.addCleanup(patcher.stop)
        patcher = patch.object(redact.shutil, 'which', return_value='unit-ocr')
        patcher.start(); self.addCleanup(patcher.stop)
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.catalog = write_catalog(self.path/'catalog')
        for patcher in ([admitted()] if self.admit else []) + [pinned(self.catalog)]:
            patcher.start(); self.addCleanup(patcher.stop)
        self.out = self.path/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='g', package='deb',
                        destination=self.destination, build=BUILD)
        self.quarantines = []
        original = redact.tempfile.mkdtemp
        def mkdtemp(*args, **kwargs):
            directory = original(*args, **kwargs); self.quarantines.append(directory); return directory
        patcher = patch.object(redact.tempfile, 'mkdtemp', side_effect=mkdtemp)
        patcher.start(); self.addCleanup(patcher.stop)
        self.addCleanup(lambda: [shutil.rmtree(d, ignore_errors=True) for d in self.quarantines])

    def context(self, target=None, evidence=None):
        profile = {'session': 'x11', 'catalog_tolerance': .025, 'at_spi': {'available': True},
                   'evidence': dict(EVIDENCE if evidence is None else evidence)}
        return DriverContext(target or Target(), self.env, profile, self.catalog, self.out)

    def window(self, ctx, rect=(20, 10, 80, 60), role='history', page=0):
        ctx.geometry = lambda name: {'x': rect[0], 'y': rect[1], 'width': rect[2], 'height': rect[3],
                                     'scale': 1, 'window_id': '10', 'pid': 7}
        ctx.call = lambda method, params=None: {'main': {'page': page}} if method == 'ui.viewState' else {}

    def pixels(self, ctx, frame):
        return Image.open(self.out/frame.path).convert('RGB')

    def leaked(self, *needles):
        found = []
        for path in self.out.rglob('*'):
            if path.is_file():
                data = path.read_bytes()
                found += [(path.name, n) for n in needles if n.encode() in data]
        return found


class F1OwnedWindowOnly(Base):
    def test_live_window_frame_crops_and_masks_foreign_windows_and_notification_area(self):
        # A browser window stacked over the right half of the owned window, and
        # the notification area in the head's top-right corner (110..120, 0..10).
        ctx = self.context()
        self.window(ctx, rect=(20, 0, 100, 80))
        ctx._probe = FakeDesktop([owned_window((20, 0, 100, 80), stack=0),
                                  {'id': '30', 'title': 'Shared link - Firefox', 'pid': 99, 'owned': False,
                                   'rect': [70, 20, 50, 30], 'stack': 1}], [control('historyLink', [25, 70, 10, 4])])
        frame = ctx.frame('history', window='history')
        meta = ctx.evidence.frames[frame.path]
        self.assertFalse(meta['fallback'])
        image = self.pixels(ctx, frame)
        self.assertEqual(image.size, (100, 80))
        self.assertEqual(image.getpixel((10, 40)), (255, 255, 255))      # owned pixels kept
        self.assertEqual(image.getpixel((60, 30)), (0, 0, 0))            # foreign window above masked
        self.assertEqual(image.getpixel((95, 5)), (0, 0, 0))             # notification area masked

    def test_window_below_the_owned_window_is_not_masked(self):
        ctx = self.context()
        self.window(ctx, rect=(20, 0, 100, 80))
        ctx._probe = FakeDesktop([{'id': '30', 'title': 'Firefox', 'pid': 99, 'owned': False, 'rect': [0, 0, 120, 80], 'stack': 0},
                                  owned_window((20, 0, 100, 80), stack=1)], [control('historyLink', [25, 70, 10, 4])])
        frame = ctx.frame('history', window='history')
        self.assertEqual(self.pixels(ctx, frame).getpixel((60, 30)), (255, 255, 255))

    def test_live_desktop_frame_is_fully_masked_without_a_declaration(self):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        frame = ctx.frame('desktop')
        self.assertIsNone(self.pixels(ctx, frame).getbbox())
        self.assertEqual(ctx.evidence.frames[frame.path]['policy'], 'live-desktop-masked')
        self.assertEqual(ctx.reviews, [])  # policy, not a failure

    def test_declared_desktop_still_masks_notifications_browser_chrome_and_leaking_titles(self):
        ctx = self.context()
        ctx.sensitive(SHARE)
        ctx._probe = FakeDesktop([owned_window((0, 20, 40, 40)),
                                  {'id': '30', 'title': SHARE_TOKEN+'.png - Firefox', 'pid': 99, 'owned': False,
                                   'rect': [50, 30, 60, 40], 'stack': 1}])
        frame = ctx.frame('desktop', desktop_safe='synthetic desktop')
        image = self.pixels(ctx, frame)
        self.assertEqual(image.getpixel((115, 5)), (0, 0, 0))     # notification area
        self.assertEqual(image.getpixel((60, 35)), (0, 0, 0))     # browser chrome (top 12 px)
        self.assertEqual(image.getpixel((60, 60)), (255, 255, 255))  # browser page body kept
        self.assertEqual(image.getpixel((5, 78)), (0, 0, 0))      # window list shows a leaking title
        self.assertEqual(image.getpixel((5, 5)), (255, 255, 255))

    def test_declared_desktop_masks_foreign_windows_except_under_allowed_ones(self):
        # A foreign app window carrying links (the GNOME live case), partly
        # under an owned window; the shell's own window and the desktop stay.
        ctx = self.context(evidence=dict(EVIDENCE, shell_processes=['gnome-shell'], shell_areas=[{'anchor': 'left', 'width': 8}]))
        ctx.sensitive(SHARE)
        ctx._probe = FakeDesktop([
            {'id': '5', 'title': '', 'pid': 3, 'process': 'gnome-shell', 'owned': False, 'rect': [0, 0, 120, 8], 'stack': 9},
            {'id': '20', 'title': 'sd726 foreign window', 'pid': 99, 'process': 'python3', 'owned': False,
             'rect': [10, 20, 80, 40], 'stack': 1},
            owned_window((50, 30, 60, 40), stack=2)])
        frame = ctx.frame('desktop', desktop_safe='synthetic desktop')
        image = self.pixels(ctx, frame)
        self.assertEqual(image.getpixel((20, 30)), (0, 0, 0))          # foreign window masked
        self.assertEqual(image.getpixel((60, 50)), (255, 255, 255))    # owned window above it kept
        self.assertEqual(image.getpixel((40, 4)), (255, 255, 255))     # shell-drawn top bar kept
        self.assertEqual(image.getpixel((5, 70)), (255, 255, 255))     # desktop background kept

    def test_declared_desktop_keeps_shell_chrome_over_a_foreign_background(self):
        # GNOME's desktop-icons window covers the screen; the top bar and dock
        # the shell draws over it stay, the rest of that window is masked.
        ctx = self.context(evidence=dict(EVIDENCE, shell_areas=[{'anchor': 'top', 'height': 8}, {'anchor': 'left', 'width': 8}]))
        ctx.sensitive(SHARE)
        ctx._probe = FakeDesktop([{'id': '2', 'title': 'Desktop Icons', 'pid': 40, 'process': 'gjs', 'owned': False,
                                   'rect': [0, 0, 120, 80], 'stack': 0}, owned_window((60, 30, 40, 30), stack=1)])
        image = self.pixels(ctx, ctx.frame('desktop', desktop_safe='synthetic desktop'))
        self.assertEqual(image.getpixel((50, 4)), (255, 255, 255))    # top bar
        self.assertEqual(image.getpixel((4, 50)), (255, 255, 255))    # dock
        self.assertEqual(image.getpixel((30, 50)), (0, 0, 0))         # foreign background window
        self.assertEqual(image.getpixel((80, 45)), (255, 255, 255))   # owned window above it

    def test_rectangle_subtraction(self):
        from tools.lab.surface_driver.context import subtract
        area = lambda pieces: sum(w*h for _, _, w, h in pieces)
        self.assertEqual(subtract([0, 0, 10, 10], []), [[0, 0, 10, 10]])
        self.assertEqual(area(subtract([0, 0, 10, 10], [[2, 2, 3, 3]])), 91)
        self.assertEqual(subtract([0, 0, 10, 10], [[-5, -5, 30, 30]]), [])
        self.assertEqual(area(subtract([0, 0, 10, 10], [[5, 0, 10, 10], [0, 5, 10, 10]])), 25)

    def test_ocr_matching_normalizes_whitespace_and_glyphs(self):
        # Tesseract split the link and read 5 as S and 0 as O (validator frames).
        self.assertIn('url', ocr_detected('Link copied https: //put.gd/ Fk4Rw8Lq2Zn7Hc5T.png'))
        self.assertIn('url', ocr_detected('f005.backblazeb2.com/ file/sd-fixture-bkt/x.png'))
        self.assertIn('registered-secret', ocr_detected('Secret: wJalrXUtnFEMl/K7MDENG/bPxRfiCYVAL72GKEY1',
                                                        ('wJalrXUtnFEMI/K7MDENG/bPxRfiCYVAL726KEY1',)))
        self.assertIn('registered-secret', ocr_detected('see Fk4Rw8 Lq2Zn7HcST', (SHARE_TOKEN,)))
        self.assertNotIn('registered-secret', ocr_detected('History Destinations Settings Copy link', (SHARE_TOKEN,)))

    @real_ocr
    def test_real_ocr_reads_small_banner_text(self):
        # The validator's banner case with the text inside the 40 px canvas.
        path = next(p for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
                                '/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf') if Path(p).is_file())
        results = {}
        for link, literals in ((SHARE, ()), (B2.split('://')[1], ()), (B2, (B2_TOKEN,))):
            for size in (11, 12, 13):
                for foreground, background in (('white', '#2b2b2b'), ('black', 'white')):
                    image = Image.new('RGB', (560, 40), background)
                    ImageDraw.Draw(image).text((10, 12), 'Link copied: '+link, font=ImageFont.truetype(path, size), fill=foreground)
                    with patch.object(redact, 'ocr_text', REAL_OCR):
                        meta = save_frame(image, self.path/'banner.png', [], literals=literals, ocr=TESSERACT)
                    results[(link[:12], size, foreground)] = meta['guard']
        self.assertEqual(set(results.values()), {'ocr-sensitive-text-fully-masked'}, results)


class F2ProbeRegistration(Base):
    destination = 'local'

    def test_persistent_probe_text_is_registered_and_marks_sensitive(self):
        ctx = self.context(Target(probe=[{'name': B2.split('://')[1], 'text': B2.split('://')[1], 'role': 'label'}]))
        self.assertFalse(ctx._sensitive_seen)
        ctx.accessible({'name': 'historyLink'})
        self.assertTrue(ctx._sensitive_seen)
        self.assertIn(B2.split('://')[1], ctx.evidence.redactor.audit_literals)

    def test_secret_field_value_from_the_desktop_is_registered_opaque(self):
        secret = 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY'
        ctx = self.context(Target(probe=[{'name': 'Secret access key', 'objectName': 'credentialSecretKey',
                                          'role': 'text', 'text': secret}]))
        ctx.accessible({'owned': True})
        self.assertIn(secret, ctx.evidence.redactor.audit_literals)
        self.assertNotIn('Secret access key', ctx.evidence.redactor.audit_literals)

    def test_scheme_less_history_link_in_failure_never_reaches_receipt_or_coverage(self):
        ctx = self.context(Target(probe=[{'name': B2}]))
        def recipe(c):
            raise RecipeFailed('History shows '+c.accessible({'name': 'historyLink'})[0]['name'].split('://')[1])
        runner.run_recipes(ctx, [Recipe(('editor.undo',), recipe)], self.env, self.out, 'f2', self.out/'coverage.json')
        self.assertEqual(self.leaked(B2_TOKEN), [])


class F3Patterns(Base):
    def test_object_store_links_and_signature_markers(self):
        for text in ('see '+B2.split('://')[1], 'url='+PRESIGNED.split('://')[1],
                     'bkt.s3.eu-central-1.amazonaws.com/key.png', 'x.r2.cloudflarestorage.com/k/v.png',
                     's3.sd-fixture.internal/bkt/key.png'):
            self.assertIn('url', detected(text), text)
        for text in ('X-Amz-Signature=0a1b2c3d4e5f6071', 'X-Amz-Credential: 004fixturekey/2026',
                     'X-Amz-Security-Token=FwoGZXIvYXdzE', 'Authorization: AWS4-HMAC-SHA256 Credential=x',
                     '?a=1&sig=abcdef0123'):
            self.assertIn('session-or-authorization', detected(text), text)
        for text in ('src/storage/s3/S3Provider.cpp', 'put.gd History', 'tools/lab/surface_driver/run.py'):
            self.assertEqual(detected(text), [], text)

    def test_configured_endpoint_hosts_are_links_without_a_scheme(self):
        redactor = Redactor()
        bare = CUSTOM.split('://')[1]
        self.assertNotIn('url', detected('object '+bare))
        redactor.add_link_hosts(runner.endpoint_hosts({'document': {'profiles': [
            {'settings': {'endpoint': 'https://objects.sd-fixture.internal:9000', 'publicUrlBase': None}}]}}))
        self.assertIn('url', detected('object '+bare, hosts=redactor.link_hosts))
        self.assertNotIn(CUSTOM_TOKEN, redactor.text('object '+bare+' missing'))
        self.assertIn(CUSTOM_TOKEN, ' '.join(redactor.audit_literals))

    def test_presigned_parts_are_redacted_everywhere(self):
        redactor = Redactor()
        text = redactor.text('signed '+PRESIGNED.split('://')[1])
        for part in ('Ps7Lk2Jd9Rf4Gh6Q', '0a1b2c3d4e5f60718293a4b5c6d7e8f9', 'FwoGZXIvYXdzEFixtureSessionToken'):
            self.assertNotIn(part, text)


class F4F7Gate(Base):
    admit = False

    def setUp(self):
        super().setUp()
        patcher = patch.object(redact.shutil, 'which', return_value='unit-ocr')
        patcher.start(); self.addCleanup(patcher.stop)

    def blockers(self, placement='boat', catalog=None, build='abcdef012345', physical=False, selftest=True):
        with patch.object(redact, 'ocr_self_test', return_value=selftest):
            return runner.secret_safe_evidence_blockers(placement, self.catalog if catalog is None else catalog, build, physical)

    def test_complete_disposable_placement_is_admitted(self):
        self.assertEqual(self.blockers('boat'), [])
        self.assertEqual(self.blockers('guest'), [])

    def test_blind_ocr_is_refused(self):
        self.assertIn('ocr-self-test-failed', self.blockers(selftest=False))
        # The real self-test against an OCR that exits 0 and reads nothing.
        with patch.object(redact, 'ocr_text', return_value=''):
            self.assertFalse(redact.ocr_self_test('unit-ocr'))
        with patch.object(redact.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b'', b'')):
            self.assertFalse(redact.ocr_self_test('unit-ocr'))

    def test_catalog_must_be_complete_for_this_build_with_secret_inventories(self):
        self.assertIn('catalog-missing', self.blockers(catalog=self.path/'absent'))
        empty = self.path/'empty'; empty.mkdir()
        self.assertIn('catalog-missing', self.blockers(catalog=empty))
        (empty/'manifest.json').write_text(json.dumps({'kind': 'synthetic-ui-catalog', 'captures': []}))
        self.assertIn('catalog-empty', self.blockers(catalog=empty))
        self.assertIn('catalog-build-mismatch', self.blockers(build='0123456'))
        self.assertIn('catalog-build-mismatch', self.blockers(catalog=write_catalog(self.path/'dirty', dirty=True)))
        self.assertIn('catalog-build-mismatch', self.blockers(catalog=write_catalog(self.path/'nobuild', build='')))
        plain = write_catalog(self.path/'plain', secret=False)
        self.assertIn('catalog-secret-states-differ-from-pin', self.blockers(catalog=plain))
        (self.catalog/'01-history-list.controls.json').unlink()
        self.assertIn('catalog-controls-missing', self.blockers())

    def test_physical_and_native_placements_are_refused(self):
        self.assertIn('placement-not-disposable', self.blockers('native', physical=True))
        self.assertIn('placement-not-disposable', self.blockers(None))
        self.assertIn('physical-desktop-refused', self.blockers('guest', physical=True))

    def test_main_refuses_a_physical_live_run_before_touching_a_target(self):
        out = self.path/'refused'
        args = ['--desktop', 'mint-cinnamon-x11', '--host', 'm74', '--placement', 'native', '--host-profile', str(self.path/'h.json'),
                '--lock-session', 's', '--package', 'deb', '--destination', 'b2', '--recipes', 'core_demo',
                '--catalog', str(self.catalog), '--build','abcdef012345', '--out', str(out)]
        with patch.object(redact, 'ocr_self_test', return_value=True), patch.object(runner, 'Target') as target:
            with self.assertRaises(SystemExit) as raised:
                runner.main(args)
            target.assert_not_called()
        self.assertIn('placement-not-disposable', str(raised.exception))
        self.assertFalse(out.exists())

    @real_ocr
    def test_real_ocr_self_test_reads_eleven_pixel_text(self):
        with patch.object(redact, 'ocr_text', REAL_OCR):
            self.assertTrue(redact.ocr_self_test(TESSERACT))


class GateBuildIdentity(Base):
    """Third validation, gate item 1: --build names one exact clean commit."""
    admit = False

    def test_build_must_be_an_exact_commit_prefix_of_twelve_or_more_hex(self):
        from tools.lab.surface_driver.gate import build_matches
        self.assertTrue(build_matches(BUILD, COMMIT))
        self.assertTrue(build_matches(COMMIT.upper(), COMMIT))
        for expected, commit in (('a', COMMIT), ('abcdef01', COMMIT), ('26.1030.1-nightly', COMMIT),
                                 (BUILD, COMMIT[:12]), (BUILD, None), ('bcdef0123456', COMMIT), (BUILD+'x', COMMIT)):
            self.assertFalse(build_matches(expected, commit), (expected, commit))

    def test_catalog_needs_the_exact_commit_never_a_short_prefix_or_version(self):
        with patch.object(redact, 'ocr_self_test', return_value=True), patch.object(redact.shutil, 'which', return_value='unit-ocr'):
            for build in ('a', 'abcdef01', '26.1030.1-nightly'):
                self.assertIn('catalog-build-mismatch', runner.catalog_blockers(self.catalog, build), build)
            other = write_catalog(self.path/'other', build='f'*40)
            self.assertIn('catalog-build-mismatch', runner.catalog_blockers(other, '26.1030.1-nightly'))
            self.assertEqual(runner.catalog_blockers(self.catalog, BUILD), [])

    def test_main_refuses_short_and_version_builds_before_any_target(self):
        for build in ('2', 'abcdef01', '26.1030.2490-dev'):
            out = self.path/('refused-'+build)
            args = ['--desktop', 'mint-cinnamon-x11', '--host', 'boat', '--placement', 'guest', '--run', str(self.path),
                    '--package', 'deb', '--destination', 'local', '--recipes', 'core_demo', '--catalog', str(self.catalog),
                    '--build', build, '--out', str(out)]
            with patch.object(runner, 'Target') as target, self.assertRaises(SystemExit) as raised:
                runner.main(args)
            target.assert_not_called()
            self.assertIn('--build', str(raised.exception))
            self.assertFalse(out.exists())

    def test_resident_must_report_the_full_commit(self):
        from tools.lab.surface_driver.gate import build_matches
        # The resident check uses the same rule (run.main): a short or
        # version-only resident identity never matches.
        self.assertFalse(build_matches(BUILD, BUILD))
        source = Path(runner.__file__).read_text()
        self.assertIn("build_matches(args.build, observed.get('gitCommit')", source)
        self.assertNotIn("observed.get('version')==args.build", source)


class GateCatalogPin(Base):
    """Gate item 2: every expected secret-control inventory, pinned."""
    admit = False

    def blockers(self, catalog):
        return runner.catalog_blockers(catalog, BUILD)

    def test_deleting_secret_states_with_their_manifest_entries_is_refused(self):
        # The validator's attack: all but one destination state removed, with
        # their manifest entries, so the catalog is self-consistent.
        manifest = json.loads((self.catalog/'manifest.json').read_text())
        manifest['captures'] = [c for c in manifest['captures'] if not c['file'].startswith('18-destination')]
        (self.catalog/'manifest.json').write_text(json.dumps(manifest))
        (self.catalog/'18-destination-credentials-compact.controls.json').unlink()
        self.assertIn('catalog-secret-states-differ-from-pin', self.blockers(self.catalog))

    def test_stripping_a_secret_control_is_refused(self):
        inventory = self.catalog/'04-destinations-s3.controls.json'
        value = json.loads(inventory.read_text())
        value['controls'] = [c for c in value['controls'] if c['name'] != 'credentialSecretKey']
        inventory.write_text(json.dumps(value))
        self.assertIn('catalog-secret-states-differ-from-pin', self.blockers(self.catalog))

    def test_a_declared_inventory_must_exist(self):
        (self.catalog/'01-history-list.controls.json').unlink()
        self.assertIn('catalog-controls-missing', self.blockers(self.catalog))

    def test_committed_pin_lists_every_history_and_credential_state(self):
        from tools.lab.surface_driver import gate
        pin = json.loads(gate.PIN.read_text())['states']
        self.assertGreaterEqual(len(pin), 29)
        names = {n for v in pin.values() for n in v}
        self.assertTrue({'historyLink', 'credentialAccessKey', 'credentialSecretKey', 'credentialSessionToken',
                         'protonSharePassword'} <= names, names)
        self.assertGreaterEqual(sum('credentialSecretKey' in v for v in pin.values()), 7)

    def test_a_self_consistent_but_reduced_catalog_fails_the_real_pin(self):
        # The validator's shape: three inventories, one per page, all present.
        from tools.lab.surface_driver import gate
        real = json.loads(gate.PIN.read_text())['states']
        with patch.object(gate, 'build_classifier', return_value=(real, redact.SECRET_FIELD.pattern, redact.SECRET_FIELD.flags,
                                                                  gate.NOT_SECRET_CLASSES)), \
                patch.object(gate, 'checkout_pin', return_value=real):
            self.assertEqual(self.blockers(self.catalog), ['catalog-secret-states-differ-from-pin'])

    def test_ci_check_catalog_command(self):
        from tools.lab.surface_driver import gate
        self.assertEqual(gate.main(['check-catalog', str(self.catalog)]), 0)
        (self.catalog/'04-destinations-s3.controls.json').write_text(json.dumps({'controls': []}))
        self.assertEqual(gate.main(['check-catalog', str(self.catalog)]), 1)


class GateOcrSelfTest(unittest.TestCase):
    """Gate item 3: a fresh random token per polarity, read exactly."""
    def setUp(self):
        if Image is None:
            self.skipTest('requires python3-pil')

    def drawn(self):
        tokens = []
        original = ImageDraw.ImageDraw.text
        def text(draw, xy, value, *args, **kwargs):
            tokens.append(value)
            return original(draw, xy, value, *args, **kwargs)
        return tokens, patch.object(ImageDraw.ImageDraw, 'text', text)

    def test_replaying_a_fixed_token_fails(self):
        for fixed in ('Link copied: https://put.gd/Kq7Zp3Wx9Ty.png', 'Link copied: https://put.gd/ACEFHKMNPRUV.png'):
            with patch.object(redact, 'ocr_text', return_value=fixed):
                self.assertFalse(redact.ocr_self_test('unit-ocr'))

    def test_each_polarity_and_run_has_a_fresh_token_read_exactly(self):
        tokens, drawing = self.drawn()
        with drawing, patch.object(redact, 'ocr_text', side_effect=lambda image, ocr=None: tokens[-1]):
            self.assertTrue(redact.ocr_self_test('unit-ocr'))
            self.assertTrue(redact.ocr_self_test('unit-ocr'))
        self.assertEqual(len(tokens), 4)
        self.assertEqual(len(set(tokens)), 4)
        self.assertTrue(all(re.fullmatch(r'Link copied: https://put\.gd/[ACEFHKMNPRUVWXY]{12}\.png', t) for t in tokens))
        # One misread glyph in every attempt is a failure, not a fuzzy match;
        # a single misread followed by an exact read of a new token passes.
        tokens.clear()
        def misread(image, ocr=None):
            text = tokens[-1]
            token = text.rsplit('/', 1)[1][:-4]
            return text.replace(token, token[:-1] + ('A' if token[-1] != 'A' else 'C'))
        with drawing, patch.object(redact, 'ocr_text', side_effect=misread):
            self.assertFalse(redact.ocr_self_test('unit-ocr'))
        self.assertEqual(len(tokens), redact.OCR_PROBE_ATTEMPTS)  # first polarity exhausted its new tokens
        tokens.clear()
        answers = iter([False, True, True])
        with drawing, patch.object(redact, 'ocr_text', side_effect=lambda image, ocr=None: tokens[-1] if next(answers) else misread(image)):
            self.assertTrue(redact.ocr_self_test('unit-ocr'))
        self.assertEqual(len(set(tokens)), 3)


class GateEnforcement(Base):
    """Gate item 4: live evidence only through an admitted context."""
    admit = False

    def test_live_context_is_refused_at_construction_before_any_write(self):
        from tools.lab.surface_driver.gate import LiveEvidenceRefused
        with patch.object(redact.shutil, 'which', return_value=None), self.assertRaises(LiveEvidenceRefused) as raised:
            self.context()
        self.assertIn('ocr-unavailable', str(raised.exception))
        self.assertFalse(self.out.exists())

    def test_admitted_live_context_needs_the_complete_gate(self):
        with patch.object(redact, 'ocr_self_test', return_value=True), patch.object(runner, 'prove_disposable'):
            ctx = self.context()
        self.assertTrue(ctx._live_admitted)
        self.env['build'] = 'abcdef01'
        from tools.lab.surface_driver.gate import LiveEvidenceRefused
        with patch.object(redact, 'ocr_self_test', return_value=True), patch.object(runner, 'prove_disposable'), \
                self.assertRaises(LiveEvidenceRefused):
            self.context()

    def test_run_recipes_refuses_contexts_the_gate_did_not_admit(self):
        from types import SimpleNamespace
        from tools.lab.surface_driver.gate import LiveEvidenceRefused
        local = dict(self.env, destination='local')
        ctx = DriverContext(Target(), local, {'session': 'x11', 'at_spi': {'available': True}}, self.catalog, self.out)
        for context, environment in ((ctx, self.env), (SimpleNamespace(destination='s3', _live_admitted=False), self.env),
                                     (SimpleNamespace(destination='s3', _live_admitted=True), dict(self.env, destination='b2'))):
            with self.assertRaises(LiveEvidenceRefused):
                runner.run_recipes(context, [Recipe(('editor.undo',), lambda c: None, destinations=('s3', 'b2'))],
                                   environment, self.out, 'gate', self.out/'coverage.json')
        self.assertFalse((self.out/'receipt.json').exists())

    def test_local_contexts_never_consult_the_gate(self):
        local = dict(self.env, destination='local')
        with patch.object(DriverContext, 'live_gate', staticmethod(lambda *a, **k: self.fail('gate consulted'))):
            DriverContext(Target(), local, {'session': 'x11', 'at_spi': {'available': True}}, self.catalog, self.out)

    def test_the_gate_requires_construction_to_still_run_it(self):
        with patch.object(redact, 'ocr_self_test', return_value=True), \
             patch.object(DriverContext, 'live_gate', staticmethod(lambda *a, **k: [])):
            blockers = runner.secret_safe_evidence_blockers('boat', self.catalog, BUILD)
        self.assertIn('prewrite-frame-gate-not-loaded', blockers)


class SmallTrackedFixes(Base):
    """Trivial parts of the third validation's separate findings (#852, #854)."""
    def test_a_single_row_probe_result_registers_its_secret_field(self):
        ctx = self.context(Target(probe={'role': 'password text', 'protected': True, 'objectName': 'secretKey',
                                         'name': 'Secret access key', 'text': 'Zx9Qw8Er7Ty6Ui5Op4As', 'rect': [1, 1, 5, 5]}))
        ctx._probe('control', {})
        self.assertIn('Zx9Qw8Er7Ty6Ui5Op4As', ctx.evidence.redactor.audit_literals)

    def test_a_label_showing_an_unregistered_key_id_or_marker_is_masked(self):
        ctx = self.context()
        for name in ('Access key AKIAV3VALIDATOR0000X', 'Authorization: Bearer v3tokenvalue'):
            ctx._probe = FakeDesktop([owned_window()], [dict(control('', [10, 10, 60, 10], role='label'), name=name)])
            self.assertTrue(ctx._secret_regions(0), name)

    def test_frame_manifest_records_desktop_safe_and_policy(self):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        declared = ctx.frame('desktop', desktop_safe='reviewer-visible declaration')
        undeclared = ctx.frame('desktop-undeclared')
        frames = json.loads((self.out/'frame-masks.json').read_text())['frames']
        self.assertEqual(frames[declared.path]['desktop_safe'], 'reviewer-visible declaration')
        self.assertEqual(frames[undeclared.path]['policy'], 'live-desktop-masked')


def head_commit():
    import subprocess
    from tools.lab.surface_driver import gate
    commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--', gate.PIN_PATH, gate.REDACT_PATH, gate.GATE_PATH],
                           capture_output=True, text=True).stdout.strip()
    return commit, dirty


class GatePinProvenance(unittest.TestCase):
    """Fourth validation: the pin and classifier come from the build commit."""
    def setUp(self):
        from tools.lab.surface_driver import gate
        self.gate = gate
        self.commit, dirty = head_commit()
        if not gate.COMMIT.fullmatch(self.commit) or dirty:
            self.skipTest('needs a clean git checkout whose HEAD holds the pin and classifier')
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)

    def edited_pin(self, change):
        value = json.loads(self.gate.PIN.read_text())
        change(value['states'])
        path = Path(self.temp.name)/'secret-states.json'
        path.write_text(json.dumps(value))
        return patch.object(self.gate, 'PIN', path)

    def test_the_build_commits_own_pin_and_classifier_are_read(self):
        pin, pattern, flags, classes = self.gate.build_classifier(self.commit)
        self.assertEqual(pin, json.loads(self.gate.PIN.read_text())['states'])
        self.assertEqual((pattern, flags, classes), (redact.SECRET_FIELD.pattern, redact.SECRET_FIELD.flags, self.gate.NOT_SECRET_CLASSES))
        self.assertEqual(self.gate.classifier_blockers(self.commit), (pin, []))

    def test_an_edited_local_pin_is_refused(self):
        # write-pin on a reduced catalog rewrites only the checkout's copy.
        with self.edited_pin(lambda states: [states.pop(k) for k in [k for k in states if 'credential' in k]]):
            self.assertIn('driver-pin-differs-from-build', self.gate.classifier_blockers(self.commit)[1])

    def test_one_control_removed_from_one_state_is_refused(self):
        with self.edited_pin(lambda states: states['04-destinations-s3'].remove('credentialAccessKey')):
            pin, blockers = self.gate.classifier_blockers(self.commit)
        self.assertIn('driver-pin-differs-from-build', blockers)
        self.assertIn('credentialAccessKey', pin['04-destinations-s3'])  # the comparison uses the build's pin

    def test_a_loaded_classifier_that_differs_from_the_build_is_refused(self):
        import re as _re
        with patch.object(redact, 'SECRET_FIELD', _re.compile('password', _re.I)):
            self.assertIn('driver-classifier-differs-from-build', self.gate.classifier_blockers(self.commit)[1])
        with patch.object(self.gate, 'NOT_SECRET_CLASSES', self.gate.NOT_SECRET_CLASSES+('QLineEdit',)):
            self.assertIn('driver-classifier-differs-from-build', self.gate.classifier_blockers(self.commit)[1])

    def test_an_unknown_build_commit_has_no_pin(self):
        self.assertEqual(self.gate.classifier_blockers('f'*40), (None, ['catalog-pin-unavailable']))


class GatePinChangesSelectTheCatalogBuild(unittest.TestCase):
    def test_pin_or_classifier_only_changes_select_the_build_matrix(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('ci_changes', ROOT/'tools/ci/changes.py')
        changes = importlib.util.module_from_spec(spec); spec.loader.exec_module(changes)
        for path in ('tools/lab/surface_driver/secret-states.json', 'tools/lab/surface_driver/redact.py',
                     'tools/lab/surface_driver/gate.py'):
            self.assertTrue(changes.select([path])['build'], path)
        self.assertFalse(changes.select(['tools/lab/surface_driver/context.py'])['build'])


class GatePlacementProof(Base):
    """Fourth validation finding 4: the placement label must be proven."""
    admit = False

    def admitted_otherwise(self):
        return patch.object(redact, 'ocr_self_test', return_value=True)

    def test_a_boat_or_guest_label_on_an_unproven_target_is_refused(self):
        from tools.lab.surface_driver.gate import LiveEvidenceRefused
        for placement in ('boat', 'guest'):
            self.env['placement'] = placement
            with self.admitted_otherwise(), self.assertRaises(LiveEvidenceRefused) as raised:
                self.context()
            self.assertIn('placement-unproven', str(raised.exception))
            self.assertFalse(self.out.exists())

    def test_a_physical_target_needs_a_proven_boat_head(self):
        from tools.lab.surface_driver.gate import LiveEvidenceRefused
        target = Target(); target.physical = SimpleNamespace(config={'host': '192.168.68.74', 'user': 'patrick'})
        for placement, details in (('boat', {}), ('boat', {'boat_sandbox_id': 'bx_abc', 'boat_cli': '/bin/false'}), ('guest', {})):
            self.env['placement'] = placement; self.env['placement_details'] = details
            with self.admitted_otherwise(), self.assertRaises(LiveEvidenceRefused) as raised:
                self.context(target)
            self.assertTrue({'placement-unproven', 'physical-desktop-refused'} & set(str(raised.exception).split(': ', 1)[1].split(', ')))

    def test_proof_receives_the_target_and_details(self):
        seen = []
        self.env['placement_details'] = {'boat_sandbox_id': 'bx_abc'}
        with self.admitted_otherwise(), patch.object(runner, 'prove_disposable', side_effect=lambda *a: seen.append(a)):
            ctx = self.context()
        self.assertTrue(ctx._live_admitted)
        self.assertEqual(seen[0][1:], ('boat', {'boat_sandbox_id': 'bx_abc'}))


class ResidentMustReportClean(Base):
    """Fourth validation finding 2: a resident that omits dirty is refused."""
    destination = 'local'

    def test_resident_without_a_dirty_flag_is_refused(self):
        source = Path(runner.__file__).read_text()
        self.assertIn("if observed.get('dirty') is not False:", source)
        target = Target(); target.info = {'id': 'r', 'profile': 'p'}; target.run = self.path
        target.identity = lambda: {'session_type': 'x11'}; target.start = lambda: None; target.close = lambda: []
        calls, ran = [], []
        def cli(method, *a, **k):
            calls.append(method)
            result = {'ui.snapshot': {'build': {'gitCommit': COMMIT}}}.get(method, {})
            return {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': result}), 'stderr': ''}
        target.cli = cli
        out = self.path/'resident'
        args = ['--desktop', 'mint-cinnamon-x11', '--host', 'boat', '--placement', 'guest', '--run', str(self.path),
                '--package', 'deb', '--destination', 'local', '--recipes', 'core_demo', '--catalog', str(self.catalog),
                '--build', BUILD, '--out', str(out), '--coverage', str(out/'c.json')]
        with patch.object(runner, 'Target', return_value=target), patch.object(runner, 'guard_target'), \
                patch.object(runner, 'discover', return_value=[Recipe(('editor.undo',), lambda c: ran.append(1))]):
            self.assertEqual(runner.main(args), 2)
        self.assertEqual((calls, ran), (['ui.snapshot'], []))


class F6VisiblePageOnly(Base):
    def test_other_pages_secret_controls_do_not_black_out_the_frame(self):
        # Destinations' credentialSecretKey is not showing on the History page.
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx, page=0)
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [control('historyLink', [30, 20, 30, 6], SHARE)])
        frame = ctx.frame('history', window='history')
        meta = ctx.evidence.frames[frame.path]
        self.assertFalse(meta['fallback'], meta)
        image = self.pixels(ctx, frame)
        self.assertEqual(image.getpixel((20, 13)), (0, 0, 0))          # historyLink masked
        self.assertEqual(image.getpixel((5, 50)), (255, 255, 255))     # rest of the page kept

    def test_main_window_page_comes_from_the_selected_navigation_tab(self):
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx, page=None)
        tab = lambda name, selected: {'window': 'put.gd', 'role': 'page tab', 'name': name, 'objectName': '',
                                      'rect': [30+len(name), 12, 20, 6], 'states': {'selected': selected}}
        rows = [tab('History', False), tab('Destinations', True), tab('Settings', False),
                control('credentialSecretKey', [30, 20, 30, 6])]
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], rows)
        self.assertFalse(ctx.evidence.frames[ctx.frame('destinations', window='history').path]['fallback'])
        rows[0], rows[1] = tab('History', True), tab('Destinations', False)
        self.assertTrue(ctx.evidence.frames[ctx.frame('history', window='history').path]['fallback'])
        # Unknown page: no page requirement, every showing editable field masked.
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [control('credentialSecretKey', [30, 20, 30, 6])])
        frame = ctx.frame('unknown-page', window='history')
        self.assertFalse(ctx.evidence.frames[frame.path]['fallback'])
        self.assertEqual(self.pixels(ctx, frame).getpixel((20, 13)), (0, 0, 0))

    def test_qt64_rows_without_object_names_still_mask_every_editable_field(self):
        # Qt 6.4 exports neither objectNames nor a selected tab (Mint guest).
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx, page=None)
        unnamed = lambda rect, role='text', name='': dict(control('', rect, role=role), name=name)
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [
            unnamed([30, 20, 30, 6]), unnamed([30, 40, 30, 6], 'password text'),
            unnamed([30, 60, 30, 6], 'label', 'All items'), unnamed([60, 60, 30, 6], 'list item', SHARE_TOKEN+'.png')])
        frame = ctx.frame('history', window='history')
        image = self.pixels(ctx, frame)
        self.assertFalse(ctx.evidence.frames[frame.path]['fallback'])
        self.assertEqual(image.getpixel((20, 13)), (0, 0, 0))      # editable field
        self.assertEqual(image.getpixel((20, 33)), (0, 0, 0))      # password field
        self.assertEqual(image.getpixel((15, 53)), (255, 255, 255))  # plain label kept
        self.assertEqual(image.getpixel((45, 53)), (0, 0, 0))      # item naming a registered value

    def test_declared_state_requires_as_many_showing_fields_without_object_names(self):
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx)
        ctx._captures['history'] = '01-history-list'   # requires historyLink
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [dict(control('', [30, 20, 30, 6]), name='Link')])
        self.assertFalse(ctx.evidence.frames[ctx.frame('one-field', window='history').path]['fallback'])
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [dict(control('', [30, 20, 30, 6], role='label'), name='Link')])
        self.assertTrue(ctx.evidence.frames[ctx.frame('no-field', window='history').path]['fallback'])

    def test_page_witness_from_catalog_only_labels(self):
        ctx = self.context()
        rows = [{'role': 'label', 'name': 'pageAnchordestinations'}]  # fixture: label only on the destinations capture
        ctx.call = lambda method, params=None: {}
        for inventory in ctx.catalog.inventories.values():
            for c in inventory['controls']:
                c.setdefault('text', c['name'])
        self.assertEqual(ctx._visible_page(rows), 'destinations')
        self.assertIsNone(ctx._visible_page([{'role': 'label', 'name': 'shared'}]))

    def test_main_window_roles_follow_the_view_state_page(self):
        # The fixture's destinations page always shows credentialSecretKey and
        # its history page always shows historyLink; only the visible one counts.
        ctx = self.context()
        ctx.sensitive(SHARE)
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [control('credentialSecretKey', [30, 20, 30, 6])])
        self.window(ctx, page=1)  # destinations is showing, although the role says history
        self.assertFalse(ctx.evidence.frames[ctx.frame('destinations', window='history').path]['fallback'])
        self.window(ctx, page=0)
        self.assertTrue(ctx.evidence.frames[ctx.frame('history', window='history').path]['fallback'])

    def test_declared_catalog_state_requires_its_secret_controls(self):
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx)
        ctx.use_catalog = lambda window, reference: ctx._captures.__setitem__(window, reference)
        ctx.use_catalog('history', '01-history-list')
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))])  # historyLink not readable
        frame = ctx.frame('history', window='history')
        self.assertTrue(ctx.evidence.frames[frame.path]['fallback'])
        self.assertIsNone(self.pixels(ctx, frame).getbbox())

    def test_secret_field_scrolled_past_the_window_edge_is_masked_where_drawn(self):
        # Wayland: a field straddling the viewport edge has no in-frame rect,
        # but the probe keeps its placed rectangle; the crop clips the mask.
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx, page=None)
        row = dict(control('', [0, 0, 1, 1]), rect=None, unclipped_rect=[30, 60, 40, 20], coordinate_error='outside')
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))], [row])
        frame = ctx.frame('destinations', window='history')
        self.assertFalse(ctx.evidence.frames[frame.path]['fallback'])
        self.assertEqual(self.pixels(ctx, frame).getpixel((15, 55)), (0, 0, 0))

    def test_unreadable_window_tree_and_unplaced_secret_fail_closed(self):
        ctx = self.context()
        ctx.sensitive(SHARE)
        self.window(ctx)
        desktop = FakeDesktop([owned_window((20, 10, 80, 60))], [dict(control('historyLink', [0, 0, 1, 1]), rect=None)])
        ctx._probe = desktop
        self.assertTrue(ctx.evidence.frames[ctx.frame('a', window='history').path]['fallback'])
        ctx._probe = lambda name, query: {'windows': [owned_window((20, 10, 80, 60))]} if name == 'layout' else []
        self.assertTrue(ctx.evidence.frames[ctx.frame('b', window='history').path]['fallback'])

    def test_local_frames_before_anything_sensitive_stay_unmasked(self):
        self.env['destination'] = 'local'
        ctx = self.context()
        self.window(ctx)
        ctx._probe = FakeDesktop([owned_window((20, 10, 80, 60))])
        frame = ctx.frame('editor', window='history')
        self.assertEqual(self.pixels(ctx, frame).getpixel((75, 5)), (255, 255, 255))
        desktop = ctx.frame('desktop')
        self.assertIsNotNone(self.pixels(ctx, desktop).getbbox())


class LayoutProbe(unittest.TestCase):
    def test_x11_stacking_follows_the_server_root_order(self):
        from tools.lab.surface_driver import probe
        tree = '''  Root window id: 0x21d (the root window) (has no name)
     3 children:
     0x500 "put.gd": ("putgd-ui" "putgd-ui")  1280x760+0+0  +0+0
     0x300 "Desktop": ("nemo-desktop" "Nemo-desktop")  1280x760+0+0  +0+0
     0x210 (has no name): ()  1280x800+0+0  +0+0
'''
        windows = [{'window_id': str(i), 'name': n, 'pid': pid, 'x': 0, 'y': 0, 'width': 1280, 'height': 760,
                    'frame': None, 'states': {'minimized': False}} for i, n, pid in
                   ((0x500, 'put.gd', 7), (0x300, 'Desktop', 8), (0x210, 'mutter guard window', 9), (0x21d, '', None))]
        with patch.dict(probe.os.environ, {'XDG_SESSION_TYPE': 'x11'}), \
             patch.object(probe, 'run', return_value=tree), patch.object(probe, 'x11_windows', return_value=windows), \
             patch.object(probe, 'x11_ancestors', side_effect=lambda wid: [wid] if wid == str(0x21d) else [wid, str(0x21d)]), \
             patch.object(probe, 'owned', side_effect=lambda pid: pid == 7):
            rows = {r['title']: r for r in probe.layout({})['windows']}
        self.assertGreater(rows['put.gd']['stack'], rows['Desktop']['stack'])
        self.assertGreater(rows['Desktop']['stack'], rows['mutter guard window']['stack'])
        self.assertTrue(rows['put.gd']['owned'])
        self.assertNotIn('', rows)  # the root window is never an occluder


class WaylandGeometryProbe(unittest.TestCase):
    def test_geometry_walk_prunes_hidden_subtrees(self):
        from tools.lab.surface_driver import probe
        seen = []
        row = {'role': 'frame', 'name': 'put.gd', 'visible': True, 'showing': True, 'rect': [0, 0, 10, 10],
               'pid': 7, 'states': {}, 'ancestors': [{'role': 'application'}]}
        with patch.dict(probe.os.environ, {'XDG_SESSION_TYPE': 'wayland'}), \
             patch.object(probe, 'accessible', side_effect=lambda q: seen.append(q) or [row]):
            self.assertEqual(probe.geometry({'title': '^put\\.gd$', 'compositor': {'kind': 'x'}})['pid'], 7)
        self.assertTrue(seen[0]['visible'] and seen[0]['showing'] and seen[0]['owned'])


class NaFinalBytes(Base):
    def test_final_receipt_and_coverage_bytes_are_the_last_scanned(self):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        seen = []
        original = runner.scan_run
        def spy(directory, **kwargs):
            seen.append({name: (Path(directory)/name).read_bytes() for name in ('receipt.json', 'coverage.json')
                         if (Path(directory)/name).exists()})
            return original(directory, **kwargs)
        import tools.lab.surface_driver.receipts as receipts
        with patch.object(runner, 'scan_run', side_effect=spy), patch.object(receipts, 'scan_run', side_effect=spy):
            runner.run_recipes(ctx, [Recipe(('editor.undo',), lambda c: None, destinations=('s3',))], self.env,
                               self.out, 'na', self.path/'shared'/'coverage.json')
        self.assertEqual(seen[-1]['receipt.json'], (self.out/'receipt.json').read_bytes())
        self.assertEqual(seen[-1]['coverage.json'], (self.out/'coverage.json').read_bytes())

    def test_a_value_in_the_rewritten_receipt_is_replaced_by_a_constant_stub(self):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        ctx.sensitive(SHARE)
        original = runner.write_receipts
        def poisoned(out, environment, rows, campaign, coverage, **kwargs):
            original(out, environment, rows, campaign, coverage, **kwargs)
            if kwargs.get('scan') is not None:
                path = Path(out)/'receipt.json'
                path.write_text(path.read_text().replace('"schemaVersion"', '"leak": "'+SHARE_TOKEN+'", "schemaVersion"', 1))
        with patch.object(runner, 'write_receipts', side_effect=poisoned):
            rows = runner.run_recipes(ctx, [Recipe(('editor.undo',), lambda c: None, destinations=('s3',))], self.env,
                                      self.out, 'na', self.out/'coverage.json')
        self.assertEqual(rows[0].functional, 'failed')
        self.assertEqual(self.leaked(SHARE_TOKEN), [])
        self.assertEqual(json.loads((self.out/'receipt.json').read_text())['secret_scan']['findings'][0]['kind'],
                         'final-receipt-audit-failed')


class NbSharedCoverage(Base):
    OTHER = {'interaction': 'tray.open', 'desktop': 'ubuntu-gnome-wayland', 'package': 'deb', 'host': 'boat',
             'placement': 'boat', 'guest': 'gnome', 'destination': 's3', 'recipe': 'other', 'build': 'abcdef',
             'functional': 'passed'}

    def shared(self, entries):
        path = self.path/'shared'/'coverage.json'
        path.parent.mkdir()
        path.write_text(json.dumps({'schemaVersion': 2, 'campaign': 'nb', 'entries': entries}, indent=2))
        return path

    def run_one(self, shared, recipe=lambda c: None):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        return runner.run_recipes(ctx, [Recipe(('editor.undo',), recipe, destinations=('s3',))], self.env,
                                  self.out, 'nb', shared)

    def test_publication_merges_and_keeps_other_runs_rows(self):
        shared = self.shared([self.OTHER])
        rows = self.run_one(shared)
        self.assertEqual(rows[0].functional, 'passed')
        entries = json.loads(shared.read_text())['entries']
        self.assertIn(self.OTHER, entries)
        self.assertIn('editor.undo', {e['interaction'] for e in entries})
        self.assertTrue(json.loads((self.out/'receipt.json').read_text())['environment']['coverage_export']['published'])

    def test_failed_own_export_quarantines_only_this_runs_file(self):
        shared = self.shared([self.OTHER])
        before = shared.read_bytes()
        original = runner.write_json
        def poisoned(path, value, **kwargs):
            original(path, value, **kwargs)
            if Path(path).name == 'coverage-export.json':
                Path(path).write_text(json.dumps({'leak': SHARE}))
        with patch.object(runner, 'write_json', side_effect=poisoned):
            rows = self.run_one(shared)
        self.assertEqual(rows[0].functional, 'failed')
        self.assertEqual(shared.read_bytes(), before)
        self.assertFalse((self.out/'coverage-export.json').exists())

    def test_existing_finding_in_the_shared_file_blocks_publication_untouched(self):
        shared = self.shared([dict(self.OTHER, note='planted '+SHARE)])
        before = shared.read_bytes()
        rows = self.run_one(shared)
        self.assertEqual(rows[0].functional, 'failed')
        self.assertEqual(shared.read_bytes(), before)
        kinds = {f['kind'] for f in json.loads((self.out/'receipt.json').read_text())['secret_scan']['findings']}
        self.assertIn('shared-coverage-has-findings', kinds)
        self.assertFalse((self.out/'.coverage-merged.json').exists())


class NcProfiles(unittest.TestCase):
    def test_no_blanket_address_bar_masks_and_every_profile_declares_areas(self):
        for path in sorted((ROOT/'tools/lab/surface_driver/profiles').glob('*.json')):
            if path.name == 'control-names.json':
                continue
            profile = load_profile(path.stem)
            evidence = profile['evidence']
            self.assertNotIn('browser_address_bars', evidence, path.name)
            self.assertTrue(evidence['notification_areas'], path.name)
            self.assertTrue(evidence['browser']['title'], path.name)
            for area in evidence['notification_areas']:
                self.assertLess(area.get('width', 1), 1000, path.name)


class NdFailureMessages(Base):
    destination = 'local'

    def test_unregistered_scheme_less_links_in_recipe_failures_are_redacted(self):
        ctx = self.context()
        ctx._probe = FakeDesktop()
        def recipe(c):
            raise RecipeFailed('expected files.sd-fixture.example/obj/Un9Rg4St3Rd7Lk1M.png, saw '+
                               str(c.url_evidence('https://put.gd/Ev1De5Nc9Ek3Yv7X.png')))
        rows = runner.run_recipes(ctx, [Recipe(('editor.undo',), recipe)], self.env, self.out, 'nd', self.out/'coverage.json')
        self.assertEqual(rows[0].functional, 'failed')
        self.assertEqual(self.leaked('Un9Rg4St3Rd7Lk1M', 'Ev1De5Nc9Ek3Yv7X'), [])
        self.assertIn("'host': 'put.gd'", rows[0].note)

    def test_url_evidence_contract_is_documented_on_the_api(self):
        from tools.lab.surface_driver.api import Context
        self.assertIn('RecipeFailed', Context.url_evidence.__doc__)


if __name__ == '__main__':
    unittest.main()
