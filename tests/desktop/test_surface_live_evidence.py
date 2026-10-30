"""Live-destination evidence rules (PR 726 validation F1-F7, N-a..N-d). Synthetic only.

Every value here is fake. Tests marked real_ocr need tesseract (boat/guest);
everything else runs anywhere in seconds.
"""
import hashlib
import json
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
from surface_live_fixture import EVIDENCE, FakeDesktop, control, owned_window, write_catalog
from tools.lab.surface_driver import redact, run as runner
from tools.lab.surface_driver.api import Recipe, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.redact import Redactor, Sensitive, detected, ocr_detected, save_frame
try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = None

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
        self.out = self.path/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='g', package='deb',
                        destination=self.destination, build='abcdef')
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
        ctx = self.context(evidence=dict(EVIDENCE, shell_processes=['gnome-shell']))
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
    def setUp(self):
        super().setUp()
        patcher = patch.object(redact.shutil, 'which', return_value='unit-ocr')
        patcher.start(); self.addCleanup(patcher.stop)

    def blockers(self, placement='boat', catalog=None, build='abcdef', physical=False, selftest=True):
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
        self.assertIn('catalog-lacks-history-secret-controls', self.blockers(catalog=plain))
        self.assertIn('catalog-lacks-destinations-secret-controls', self.blockers(catalog=plain))
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
                '--catalog', str(self.catalog), '--build', 'abcdef', '--out', str(out)]
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
