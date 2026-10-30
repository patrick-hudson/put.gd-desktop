"""Observation false positives (#910/#853) and host OCR preflight (#902).

Synthetic unit evidence only. UI strings and editor.view shape follow
TrayController/MainWindow and EditorWindow::viewState, not a public-text allowlist.
"""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.lab.surface_driver import redact, run as runner
from tools.lab.surface_driver.api import RecipeBlocked
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.redact import Redactor, ocr_detected
from surface_live_fixture import EVIDENCE, FakeDesktop, admitted, control, owned_window
from test_surface_redact import MemoryTarget
from PIL import Image


UI_TEXT = ('Shared link', 'Capture region', 'put.gd version 26.1030.1-nightly',
           'Save diagnostics for this upload', 'Show in folder', 'Sans Serif',
           'Keep editing', 'Image being edited')
EDITOR_VIEW = {'view': {'style': {'font': 'Sans Serif', 'fontPixels': 28, 'color': '#dc2626'},
                        'cancelLabel': 'Keep editing',
                        'destination': {'text': 'Local only', 'sharingMode': 'Shared link'},
                        'keyboardHelp': {'open': True, 'text': 'Image being edited'}}}
LINK = 'https://put.gd/Fk4Rw8Lq2Zn7Hc5T.png'
TOKEN = 'Fk4Rw8Lq2Zn7Hc5T'
KEY = 'AKIA' + 'SYNTHETICKEY0000'
SECRET = 'wJalrXUtnFEMI/K7MDENG/bPxRfiCYVALIDATION1'


class Observations(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='efo-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='g',
                        package='source', destination='local', build='abcdef012345')
        self.profile = {'session': 'x11', 'at_spi': {'available': True}, 'evidence': dict(EVIDENCE)}
        for item in (patch.object(redact.shutil, 'which', return_value='unit-ocr'),
                     patch.object(redact, 'ocr_text', return_value='\n'.join(UI_TEXT))):
            item.start()
            self.addCleanup(item.stop)
        self.ctx = DriverContext(MemoryTarget(), self.env, self.profile, self.root, self.out)

    def mcp(self, value):
        self.ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': json.dumps(value), 'stderr': ''}
        return self.ctx.mcp('editor_view')

    def notifications(self, summary, body, actions=()):
        self.ctx.target.monitor_pid = 1
        self.ctx._notifications_since = 0
        raw = ('method call time=100.1 sender=:1.2 serial=45 '
               'interface=org.freedesktop.Notifications; member=Notify\n')
        raw += ''.join(' string '+json.dumps(s)+'\n' for s in ('put.gd', 'putgd', summary, body))
        raw += ' array [\n'+''.join(' string '+json.dumps(s)+'\n' for s in actions)+' ]\n'
        reply = {'ok': True, 'result': {'alive': True, 'text': raw}}
        self.ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': json.dumps(reply), 'stderr': ''}
        return self.ctx.notifications()

    def test_mixed_editor_and_notifications_keep_earlier_frames(self):
        frame = self.ctx.frame('before-mcp-and-notifications')
        self.assertEqual(self.mcp(EDITOR_VIEW), EDITOR_VIEW)
        # Legacy MCP text-only responses must be inspected recursively too.
        self.mcp({'content': [{'type': 'text', 'text': json.dumps(EDITOR_VIEW)}]})
        rows = self.notifications(UI_TEXT[0], UI_TEXT[2],
                                  ('capture', UI_TEXT[1], 'save-diagnostics', UI_TEXT[3], 'show-in-folder', UI_TEXT[4]))
        self.assertEqual(rows[0]['summary'], 'Shared link')
        self.assertEqual(self.ctx.evidence.redactor.audit_literals, frozenset())
        self.assertFalse(self.ctx._sensitive_seen)
        for text in UI_TEXT:
            self.assertEqual(ocr_detected(text, self.ctx.evidence.redactor.audit_literals), [])
        result = self.ctx.evidence.scan()
        self.assertTrue(result['passed'], result)
        self.assertEqual(result['quarantine'], [])
        with Image.open(self.out/frame.path) as image:
            self.assertIsNotNone(image.getbbox())

    def test_observation_text_is_scanned_without_a_public_text_allowlist(self):
        arbitrary_text = 'User annotation contains ordinary prose and a font choice'
        self.mcp({'view': {'text': arbitrary_text, 'style': {'font': 'Another Font Family'}}})
        self.assertEqual(self.ctx.evidence.redactor.audit_literals, frozenset())
        # Arbitrary fields/labels are not trusted when their contents are secret-shaped.
        self.mcp({'view': {'style': {'font': LINK}}})
        self.assertIn(TOKEN, self.ctx.evidence.redactor.audit_literals)

    def test_links_and_credential_substrings_registered_from_every_observation_channel(self):
        values = (LINK, 'f005.backblazeb2.com/file/synthetic-bucket/'+TOKEN+'.png',
                  'bucket.s3.us-west-004.backblazeb2.com/'+TOKEN+'.png?X-Amz-Signature='+SECRET,
                  'objects.fixture.internal:9000/bucket/'+TOKEN+'.png',
                  'Authorization: Bearer '+SECRET, 'X-PM-UID: '+SECRET, KEY, SECRET,
                  'ghp_syntheticFixtureToken1234567890',
                  'eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJzeW50aGV0aWMifQ.c3ludGhldGljc2lnbmF0dXJl')
        for channel in ('mcp', 'notifications'):
            for value in values:
                with self.subTest(channel=channel, value_index=values.index(value)):
                    self.ctx.evidence.redactor = Redactor()
                    self.ctx.evidence.redactor.add_link_hosts(['objects.fixture.internal'])
                    body = 'Capture region: '+value
                    if channel == 'mcp':
                        self.mcp({'content': [{'type': 'text', 'text': body}]})
                    else:
                        self.notifications('Shared link', body, ('capture', 'Capture region'))
                    literals = self.ctx.evidence.redactor.audit_literals
                    self.assertTrue(literals)
                    self.assertNotIn(body, literals)
                    self.assertNotIn('Shared link', literals)
                    self.assertNotIn('Capture region', literals)
                    self.assertIn('registered-secret', ocr_detected(value, literals))

    def test_live_late_link_still_quarantines_registered_token_anywhere(self):
        for channel in ('mcp', 'notifications'):
            with self.subTest(channel=channel), admitted():
                ctx = DriverContext(MemoryTarget(), dict(self.env, destination='s3'), self.profile,
                                    self.root, self.root/channel)
                self.ctx = ctx
                path = ctx.out/'frames/before.png'
                with patch.object(redact, 'ocr_text', return_value='Image being edited '+TOKEN):
                    ctx.evidence.frame(Image.new('RGB', (120, 80), 'white'), path, [])
                if channel == 'mcp':
                    self.mcp({'view': {'message': {'text': LINK}}})
                else:
                    self.notifications('Shared link', LINK)
                result = ctx.evidence.scan()
                self.assertFalse(result['passed'])
                self.assertIn('png-text-registered-secret', {f['kind'] for f in result['findings']})
                self.assertFalse(path.exists())
                self.assertTrue(result['quarantine'])

    def test_secret_controls_and_link_rows_still_masked(self):
        self.ctx._probe = FakeDesktop([owned_window()], [
            control('credentialSecretKey', [10, 10, 40, 10], SECRET),
            control('historyLink', [10, 30, 40, 10], LINK, role='label')])
        self.ctx._observed({'role': 'password text', 'protected': True, 'text': SECRET})
        regions = self.ctx._secret_regions(0)
        self.assertIn([8, 8, 44, 14], regions)
        self.assertIn([8, 28, 44, 14], regions)
        self.assertIn(SECRET, self.ctx.evidence.redactor.audit_literals)
        self.mcp({'content': [{'type': 'text', 'text': json.dumps({'sessionToken': SECRET})}]})
        self.assertNotIn(SECRET, self.ctx.evidence.redactor.text('error '+SECRET))

    def test_nested_mcp_secret_controls_register_values_without_their_ui_labels(self):
        view = {'view': {'controls': [{'role': 'password text', 'protected': True,
                                      'name': 'Secret access key', 'text': SECRET}]}}
        for value in (view, {'content': [{'type': 'text', 'text': json.dumps(view)}]}):
            self.ctx.evidence.redactor = Redactor()
            self.mcp(value)
            self.assertIn(SECRET, self.ctx.evidence.redactor.audit_literals)
            self.assertNotIn('Secret access key', self.ctx.evidence.redactor.audit_literals)

    def test_bearer_token_with_colon_registers_the_complete_value(self):
        token = 'synthetic:credential-token-12345'
        self.mcp({'view': {'message': {'text': 'Bearer '+token}}})
        self.assertIn(token, self.ctx.evidence.redactor.audit_literals)

    def test_late_credential_marker_quarantines_bare_token_pixels(self):
        path = self.ctx.out/'frames/before-credential.png'
        with patch.object(redact, 'ocr_text', return_value='Image being edited '+SECRET):
            self.ctx.evidence.frame(Image.new('RGB', (120, 80), 'white'), path, [])
        self.notifications('Shared link', 'Authorization: Bearer '+SECRET)
        result = self.ctx.evidence.scan()
        self.assertFalse(result['passed'])
        self.assertIn('png-text-registered-secret', {f['kind'] for f in result['findings']})
        self.assertFalse(path.exists())


