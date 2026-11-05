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
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed, ReceiptRow
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.redact import Redactor, credential_tokens
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
# HistoryService::prepare and HistoryWindow::deletionConfirmation share this
# public copy. Copying the preparation used to register it after question.png.
CACHE_NOTICE = 'Copies already cached by browsers or a CDN can stay visible for a while'
HISTORY_ID = '385dd675-0ec7-449a-9a43-633d32354461'
HISTORY_NAME = 'source-a1b2c3d4.png'  # fixtures/uploads.py's generated source name
PREPARATION = {'targets': [{'historyId': HISTORY_ID, 'effect': 'Deletes the exact recorded object'}],
               'acknowledgement': '0123456789abcdef' * 4,
               'localCopiesPreserved': True, 'cacheNotice': CACHE_NOTICE}


class Observations(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='efo-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.out = self.root/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='g',
                        package='source', destination='local', build='abcdef012345')
        self.profile = {'session': 'x11', 'at_spi': {'available': True}, 'evidence': dict(EVIDENCE)}
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
        # This scene contains no credential controls. MemoryTarget's default
        # probe tree otherwise includes unrelated synthetic secret fixtures.
        frame = self.ctx.frame('before-mcp-and-notifications')
        self.assertEqual(self.mcp(EDITOR_VIEW), EDITOR_VIEW)
        # Legacy MCP text-only responses must be inspected recursively too.
        self.mcp({'content': [{'type': 'text', 'text': json.dumps(EDITOR_VIEW)}]})
        rows = self.notifications(UI_TEXT[0], UI_TEXT[2],
                                  ('capture', UI_TEXT[1], 'save-diagnostics', UI_TEXT[3], 'show-in-folder', UI_TEXT[4]))
        self.assertEqual(rows[0]['summary'], 'Shared link')
        self.assertEqual(self.ctx.evidence.redactor.audit_literals, frozenset())
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
                    self.assertIn('registered-secret', redact.detected(value, literals))


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


    def clipboard(self, value, **kwargs):
        self.ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': value, 'stderr': ''}
        return self.ctx.clipboard_text(**kwargs)

    def test_history_fixture_identifiers_are_not_credential_shaped(self):
        values = {'name': HISTORY_NAME, 'id': HISTORY_ID, 'operationId': HISTORY_ID,
                  'key': 'campaign-0123456789ab/' + HISTORY_NAME,
                  'sha256': '0123456789abcdef' * 4}
        for value in values.values():
            self.assertEqual(list(credential_tokens(value)), [])
        redactor = Redactor()
        redactor.remember(values)
        self.assertFalse(redactor.audit_literals)

    def test_history_preparation_copy_keeps_public_dialog_text_and_passing_credit(self):
        # Pin the reproduced cause independently of clipboard_text: it is
        # blanket opacity, not the entropy detector, that registers UI copy.
        legacy = Redactor()
        legacy.remember(json.dumps(PREPARATION), opaque=True)
        self.assertIn(CACHE_NOTICE, legacy.audit_literals)
        path = self.out/'frames/question.png'
        self.ctx.evidence.frame(Image.new('RGB', (120, 80), 'white'), path)
        # Whitespace/key order can differ from production without changing JSON.
        copied = json.dumps(PREPARATION, sort_keys=True, indent=2)
        self.assertEqual(self.clipboard(copied, expected_json=PREPARATION), copied)
        self.assertNotIn(CACHE_NOTICE, self.ctx.evidence.redactor.audit_literals)
        result = self.ctx.evidence.scan()
        self.assertTrue(result['passed'], result)
        self.assertEqual(result['quarantine'], [])
        self.assertTrue(path.exists())
        rows = [ReceiptRow('history.delete-remote', self.env['desktop'], 'source', 'passed')]
        coverage = self.out/'coverage.json'
        runner.finish_evidence(self.ctx.evidence, self.out, self.env, rows, 'unit-history', coverage)
        self.assertEqual(rows[0].functional, 'passed')
        self.assertEqual(json.loads(coverage.read_text())['entries'][0]['functional'], 'passed')
        self.assertTrue(json.loads((self.out/'receipt.json').read_text())['secret_scan']['passed'])

    def test_history_detail_copy_does_not_register_public_names_or_hashes(self):
        detail = {'id': HISTORY_ID, 'name': HISTORY_NAME, 'sha256': '0123456789abcdef' * 4,
                  'local': {'status': 'saved', 'path': '/tmp/synthetic/' + HISTORY_NAME}}
        self.clipboard(json.dumps(detail), expected_json=detail)
        self.assertFalse(self.ctx.evidence.redactor.audit_literals)

    def test_unverified_clipboard_json_stays_opaque(self):
        self.clipboard(json.dumps(PREPARATION))
        self.assertIn(CACHE_NOTICE, self.ctx.evidence.redactor.audit_literals)

    def test_different_clipboard_is_registered_before_exact_match_refusal(self):
        for value in (json.dumps({'password': 'synthetic private phrase'}),
                      json.dumps(dict(PREPARATION, localCopiesPreserved=1)),
                      '{"cacheNotice":"synthetic private phrase",' + json.dumps(PREPARATION)[1:],
                      'not JSON'):
            with self.subTest(case=len(value)), self.assertRaisesRegex(RecipeFailed, 'clipboard JSON differs'):
                self.clipboard(value, expected_json=PREPARATION)
            self.assertIn(value, self.ctx.evidence.redactor.audit_literals)

    def test_verified_local_json_still_registers_links_secret_fields_and_credentials(self):
        path = self.out/'frames/before-verified-link.png'
        self.ctx.evidence.frame(Image.new('RGB', (120, 80), 'white'), path)
        observation = {'name': HISTORY_NAME, 'receipt': {'directUrl': LINK},
                       'password': 'synthetic private phrase', 'message': SECRET}
        self.clipboard(json.dumps(observation), expected_json=observation)
        literals = self.ctx.evidence.redactor.audit_literals
        for secret in (LINK, TOKEN, 'synthetic private phrase', SECRET):
            self.assertIn(secret, literals)
        self.assertNotIn(HISTORY_NAME, literals)
        result = self.ctx.evidence.scan()
        self.assertTrue(result['passed'])
        self.assertTrue(path.exists())


if __name__ == '__main__':
    unittest.main()
