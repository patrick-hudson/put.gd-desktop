"""Synthetic evidence confinement tests, never real desktop/provider proof."""
from contextlib import redirect_stderr, redirect_stdout
import dataclasses
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.lab.surface_driver.api import Recipe, RecipeBlocked, RecipeFailed, ReceiptRow
from tools.lab.surface_driver.catalog import Catalog
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.redact import Redactor, Sensitive, scan_run, save_frame
from tools.lab.surface_driver.receipts import Evidence, write_json, write_receipts
from tools.lab.surface_driver.run import run_recipes, main
from tools.lab.surface_driver.target import Target
from tools.lab.surface_driver import redact
sys.path.insert(0, str(ROOT/'tests/desktop'))
from surface_live_fixture import EVIDENCE, FakeDesktop, admitted, control, owned_window
try:
    from PIL import Image, PngImagePlugin
except ImportError:
    Image = PngImagePlugin = None

LINK = 'https://fixture.invalid/urls/synthetic-path-token#synthetic-fragment-token'
AWS_KEY = 'AKIA' + 'SYNTHETICKEY' + '0'*4
SESSION = 'synthetic-session-opaque-value'


class MemoryTarget:
    root = '/tmp/sd-safe'
    socket = root+'/runtime/putgd/session.sock'
    physical = None
    monitor_pid = None

    def cli(self, *args, **kwargs):
        return {'returncode': 0, 'stdout': json.dumps({'ok': True, 'result': {
            'receipt': {'directUrl': LINK}, 'arbitrary': SESSION, 'operationId': 'owned-operation'}}),
                'stderr': 'Authorization: Bearer '+SESSION}

    def execute(self, *args, **kwargs):
        return {'returncode': 0, 'stdout': LINK, 'stderr': SESSION}

    def capture_image(self, head=0):
        return Image.new('RGB', (120, 80), 'white')

    def screenshot(self, *args, **kwargs):
        raise AssertionError('raw disk screenshot must never be called')