class HostOcrPreflight(unittest.TestCase):
    def test_local_context_refuses_missing_host_ocr_before_output_or_target_access(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)/'new'
            env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', package='source', destination='local')
            with patch.object(redact.shutil, 'which', return_value=None), self.assertRaises(RecipeBlocked) as error:
                DriverContext(object(), env, {}, temporary, out)
            self.assertIn('driver host, not only in the guest', str(error.exception))
            self.assertIn('tesseract-ocr', str(error.exception))
            self.assertFalse(out.exists())

    def test_local_run_refuses_missing_host_ocr_before_creating_output_or_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)/'new'
            args = ['--desktop', 'mint-cinnamon-x11', '--host', 'boat', '--placement', 'guest',
                    '--run', 'unused', '--package', 'source', '--destination', 'local', '--recipes', 'core_demo',
                    '--catalog', temporary, '--build', 'abcdef012345', '--out', str(out)]
            with patch.object(redact.shutil, 'which', return_value=None), patch.object(runner, 'Target') as target, \
                    self.assertRaises(SystemExit) as error:
                runner.main(args)
            self.assertIn('host OCR unavailable', str(error.exception))
            target.assert_not_called()
            self.assertFalse(out.exists())

    def test_missing_ocr_after_preflight_still_masks_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            from tools.lab.surface_driver.receipts import Evidence
            evidence = Evidence(temporary)
            path = Path(temporary)/'frame.png'
            with patch.object(redact.shutil, 'which', return_value=None):
                metadata = evidence.frame(Image.new('RGB', (120, 80), 'white'), path, [])
                result = evidence.scan()
            self.assertEqual(metadata['guard'], 'ocr-unavailable-fully-masked')
            self.assertFalse(metadata['visible_pixels'])
            self.assertFalse(result['passed'])


if __name__ == '__main__':
    unittest.main()