class Secrets(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name)
        self.catalog = self.path/'catalog'; self.catalog.mkdir()
        self.out = self.path/'run'
        self.env = dict(desktop='mint-cinnamon-x11', host='boat', placement='boat', guest='cinnamon-x11',
                        package='source', destination='local', build='synthetic-build')
        self.ctx = DriverContext(MemoryTarget(), self.env, {'session': 'x11', 'catalog_tolerance': .025,
            'at_spi': {'available': True}, 'evidence': dict(EVIDENCE)}, self.catalog, self.out)

    def tearDown(self):
        self.temp.cleanup()

    def live(self):
        # These full-layer assertions belong to live destinations. Local
        # synthetic frames now have their own secret-field-only coverage.
        with admitted():
            self.ctx = DriverContext(self.ctx.target, dict(self.env, destination='s3'), self.ctx.profile,
                                     self.catalog, self.out)

    def assert_safe(self, directory):
        for path in Path(directory).rglob('*'):
            if path.is_file():
                for secret in (LINK, AWS_KEY, SESSION, 'synthetic-fragment-token', 'synthetic-path-token'):
                    self.assertNotIn(secret.encode(), path.read_bytes(), path.name)

    def test_sensitive_str_repr_json_and_dataclass(self):
        value = Sensitive({'url': LINK, 'session': SESSION})
        self.assertEqual(value.reveal()['url'], LINK)
        self.assertEqual(str(value), '[redacted]')
        self.assertNotIn(SESSION, repr(value))
        self.assertEqual(json.loads(json.dumps(value)), '[redacted]')
        @dataclasses.dataclass
        class Row:
            value: object
        self.assertNotIn(SESSION, json.dumps(dataclasses.asdict(Row(value))))

    def test_url_derivative_is_equal_only_within_a_run(self):
        redactor = Redactor()
        a, b = redactor.url(LINK), redactor.url(LINK)
        self.assertEqual(a, b)
        self.assertEqual(a['host'], 'fixture.invalid')
        self.assertEqual(a['length'], len(LINK))
        self.assertNotEqual(a['hash'], Redactor().url(LINK)['hash'])
        self.assertNotEqual(a['hash'], redactor.url(LINK+'x')['hash'])
        self.assertEqual(set(a), {'host', 'hash', 'length', 'present'})

    def test_short_secrets_preserve_schema_identity_paths_and_salted_derivatives(self):
        redactor = self.ctx.evidence.redactor
        redactor.remember('e', opaque=True)
        derivative = redactor.url(LINK)
        row = ReceiptRow('editor.undo', self.env['desktop'], 'source', 'passed',
                         note='secret e', recipe='core_demo.text_tool', build='abcdef', receipt='evidence/receipt.json',
                         frames=[{'label': 'before', 'path': 'frames/before.png'}])
        write_receipts(self.out, {**self.env, 'fixture_url': derivative}, [row], 'safe', self.out/'coverage.json', evidence=self.ctx.evidence)
        receipt = json.loads((self.out/'receipt.json').read_text())
        actual = receipt['interactions'][0]
        self.assertEqual(actual['interaction'], row.interaction)
        self.assertEqual(actual['desktop'], row.desktop)
        self.assertEqual(actual['recipe'], row.recipe)
        self.assertEqual(actual['build'], row.build)
        self.assertEqual(actual['receipt'], row.receipt)
        self.assertEqual(actual['frames'][0]['path'], 'frames/before.png')
        self.assertEqual(receipt['environment']['fixture_url'], derivative)
        self.assertEqual(actual['note'], 'secret e')
        self.assertIn(derivative['hash'], redactor.text(LINK))
        row.note = LINK
        write_receipts(self.out, self.env, [row], 'safe', self.out/'coverage.json', evidence=self.ctx.evidence)
        actual = json.loads((self.out/'receipt.json').read_text())['interactions'][0]
        self.assertIsInstance(actual['note'], str)
        self.assertNotIn(LINK, actual['note'])
        self.assertIn(derivative['hash'], actual['note'])
        self.assertIn(derivative['host'], actual['note'])
        self.assertEqual(redactor.text(actual['note']), actual['note'])
        masked = redactor.text('secret e')
        self.assertEqual(redactor.text(masked), masked)
        opaque = Redactor()
        forged = '[url '+json.dumps({'hash': '0'*64, 'host': SESSION, 'length': 94, 'present': True}, sort_keys=True)+']'
        opaque.remember(forged, opaque=True)
        self.assertNotIn(SESSION, opaque.text('failure: '+forged))

    def test_escaped_errors_and_symlink_directories_fail_closed(self):
        redactor = Redactor()
        value = SESSION+'\nsecond line'
        redactor.remember(value, opaque=True)
        self.assertNotIn(SESSION, redactor.text(repr(value)))
        self.assertNotIn('synthetic-fragment-token', redactor.text(LINK.replace('/', '\\/')))
        with self.assertRaises(RecipeBlocked) as error:
            self.ctx.type_text('synthetic-secret-☃-value')
        self.assertNotIn('☃', self.ctx.evidence.redactor.exception(error.exception))
        self.out.mkdir()
        (self.out/'outside').symlink_to(self.catalog, target_is_directory=True)
        self.assertFalse(scan_run(self.out)['passed'])

    def test_sanitized_session_derivatives_pass_but_proton_password_fails(self):
        self.out.mkdir()
        write_json(self.out/'safe.json', {'accessToken': Sensitive(SESSION), 'sessionId': None})
        self.assertTrue(scan_run(self.out)['passed'])
        (self.out/'leak.log').write_text('userKeyPassword: '+SESSION)
        self.assertFalse(scan_run(self.out)['passed'])

    def test_explicit_distinctive_secret_is_audited_only_in_memory(self):
        self.ctx.sensitive(SESSION)
        self.ctx.sensitive('e')
        self.assertEqual(self.ctx.evidence.redactor.audit_literals, {SESSION})
        self.out.mkdir()
        (self.out/'planted.log').write_text(SESSION)
        result = self.ctx.evidence.scan()
        self.assertFalse(result['passed'])
        self.assertIn('registered-secret', {v['kind'] for v in result['findings']})
        self.assertNotIn(SESSION, json.dumps(result))

    def test_mcp_link_and_explicit_url_paths_activate_frame_guard(self):
        self.ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': json.dumps({'link': LINK}), 'stderr': ''}
        self.ctx.mcp('synthetic_session')
        self.ctx.url_evidence('synthetic-opaque-url-input')

    def test_log_call_shell_clipboard_mcp_notes_receipts_and_exceptions(self):
        ctx = self.ctx
        self.assertEqual(ctx.call('history.detail')['arbitrary'], SESSION)
        self.assertEqual(ctx.clipboard_text(), LINK)
        ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': json.dumps({'result': {'link': LINK, 'sessionToken': SESSION}}), 'stderr': SESSION}
        self.assertEqual(ctx.mcp('get_history_entry')['result']['link'], LINK)
        nested = 'synthetic-MCP-text-only-secret'
        ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': json.dumps({
            'content': [{'type': 'text', 'text': json.dumps({'sessionToken': nested})}]}), 'stderr': ''}
        reply = ctx.mcp('legacy_text_result')
        ctx.note(json.loads(reply['content'][0]['text'])['sessionToken'])
        self.assertNotIn(nested, '\n'.join(ctx.notes))
        self.assertNotIn(nested, ctx.evidence.redactor.exception(RecipeFailed(nested)))
        ctx.note('seen '+LINK+' '+SESSION+' '+AWS_KEY)
        ctx.review('fragment synthetic-fragment-token '+SESSION)
        redactor = ctx.evidence.redactor
        message = redactor.exception(RecipeFailed('bad '+LINK+' '+SESSION))
        self.assertNotIn(SESSION, message)
        self.assertNotIn('http', message)
        self.assertEqual(redactor.exception(RuntimeError(SESSION)), 'driver error: RuntimeError')
        write_receipts(self.out, {**self.env, 'clipboard': Sensitive(LINK)}, [ReceiptRow(
            'editor.undo', self.env['desktop'], 'source', 'failed', note='; '.join(ctx.notes)+message,
            review_reason='; '.join(ctx.reviews))], 'safe', self.out/'coverage.json', evidence=ctx.evidence)
        self.assert_safe(self.out)
        self.assertTrue(scan_run(self.out)['passed'])
        events = [json.loads(line) for line in (self.out/'commands.jsonl').read_text().splitlines()]
        self.assertTrue(all(set(event['stdout']) in ({'redacted', 'length', 'present'}, {'host', 'hash', 'length', 'present'}) for event in events))
        self.assertIn('operationId=owned-operation', ctx.notes)

    def test_notifications_have_no_raw_file_sink_and_body_errors_are_redacted(self):
        scripts = []
        def execute(script, **kwargs):
            scripts.append(script)
            # Only the gdbus owner lookup; the monitor script embeds probe.py, which mentions GetNameOwner too.
            owner = script.startswith('gdbus call') and 'GetNameOwner' in script
            return {'returncode': 0, 'stdout': "(':1.42',)" if owner else '123', 'stderr': ''}
        self.ctx.target.execute = execute
        self.ctx.start_notifications()
        self.assertNotIn('notifications.log', '\n'.join(scripts))
        self.assertIn('GetNameOwner', '\n'.join(scripts))
        self.assertIn(':1.42', '\n'.join(scripts))
        self.ctx._notifications_since = 0
        self.assertIn('notification_read', '\n'.join(scripts))
        body = 'Authorization: Bearer '+SESSION
        raw = 'method call time=100.1 sender=:1.2 serial=45 interface=org.freedesktop.Notifications; member=Notify\n'+''.join(' string "'+s+'"\n' for s in ('put.gd', 'putgd', 'Synthetic', body))
        reply = json.dumps({'ok': True, 'result': {'alive': True, 'text': raw}})
        self.ctx.target.execute = lambda *a, **kw: {'returncode': 0, 'stdout': reply, 'stderr': ''}
        self.assertEqual(self.ctx.notifications()[0]['body'], body)
        self.ctx.note('notification body '+SESSION)
        self.assertNotIn(SESSION, '\n'.join(self.ctx.notes))
        self.assert_safe(self.out)

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_runner_suppresses_recipe_stdout_stderr_and_failed_blocked_messages(self):
        def failed(ctx):
            ctx.call('history.detail')
            print(LINK)
            print(SESSION, file=sys.stderr)
            raise RecipeFailed('mismatch '+SESSION+' '+LINK)
        def blocked(ctx):
            raise RecipeBlocked('unsafe '+LINK+' '+SESSION, 669)
        def exit_recipe(ctx):
            raise SystemExit(SESSION)
        console = io.StringIO()
        with redirect_stdout(console), redirect_stderr(console):
            rows = run_recipes(self.ctx, [Recipe(('editor.undo',), failed), Recipe(('editor.redo',), blocked),
                Recipe(('editor.tool-text',), exit_recipe), Recipe(('editor.tool-redact',), lambda c: None)],
                self.env, self.out, 'safe', self.out/'coverage.json')
        self.assertEqual([row.functional for row in rows], ['failed', 'blocked', 'failed'])
        self.assertTrue(self.env['interrupted'])
        self.assertNotIn(SESSION, console.getvalue())
        self.assertNotIn(LINK, console.getvalue())
        self.assert_safe(self.out)
        self.assertTrue(json.loads((self.out/'receipt.json').read_text())['secret_scan']['passed'])


    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_scan_catches_planted_json_logs_png_metadata_and_changed_frames(self):
        self.out.mkdir()
        for filename, content, kind in [('leak.json', json.dumps({'url': LINK}).replace('/', '\\/'), 'url'),
            ('leak.log', AWS_KEY, 'aws-key'), ('session.log', 'X-PM-UID: '+SESSION, 'session-or-authorization')]:
            with self.subTest(filename=filename):
                path = self.out/filename; path.write_text(content)
                result = scan_run(self.out)
                self.assertFalse(result['passed'])
                self.assertIn(kind, {finding['kind'] for finding in result['findings']})
                self.assertNotIn(SESSION, json.dumps(result))
                path.unlink()
        meta = PngImagePlugin.PngInfo(); meta.add_text('comment', LINK)
        path = self.out/'planted.png'
        Image.new('RGB', (10, 10), 'white').save(path, pnginfo=meta)
        self.assertFalse(scan_run(self.out)['passed'])
        path.unlink()
        self.ctx.evidence.frame(Image.new('RGB', (10, 10), 'white'), path)
        self.assertTrue(scan_run(self.out, frames=self.ctx.evidence.frames)['passed'])
        Image.new('RGB', (10, 10), 'black').save(path)
        self.assertFalse(scan_run(self.out, frames=self.ctx.evidence.frames)['passed'])

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_planted_leak_fails_run_receipt_coverage_and_cli(self):
        def plant(ctx):
            ctx.note('original synthetic failure context')
            (ctx.out/'planted.log').write_text(LINK)
        rows = run_recipes(self.ctx, [Recipe(('editor.undo',), plant)], self.env, self.out, 'safe', self.out/'coverage.json')
        self.assertEqual(rows[0].functional, 'failed')
        self.assertIn('original synthetic failure context', rows[0].note)
        self.assertIn('secret evidence audit failed', rows[0].note)
        receipt = json.loads((self.out/'receipt.json').read_text())
        self.assertFalse(receipt['secret_scan']['passed'])
        self.assertEqual(json.loads((self.out/'coverage.json').read_text())['entries'][0]['functional'], 'failed')
        # The independent CLI audit must invalidate coverage that was previously
        # imported, even when another recipe in that campaign remains passing.
        coverage = json.loads((self.out/'coverage.json').read_text())
        coverage['entries'][0]['functional'] = 'passed'
        other = dict(coverage['entries'][0], interaction='editor.redo')
        coverage['entries'].append(other)
        external = self.path/'campaign.json'
        external.write_text(json.dumps(coverage))
        result = subprocess.run([sys.executable, '-m', 'tools.lab.surface_driver.redact', str(self.out), '--coverage', str(external)], cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(LINK, result.stdout+result.stderr)
        self.assertEqual([r['functional'] for r in json.loads(external.read_text())['entries']], ['failed', 'passed'])

    @unittest.skipUnless(Image, 'requires python3-pil')
    def test_transport_capture_uses_only_in_memory_rfb(self):
        target = Target.__new__(Target)
        target.heartbeat = lambda **kw: None
        target.physical = None; target.info = {'vnc_display': 20}
        client = unittest.mock.Mock()
        client.frame.return_value = (2, 1, b'\xff\xff\xff\0'*2)
        with patch('tools.lab.surface_driver.target.rfb.RFB', return_value=client), patch('tools.lab.lab.screenshot', side_effect=AssertionError('raw disk witness')):
            image = target.capture_image()
            self.assertEqual(image.size, (2, 1))
            target.screenshot(self.path/'legacy.png')
        client.close.assert_called()
        with Image.open(self.path/'legacy.png') as image:
            self.assertIsNotNone(image.getbbox())


if __name__ == '__main__':
    unittest.main()
