"""Official SDK / production CLI contract tests; synthetic local IPC only.

Run with PUTGD_CLI pointing at the built production binary. No desktop/cloud proof.
"""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
import zipfile
import zlib

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from share_mcp.server import CliAdapter, create_server, safe_commands


def synthetic_png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return len(data).to_bytes(4, 'big') + kind + data + zlib.crc32(kind + data).to_bytes(4, 'big')
    header = (2).to_bytes(4, 'big') + (1).to_bytes(4, 'big') + bytes([8, 2, 0, 0, 0])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(b'\x00' + bytes(6))) + chunk(b'IEND', b''))


class AdapterTests(unittest.IsolatedAsyncioTestCase):
    async def test_invalid_input_never_spawns_cli(self):
        # What an agent must never get through: an action outside the list, a secret
        # written through a configuration patch, a delete without its acknowledgement,
        # an update from an image of its choosing.
        adapter = CliAdapter('/does/not/exist')
        server = create_server(adapter)
        for name, args in [
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email']}, {'op': 'undo'}]}),
            # #1618: terms and patterns need the list category, stay inside their limits, and never carry a line break.
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email'], 'terms': ['Project Aurora']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': []}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['a\nb']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['x'] * 201}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['x' * 101]}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': ['x'] * 51}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': ['x' * 501]}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': [3]}]}),
            # The editor's rules, counted in code points: one line (even U+2028 and U+2029), a letter or digit in a word.
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['a\u2028b']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['...']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['\U0001F600' * 100 + 'x']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': ['a\u2029b']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': ['a\rb']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'patterns': ['x' * 500 + '\n']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['username']}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email', 'phone', 'ip', 'card', 'key', 'name', 'list', 'email']}]}),
            ('perform_action', {'action': 'shell'}),
            ('delete_history_uploads', {'ids': [], 'acknowledgement': ''}),
            ('patch_configuration', {'patch': {'accessKey': 'synthetic'}, 'expected_revision': 'r'}),
            ('patch_configuration', {'patch': {'captureDefaults': {'accessKey': 'synthetic'}}, 'expected_revision': 'r'}),
            ('install_update', {'confirm': True, 'image': '/tmp/x.AppImage'}),
            # #1678: the Cloudflare R2 form takes only its own fields, each action only what it needs.
            ('drive_r2_destination', {'action': 'set', 'field': 'endpoint', 'value': 'https://example.com'}),
            ('drive_r2_destination', {'action': 'set', 'field': 'bucket'}),
            ('drive_r2_destination', {'action': 'paste'}),
            ('drive_r2_destination', {'action': 'inspect', 'text': 'https://example.com'}),
            ('drive_r2_destination', {'action': 'inspect', 'text': ''}),
            ('drive_r2_destination', {'action': 'inspect', 'text': '\x00'}),
            ('drive_r2_destination', {'action': 'move', 'profile_id': '\x00', 'label': '\x00'}),
            ('drive_r2_destination', {'action': 'move', 'label': 'Lab'}),
            ('drive_r2_destination', {'action': 'add'}),
            ('drive_r2_destination', {'action': 'add', 'label': 'Lab', 'profile_id': 'lab'}),
            ('drive_r2_destination', {'action': 'set', 'field': 'advanced', 'value': 'half'}),
            ('drive_r2_destination', {'action': 'set', 'field': 'bucket', 'value': 'x' * 2049}),
            # A field an action does not take is omitted, never sent as null.
            ('drive_r2_destination', {'action': 'inspect', 'field': None}),
            ('drive_r2_destination', {'action': 'move', 'text': None}),
            ('drive_r2_destination', {'action': 'set', 'field': 'bucket', 'value': None}),
            # #1753: an error log step the resident does not take, an empty clear, a filter outside setFilter.
            ('act_on_error', {'id': 'synthetic-error', 'action': 'open_destination'}),
            ('clear_errors', {'ids': []}),
            ('drive_error_log', {'action': 'inspect', 'kind': 'upload'}),
            ('list_errors', {'kind': 'everything'}),
        ]:
            result = await server.call_tool(name, args)
            envelope = result.structured_content or json.loads(result.content[0].text)
            self.assertEqual(envelope['error']['code'], 'invalid_request', name)

    def test_redact_terms_and_patterns_follow_the_shared_table(self):
        # #1618: the editor's validation table (tests/fixtures/text/redact-validation.json) feeds the same inputs
        # to this helper. Only the rules it can check without PCRE2 are held to it: valid rows are accepted, and
        # line breaks, empty and too long values and words without a letter or digit are refused. Every other rule
        # (compiling, empty matches and whatever else the editor adds) is skipped by name, so the table can gain or
        # lose rules; the editor's edit_command_invalid stays authoritative.
        table = json.loads((Path(__file__).resolve().parents[1] / 'fixtures/text/redact-validation.json').read_text(encoding='utf-8'))
        refused = {'lineBreak', 'empty', 'length', 'letter'}
        checked = 0
        for key in ('terms', 'patterns'):
            for row in table[key]:
                accepted = safe_commands([{'op': 'redactText', 'categories': ['list'], key: [row['value']]}])
                if row['rule'] == 'ok':
                    self.assertTrue(accepted, (key, row['value'][:40]))
                elif row['rule'] in refused:
                    self.assertFalse(accepted, (key, row['rule'], row['value'][:40]))
                else:
                    continue
                checked += 1
        self.assertGreater(checked, 30)
        # A pattern is not trimmed when it runs, so its 500 code points are counted as written.
        self.assertTrue(safe_commands([{'op': 'redactText', 'categories': ['list'], 'patterns': ['x' * 500]}]))
        self.assertFalse(safe_commands([{'op': 'redactText', 'categories': ['list'], 'patterns': [' ' * 500 + 'x']}]))

    async def test_review_controls_forward_their_fields(self):
        # #1613: each new review control reaches recording.review with the fields the window
        # reads, and a missing or out-of-range field never reaches the resident.
        from unittest.mock import AsyncMock
        adapter = CliAdapter('/does/not/exist')
        adapter.call = AsyncMock(return_value={'ok': True})
        server = create_server(adapter)
        for args, params in [
            ({'action': 'selectRange', 'start_us': 1, 'end_us': 5}, {'action': 'selectRange', 'start': 1, 'end': 5}),
            ({'action': 'adjustCut', 'cut_id': 'c1', 'start_us': 0, 'end_us': 9}, {'action': 'adjustCut', 'cutId': 'c1', 'start': 0, 'end': 9}),
            ({'action': 'selectEdit', 'kind': 'cover', 'edit_id': 'm1'}, {'action': 'selectEdit', 'kind': 'cover', 'editId': 'm1'}),
            ({'action': 'selectEdit', 'kind': 'trim'}, {'action': 'selectEdit', 'kind': 'trim'}),
            ({'action': 'setTrim', 'start_us': 3}, {'action': 'setTrim', 'start': 3, 'end': None}),
            ({'action': 'setCoverEdgeToPlayhead', 'edge': 'end'}, {'action': 'setCoverEdgeToPlayhead', 'edge': 'end'}),
            ({'action': 'viewerPan', 'dx': 4.5, 'dy': -2}, {'action': 'viewerPan', 'dx': 4.5, 'dy': -2}),
            ({'action': 'timelineZoom', 'level': 4}, {'action': 'timelineZoom', 'level': 4}),
            ({'action': 'frameStep', 'delta': -1}, {'action': 'frameStep', 'delta': -1}),
            ({'action': 'skim', 'time_us': 7}, {'action': 'skim', 'time': 7}),
            ({'action': 'panel', 'panel_open': True}, {'action': 'panel', 'open': True}),
            ({'action': 'resize', 'width': 1000, 'height': 680}, {'action': 'resize', 'width': 1000, 'height': 680}),
            ({'action': 'undo'}, {'action': 'undo'}),
        ]:
            adapter.call.reset_mock()
            result = await server.call_tool('drive_recording_review', {'id': 'r', **args})
            adapter.call.assert_awaited_once_with('recording.review', {'id': 'r', **params})
        for args in [{'action': 'selectRange', 'start_us': 1}, {'action': 'selectEdit', 'kind': 'cover'}, {'action': 'timelineZoom', 'level': 3},
                     {'action': 'frameStep', 'delta': 2}, {'action': 'viewerZoom', 'mode': 'big'}, {'action': 'panel'}, {'action': 'resize', 'width': 10, 'height': 900},
                     {'action': 'adjustCut', 'cut_id': 'c1', 'start_us': 0}, {'action': 'cut', 'edge': 'middle'}]:
            adapter.call.reset_mock()
            result = await server.call_tool('drive_recording_review', {'id': 'r', **args})
            envelope = result.structured_content or json.loads(result.content[0].text)
            self.assertEqual(envelope['error']['code'], 'invalid_request', args)
            adapter.call.assert_not_awaited()

    async def test_schema_rejects_coercion_before_calling_adapter(self):
        from unittest.mock import AsyncMock
        adapter = CliAdapter('/does/not/exist')
        adapter.call = AsyncMock()
        server = create_server(adapter)
        for name, args in [
            ('perform_action', {'action': 'clipboard', 'extra': True}),
            ('delete_history_uploads', {'ids': '["synthetic-entry"]'}),
        ]:
            with self.subTest(name=name, args=args):
                result = await server.call_tool(name, args)
                self.assertFalse(result.is_error)
                self.assertEqual(result.structured_content['error']['code'], 'invalid_request')
                adapter.call.assert_not_awaited()

    async def test_parameters_use_stdin_without_process_argument_exposure(self):
        from unittest.mock import AsyncMock, patch
        process = AsyncMock()
        process.communicate.return_value = (b'{}', b'')
        with patch('asyncio.create_subprocess_exec', return_value=process) as start:
            await CliAdapter().call('share.inspect', {'link': 'synthetic-secret-opaque'})
        self.assertIn('--params-stdin', start.call_args.args)
        self.assertNotIn('synthetic-secret-opaque', repr(start.call_args.args))
        self.assertIn(b'synthetic-secret-opaque', process.communicate.call_args.args[0])


class ProductionCliTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.cli = os.environ.get('PUTGD_CLI')
        if not self.cli:
            self.fail('PUTGD_CLI must select the built production putgd-cli')
        self.directory = tempfile.TemporaryDirectory(prefix='putgd mcp socket ')
        self.socket = str(Path(self.directory.name) / 'session socket')
        self.assertLessEqual(len(self.socket.encode('utf-8')), 107, 'Test socket exceeds Linux path limit')
        self.calls = []
        self.listener = await asyncio.start_unix_server(self.handle, self.socket)

    async def asyncTearDown(self):
        if hasattr(self, 'listener'):
            self.listener.close()
            await self.listener.wait_closed()
            self.directory.cleanup()

    async def handle(self, reader, writer):
        request = json.loads(await reader.readline())
        self.calls.append(request)
        method = request['method']
        result = {'operationId': 'synthetic-operation', 'method': method, 'params': request['params']}
        if method == 'delete.prepare':
            result = {'acknowledgement': 'synthetic-effect', 'targets': request['params']['ids']}
        reply = {'schemaVersion': 1, 'requestId': request['requestId'], 'ok': True, 'result': result,
                 'error': {'code': '', 'message': '', 'details': {}}}
        writer.write(json.dumps(reply).encode() + b'\n')
        await writer.drain()
        writer.close()
        await writer.wait_closed()

    async def test_official_sdk_discovery_and_cli_parity(self):
        parameters = StdioServerParameters(command=sys.executable, args=[
            '-m', 'share_mcp.server', '--cli', self.cli, '--socket', self.socket])
        async with stdio_client(parameters) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                discovered = (await session.list_tools()).tools
                self.assertTrue(discovered)
                # Every tool refuses what its schema does not name.
                for tool in discovered:
                    self.assertEqual(tool.input_schema['type'], 'object')
                    self.assertFalse(tool.input_schema['additionalProperties'], tool.name)
                async def call(name, args):
                    reply = await session.call_tool(name, args)
                    self.assertFalse(reply.is_error, reply)
                    return reply.structured_content
                for name, args in [
                    ('perform_action', {'action': 'clipboard', 'shell': 'extra'}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': False}),
                ]:
                    rejected = await call(name, args)
                    self.assertEqual(rejected['error']['code'], 'invalid_request', rejected)
                    self.assertEqual(self.calls, [], 'Invalid wire arguments must issue no IPC')
                # A delete is prepared first and confirmed only with the acknowledgement it returned.
                for name, args, method, params in [
                    ('delete_history_uploads', {'ids': ['synthetic-entry']}, 'delete.prepare', {'ids': ['synthetic-entry']}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'},
                     'delete.confirm', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'}),
                    ('install_update', {'confirm': True}, 'update.install', {'confirm': True}),
                    # Text recognition and QR codes (#1428): each tool forwards its one method.
                    ('perform_action', {'action': 'text', 'delay_seconds': 3}, 'action.invoke', {'action': 'text', 'delaySeconds': 3}),
                    ('recognize_text', {'source': 'file', 'path': '/tmp/synthetic.png', 'languages': ['eng', 'deu'], 'copy': True},
                     'text.recognize', {'source': 'file', 'path': '/tmp/synthetic.png', 'languages': ['eng', 'deu'], 'copy': True}),
                    ('get_text_recognition', {'id': 'synthetic-operation'}, 'text.inspect', {'id': 'synthetic-operation'}),
                    ('cancel_text_recognition', {'id': 'synthetic-operation'}, 'text.cancel', {'id': 'synthetic-operation'}),
                    ('recognize_editor_text', {'id': 'synthetic-operation'}, 'editor.recognize', {'id': 'synthetic-operation'}),
                    ('select_editor_text', {'id': 'synthetic-operation', 'result': 3, 'from_word': 0, 'to_word': 12},
                     'editor.selectText', {'id': 'synthetic-operation', 'result': 3, 'from': 0, 'to': 12}),
                    ('copy_editor_text', {'id': 'synthetic-operation', 'result': 3, 'all': True},
                     'editor.copyText', {'id': 'synthetic-operation', 'result': 3, 'all': True}),
                    ('copy_editor_code', {'id': 'synthetic-operation', 'result': 3, 'index': 0},
                     'editor.copyCode', {'id': 'synthetic-operation', 'result': 3, 'index': 0}),
                    ('open_editor_code', {'id': 'synthetic-operation', 'result': 3, 'index': 0},
                     'editor.openCode', {'id': 'synthetic-operation', 'result': 3, 'index': 0}),
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email', 'key']}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email', 'key']}]}),
                    # #1618: a term of exactly 100 code points, 99 of them outside the BMP, is within the limit (the editor counts code points).
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['\U0001F600' * 99 + 'x']}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['list'], 'terms': ['\U0001F600' * 99 + 'x']}]}),
                    # #1618: Remove suggestions, one undo step, through the editor view.
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'removeSuggestions'},
                     'editor.view', {'id': 'synthetic-operation', 'action': 'removeSuggestions'}),
                    # #1618: the name and list categories, with this call's own terms and patterns.
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['name', 'list'], 'terms': ['Project Aurora'], 'patterns': ['\\bJIRA-\\d+\\b']}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['name', 'list'], 'terms': ['Project Aurora'], 'patterns': ['\\bJIRA-\\d+\\b']}]}),
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email', 'phone', 'ip', 'card', 'key', 'name', 'list']}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [{'op': 'redactText', 'categories': ['email', 'phone', 'ip', 'card', 'key', 'name', 'list']}]}),
                    ('copy_history_text', {'id': 'synthetic-entry', 'keep_result': True}, 'history.copyText', {'id': 'synthetic-entry', 'keepResult': True}),
                    ('history_qr_code', {'id': 'synthetic-entry', 'mode': 'save', 'path': '/tmp/synthetic-qr.png', 'overwrite': True},
                     'history.qr', {'id': 'synthetic-entry', 'mode': 'save', 'path': '/tmp/synthetic-qr.png', 'overwrite': True}),
                    ('drive_r2_destination', {'action': 'set', 'field': 'jurisdiction', 'value': 'eu', 'profile_id': 'synthetic-r2'},
                     'ui.r2Destination', {'action': 'set', 'field': 'jurisdiction', 'value': 'eu', 'profileId': 'synthetic-r2'}),
                    ('list_errors', {'kind': 'deletion', 'resolved': False, 'history_id': 'synthetic-entry'},
                     'errors.list', {'kind': 'deletion', 'resolved': False, 'historyId': 'synthetic-entry'}),
                    ('drive_error_log', {'action': 'step', 'step': 'show_in_history'}, 'ui.errorLog', {'action': 'step', 'step': 'show_in_history'}),
                    # #1759: Remove from History only from an error log entry, and its confirmation.
                    ('act_on_error', {'id': 'synthetic-error', 'action': 'remove_from_history'}, 'errors.act', {'id': 'synthetic-error', 'action': 'remove_from_history'}),
                    ('drive_error_log', {'action': 'confirmRemove'}, 'ui.errorLog', {'action': 'confirmRemove'}),
                ]:
                    await call(name, args)
                    self.assertEqual(self.calls[-1]['method'], method)
                    self.assertEqual(self.calls[-1]['params'], params)
                # Every request went to this session and no other.
                self.assertTrue(all(r['schemaVersion'] == 1 and r['session'] == self.socket for r in self.calls))
        self.listener.close()
        await self.listener.wait_closed()
        # Python 3.13+ removes the Unix socket when the asyncio server closes.
        Path(self.socket).unlink(missing_ok=True)
        unavailable = await CliAdapter(self.cli, self.socket).call('capabilities', {})
        self.assertEqual(unavailable['error']['code'], 'session_unavailable')


class CustomUploaderResidentTests(unittest.IsolatedAsyncioTestCase):
    """#1361 through the MCP helper against a real private resident and the scripted loopback
    receiver (tests/fixtures/http_uploader_server.py): import a .sxcu, test, upload, retry a
    lost reply, delete, and check its token is in nothing the resident wrote. Set PUTGD_APP
    and PUTGD_CLI to the built production binaries. Synthetic token; nothing leaves loopback."""
    TOKEN = 'putgd-synthetic-uploader-token-not-real'

    async def asyncSetUp(self):
        self.cli, self.app = os.environ.get('PUTGD_CLI'), os.environ.get('PUTGD_APP')
        if not self.cli or not self.app:
            self.fail('PUTGD_CLI and PUTGD_APP must select the built production putgd-cli and putgd')
        repository = Path(__file__).resolve().parents[2]
        self.directory = tempfile.TemporaryDirectory(prefix='putgd-mcp-')
        # Registered before anything starts: a cleanup also runs when the rest of this setup fails.
        self.addAsyncCleanup(self.stop_processes)
        self.root = Path(self.directory.name)
        runtime = self.root / 'rt'
        runtime.mkdir(mode=0o700)
        self.socket = str(runtime / 'putgd/session.sock')
        self.assertLessEqual(len(self.socket.encode('utf-8')), 107, 'Test socket exceeds Linux path limit')
        environment = os.environ | {'QT_QPA_PLATFORM': 'offscreen', 'XDG_RUNTIME_DIR': str(runtime), 'XDG_CONFIG_HOME': str(self.root / 'config'),
                                    'XDG_DATA_HOME': str(self.root / 'data'), 'XDG_STATE_HOME': str(self.root / 'state'),
                                    'XDG_CACHE_HOME': str(self.root / 'cache'), 'XDG_SESSION_TYPE': 'unspecified', 'XDG_CURRENT_DESKTOP': 'putgd-mcp-fixture'}
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER'):
            environment.pop(key, None)
        self.receiver = subprocess.Popen([sys.executable, str(repository / 'tests/fixtures/http_uploader_server.py')], stdout=subprocess.PIPE, text=True)
        self.base = 'http://127.0.0.1:' + self.receiver.stdout.readline().strip()
        self.log = (self.root / 'resident.log').open('w')
        self.resident = subprocess.Popen([self.app, '--background'], env=environment, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 15
        while not Path(self.socket).exists() and self.resident.poll() is None and time.monotonic() < deadline:
            await asyncio.sleep(.05)
        self.assertTrue(Path(self.socket).exists(), 'Private resident did not start')

    async def stop_processes(self):
        for process in (getattr(self, 'resident', None), getattr(self, 'receiver', None)):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if getattr(self, 'receiver', None) is not None:
            self.receiver.stdout.close()
        if hasattr(self, 'log'):
            self.log.close()
        if hasattr(self, 'directory'):
            self.directory.cleanup()

    def receiver_call(self, path, body=None):
        request = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                         headers={'Content-Type': 'application/json'}, method='GET' if body is None else 'POST')
        with urllib.request.urlopen(request, timeout=10) as reply:
            return json.loads(reply.read())

    def stored(self):
        return [entry for entry in self.receiver_call('/__state')['files'].values() if not entry['deleted']]

    def fetch(self, link):
        try:
            with urllib.request.urlopen(link, timeout=10) as reply:
                return reply.status, reply.read()
        except urllib.error.HTTPError as error:
            return error.code, b''

    def files_with_token(self):
        found = []
        for path in self.root.rglob('*'):
            if path.is_file() and path.suffix != '.sxcu' and self.TOKEN.encode() in path.read_bytes():
                found.append(str(path.relative_to(self.root)))
        return found

    async def test_import_test_upload_retry_delete_and_no_token_on_disk(self):
        sxcu = self.root / 'scripted receiver.sxcu'
        sxcu.write_text(json.dumps({
            'Version': '17.0.0', 'Name': 'Scripted receiver', 'DestinationType': 'ImageUploader, TextUploader, FileUploader', 'RequestMethod': 'POST',
            'RequestURL': self.base + '/upload/json', 'Headers': {'Authorization': 'Bearer ' + self.TOKEN}, 'Body': 'MultipartFormData',
            'FileFormName': 'file', 'URL': '{json:files[0].url}', 'DeletionURL': '{json:files[0].delete}', 'ErrorMessage': '{json:error}'}))
        sxcu.chmod(0o600)
        shortener = self.root / 'shortener.sxcu'
        shortener.write_text(json.dumps({'Version': '17.0.0', 'DestinationType': 'URLShortener', 'RequestURL': self.base + '/shorten'}))
        payload = self.root / 'synthetic.png'
        payload.write_bytes(synthetic_png())
        parameters = StdioServerParameters(command=sys.executable, args=['-m', 'share_mcp.server', '--cli', self.cli, '--socket', self.socket])
        async with stdio_client(parameters) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                async def call(name, args):
                    reply = await session.call_tool(name, args)
                    self.assertFalse(reply.is_error, reply)
                    self.assertNotIn(self.TOKEN, json.dumps(reply.structured_content), name + ' returned the token')
                    return reply.structured_content

                async def until(name, args, done, seconds=30):
                    deadline = time.monotonic() + seconds
                    while True:
                        reply = await call(name, args)
                        if (reply['ok'] and done(reply['result'])) or time.monotonic() > deadline:
                            return reply
                        await asyncio.sleep(.1)

                async def revision():
                    return (await call('inspect', {}))['result']['configuration']['result']['metadata']['observedRevision']

                refused = await call('import_custom_uploader', {'path': str(shortener), 'expected_revision': await revision(), 'credential_mode': 'session'})
                self.assertEqual(refused['error']['code'], 'sxcu_unsupported_type', refused)
                stale = await call('import_custom_uploader', {'path': str(sxcu), 'expected_revision': 'stale', 'credential_mode': 'session'})
                self.assertEqual(stale['error']['code'], 'config_conflict', stale)
                imported = await call('import_custom_uploader', {'path': str(sxcu), 'expected_revision': await revision(), 'profile_id': 'scripted',
                                                                 'credential_mode': 'session'})
                self.assertTrue(imported['ok'], imported)
                self.assertEqual(imported['result']['profileId'], 'scripted')
                self.assertEqual(imported['result']['label'], 'Scripted receiver')
                self.assertEqual(imported['result']['storedValues'], ['header Authorization'])
                self.assertEqual(imported['result']['credentials'], {'mode': 'session'})
                # The configuration holds the header's name and a reference, never its value.
                configuration = (await call('inspect', {}))['result']['configuration']['result']
                profile = next(entry for entry in configuration['document']['profiles'] if entry['id'] == 'scripted')
                self.assertEqual(profile['provider'], 'custom-http')
                self.assertEqual(profile['settings']['headers'], [{'name': 'Authorization', 'secret': True}])
                self.assertEqual(profile['settings']['responseUrl'], '{json:files[0].url}')
                self.assertEqual(profile['credentials'], {'mode': 'session', 'reference': None})
                # What the resident saved is what schemas/config.schema.json describes.
                from jsonschema import Draft202012Validator
                Draft202012Validator(json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text(encoding='utf-8'))).validate(configuration['document'])
                current = configuration['metadata']['observedRevision']
                hint = await call('get_destination_credentials', {'profile_id': 'scripted', 'expected_revision': current})
                self.assertEqual(hint['result']['storedValues'], ['headers/Authorization'])
                self.assertEqual(hint['result']['missingValues'], [])
                # A second import cannot take the same ID.
                again = await call('import_custom_uploader', {'path': str(sxcu), 'expected_revision': current, 'profile_id': 'scripted', 'credential_mode': 'session'})
                self.assertEqual(again['error']['code'], 'profile_exists', again)

                started = await call('test_destination', {'profile_id': 'scripted', 'expected_revision': current})
                self.assertTrue(started['ok'], started)
                job = await until('get_setup_job', {'id': started['result']['setupId']}, lambda result: result['state'] != 'running')
                self.assertEqual(job['result']['state'], 'completed', job)
                self.assertEqual(job['result']['result']['uploadStatus'], 'uploaded')
                self.assertTrue(job['result']['result']['publicReachable'])
                self.assertEqual(job['result']['result']['cleanupStatus'], 'removed')
                self.assertEqual(self.stored(), [])

                async def upload():
                    # A resident reads its saved credentials about three seconds after it
                    # starts and answers `initializing` to uploads until it has.
                    deadline = time.monotonic() + 20
                    while True:
                        accepted = await call('upload_files', {'paths': [str(payload)], 'profile_id': 'scripted'})
                        if accepted['ok'] or accepted['error']['code'] != 'initializing' or time.monotonic() > deadline:
                            break
                        await asyncio.sleep(.25)
                    self.assertTrue(accepted['ok'], accepted)
                    operation = accepted['result']['operationId']
                    finished = await until('get_operation', {'id': operation}, lambda result: result['state'] in ('completed', 'partial', 'failed', 'cancelled'))
                    records = (await call('list_history', {'limit': 20}))['result']['records']
                    return operation, finished['result'], next(record for record in records if record['operationId'] == operation)

                operation, finished, record = await upload()
                self.assertEqual(finished['state'], 'completed', finished)
                self.assertEqual(record['uploadStatus'], 'uploaded')
                link = record['receipt']['directUrl']
                self.assertEqual(self.fetch(link), (200, synthetic_png()))
                upload_event = [event for event in self.receiver_call('/__state')['events'] if event['call'] == 'upload'][-1]
                self.assertEqual(upload_event['headers']['authorization'], 'Bearer ' + self.TOKEN)
                # Deletion from History: prepared with its effect, then confirmed.
                entry = await call('get_history_entry', {'id': record['id']})
                self.assertTrue(entry['result']['capabilities']['delete'])
                self.assertTrue(entry['result']['capabilities']['referenceDelete'])
                self.assertFalse(entry['result']['capabilities']['stat'])
                prepared = await call('delete_history_uploads', {'ids': [record['id']]})
                self.assertIn('deletion URL', prepared['result']['targets'][0]['effect'])
                confirmed = await call('delete_history_uploads', {'ids': [record['id']], 'acknowledgement': prepared['result']['acknowledgement']})
                self.assertTrue(confirmed['ok'], confirmed)
                removed = await until('get_history_entry', {'id': record['id']}, lambda result: result['remoteStatus'] == 'removed')
                self.assertEqual(removed['result']['remoteStatus'], 'removed', removed)
                self.assertEqual(self.fetch(link)[0], 404)

                # A reply lost after the file was stored: unknown, never resent on its own,
                # and the explicit retry leaves a second copy, as History says it may.
                self.receiver_call('/__rules', {'rules': [{'call': 'upload', 'store': True, 'drop': True}]})
                operation, finished, record = await upload()
                self.assertEqual(finished['state'], 'partial', finished)
                self.assertEqual(record['uploadStatus'], 'unknown')
                self.assertEqual(record['error']['code'], 'http_outcome_unknown')
                self.assertIn('second copy', record['error']['message'])
                self.assertEqual(len(self.stored()), 1)
                entry = await call('get_history_entry', {'id': record['id']})
                self.assertTrue(entry['result']['capabilities']['serverNamesObjects'])
                # #1753: the error log keeps it, open, with Retry; its Retry step is operation.retry, and
                # the upload that then works marks the same entry resolved.
                logged = await until('list_errors', {'operation_id': operation}, lambda result: result['entries'])
                problem = logged['result']['entries'][0]
                self.assertEqual((problem['kind'], problem['code'], problem['historyId'], problem['resolved']),
                                 ('upload', 'http_outcome_unknown', record['id'], False), problem)
                self.assertIn('retry', problem['nextSteps'])
                retried = await call('act_on_error', {'id': problem['id'], 'action': 'retry'})
                self.assertTrue(retried['ok'], retried)
                self.assertEqual(retried['result']['method'], 'operation.retry')
                finished = await until('get_operation', {'id': operation}, lambda result: result['state'] in ('completed', 'failed', 'cancelled'))
                self.assertEqual(finished['result']['state'], 'completed', finished)
                self.assertEqual(len(self.stored()), 2)
                resolved = await until('get_error', {'id': problem['id']}, lambda result: result['resolved'])
                self.assertTrue(resolved['result']['resolved'], resolved)
                cleared = await call('clear_errors', {'resolved_only': True})
                self.assertGreaterEqual(cleared['result']['removed'], 1, cleared)
                self.assertEqual((await call('get_error', {'id': problem['id']}))['error']['code'], 'error_not_found')

                # Diagnostics packages, with names kept and hashed: neither carries the token.
                for index, names in enumerate((True, False)):
                    package = self.root / f'package-{index}.zip'
                    bundle = await call('create_diagnostics_bundle', {'output': str(package), 'include_names': names})
                    self.assertTrue(bundle['ok'], bundle)
                    with zipfile.ZipFile(package) as archive:
                        members = archive.namelist()
                        self.assertIn('config.redacted.json', members)
                        for member in members:
                            self.assertNotIn(self.TOKEN.encode(), archive.read(member), member)
                        redacted = json.loads(archive.read('config.redacted.json'))
                    packaged = next(entry for entry in redacted['profiles'] if entry['provider'] == 'custom-http')
                    self.assertEqual(packaged['settings']['headers'], [{'name': 'Authorization', 'secret': True}] if names else packaged['settings']['headers'])
                    self.assertTrue(all('value' not in row for row in packaged['settings']['headers']))
                    self.assertEqual(packaged['credentials'], {'mode': 'session'})
        # Configuration, journal, artifacts, logs and both packages: the token is in none of them.
        self.assertEqual(self.files_with_token(), [])
        self.assertTrue((self.root / 'config/putgd/config.json').is_file())
        self.assertTrue(any((self.root / 'state/putgd/logs').glob('*.jsonl')))



class R2DestinationResidentTests(unittest.IsolatedAsyncioTestCase):
    """#1678 through the MCP helper against a real private resident and its Settings window
    (offscreen): Move to Cloudflare R2 from an S3 destination on R2, paste the dashboard's S3 API
    address, read the form, and check that showing a destination writes nothing. Set PUTGD_APP and
    PUTGD_CLI to the built production binaries. Synthetic settings, no credentials, no network."""
    ENDPOINT = 'https://0123456789abcdef0123456789abcdef.eu.r2.cloudflarestorage.com'

    async def asyncSetUp(self):
        self.cli, self.app = os.environ.get('PUTGD_CLI'), os.environ.get('PUTGD_APP')
        if not self.cli or not self.app:
            self.fail('PUTGD_CLI and PUTGD_APP must select the built production putgd-cli and putgd')
        self.directory = tempfile.TemporaryDirectory(prefix='putgd-r2ui-')
        self.addAsyncCleanup(self.stop_resident)
        self.root = Path(self.directory.name)
        runtime = self.root / 'rt'
        runtime.mkdir(mode=0o700)
        self.socket = str(runtime / 'putgd/session.sock')
        self.assertLessEqual(len(self.socket.encode('utf-8')), 107, 'Test socket exceeds Linux path limit')
        environment = os.environ | {'QT_QPA_PLATFORM': 'offscreen', 'XDG_RUNTIME_DIR': str(runtime), 'XDG_CONFIG_HOME': str(self.root / 'config'),
                                    'XDG_DATA_HOME': str(self.root / 'data'), 'XDG_STATE_HOME': str(self.root / 'state'),
                                    'XDG_CACHE_HOME': str(self.root / 'cache'), 'XDG_SESSION_TYPE': 'unspecified', 'XDG_CURRENT_DESKTOP': 'putgd-mcp-fixture'}
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'SESSION_MANAGER'):
            environment.pop(key, None)
        self.log = (self.root / 'resident.log').open('w')
        self.resident = subprocess.Popen([self.app, '--background'], env=environment, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 15
        while not Path(self.socket).exists() and self.resident.poll() is None and time.monotonic() < deadline:
            await asyncio.sleep(.05)
        self.assertTrue(Path(self.socket).exists(), 'Private resident did not start')

    async def stop_resident(self):
        if getattr(self, 'resident', None) is not None and self.resident.poll() is None:
            self.resident.terminate()
            try:
                self.resident.wait(10)
            except subprocess.TimeoutExpired:
                self.resident.kill()
                self.resident.wait()
        if hasattr(self, 'log'):
            self.log.close()
        if hasattr(self, 'directory'):
            self.directory.cleanup()

    async def test_move_paste_and_no_save_from_display(self):
        adapter = CliAdapter(self.cli, self.socket)
        parameters = StdioServerParameters(command=sys.executable, args=['-m', 'share_mcp.server', '--cli', self.cli, '--socket', self.socket])
        async with stdio_client(parameters) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()

                async def call(name, args):
                    reply = await session.call_tool(name, args)
                    self.assertFalse(reply.is_error, reply)
                    return reply.structured_content

                async def configuration():
                    return (await call('inspect', {}))['result']['configuration']['result']

                async def profile():
                    return next(entry for entry in (await configuration())['document']['profiles'] if entry['id'] == 'lab')

                async def form(args, done, seconds=30):
                    deadline = time.monotonic() + seconds
                    while True:
                        reply = await call('drive_r2_destination', {**args, 'profile_id': 'lab'})
                        if (reply['ok'] and done(reply['result'])) or time.monotonic() > deadline:
                            self.assertTrue(reply['ok'], reply)
                            self.assertTrue(done(reply['result']), reply['result'])
                            return reply['result']
                        await asyncio.sleep(.1)

                # An S3 destination on R2, saved with the dashboard location as its region (#1675).
                revision = (await configuration())['metadata']['observedRevision']
                s3 = {'id': 'lab', 'label': 'Lab', 'provider': 's3', 'credentials': {'mode': 'session', 'reference': None},
                      'settings': {'bucket': 'lab-bucket', 'region': 'Western North America (WNAM)', 'endpoint': self.ENDPOINT, 'objectPrefix': '',
                                   'storageClass': 'STANDARD', 'aclMode': 'omit', 'linkMode': 'public', 'linkValidityDays': 7, 'publicUrlBase': None,
                                   'addressing': 'auto', 'connectTimeoutMs': 5000, 'requestTimeoutMs': 60000, 'lowSpeedBytesPerSecond': 1, 'lowSpeedTimeSeconds': 30}}
                patched = await call('patch_configuration', {'patch': {'profiles': [s3]}, 'expected_revision': revision})
                self.assertTrue(patched['ok'], patched)
                # The baseline is the seeded file, before Destinations shows it: neither showing nor inspecting saves.
                revision = (await configuration())['metadata']['observedRevision']
                deadline = time.monotonic() + 30
                while not (shown := await adapter.call('ui.show', {'view': 'destinations'}))['ok'] and time.monotonic() < deadline:
                    await asyncio.sleep(.25)
                self.assertTrue(shown['ok'], shown)
                state = await form({'action': 'inspect'}, lambda result: result['moveAvailable'])
                self.assertEqual(state['provider'], 's3')
                await asyncio.sleep(1)
                self.assertEqual((await configuration())['metadata']['observedRevision'], revision, 'showing a destination saved it')

                # Move to Cloudflare R2: in place, the same ID and credentials, the account and jurisdiction read from the endpoint.
                await form({'action': 'move'}, lambda result: True)
                deadline = time.monotonic() + 30
                while (moved := await profile())['provider'] != 'cloudflare-r2' and time.monotonic() < deadline:
                    await asyncio.sleep(.1)
                self.assertEqual((moved['provider'], moved['credentials']), ('cloudflare-r2', s3['credentials']))
                self.assertEqual((moved['settings']['accountId'], moved['settings']['jurisdiction'], moved['settings']['bucket']),
                                 ('0123456789abcdef0123456789abcdef', 'eu', 'lab-bucket'))
                from jsonschema import Draft202012Validator
                Draft202012Validator(json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text(encoding='utf-8'))).validate((await configuration())['document'])
                state = await form({'action': 'inspect'}, lambda result: result['provider'] == 'cloudflare-r2' and result['saved'])
                self.assertEqual(state['endpoint'], self.ENDPOINT)
                self.assertFalse(state['moveAvailable'])
                # Move isn't offered for a destination that isn't S3 (R2 here, as for B2, Proton Drive or a custom uploader).
                again = await call('drive_r2_destination', {'action': 'move', 'profile_id': 'lab'})
                self.assertEqual(again['error']['code'], 'control_unavailable', again)
                # Public links with no Link domain are called out before any test (#1679).
                self.assertIn('r2.dev', state['linkDomainWarning'])
                self.assertEqual(state['bucketHint'], 'Uploads go to lab-bucket/ on Cloudflare R2')

                # The dashboard's S3 API address fills Account ID, Jurisdiction and Bucket, saved as one edit.
                pasted = await form({'action': 'paste', 'text': 'https://FEDCBA9876543210FEDCBA9876543210.r2.cloudflarestorage.com/other-bucket'},
                                    lambda result: True)
                self.assertEqual((pasted['accountId'], pasted['jurisdiction'], pasted['bucket']), ('fedcba9876543210fedcba9876543210', 'default', 'other-bucket'))
                await form({'action': 'inspect'}, lambda result: result['saved'])
                settings = (await profile())['settings']
                self.assertEqual((settings['accountId'], settings['jurisdiction'], settings['bucket']), ('fedcba9876543210fedcba9876543210', 'default', 'other-bucket'))
                # Percent-encoded text is not read as an address: it stays, with the Account ID message.
                kept = await form({'action': 'paste', 'text': 'https://fedcba9876543210fedcba9876543210.r2.cloudflarestorage.com/other%2Dbucket'},
                                  lambda result: result['errors']['accountId'] != '')
                self.assertEqual(kept['bucket'], 'other-bucket')
                self.assertEqual((await profile())['settings']['accountId'], 'fedcba9876543210fedcba9876543210')
                # A new Cloudflare R2 destination through the form, named as the dialog would name it, saves on its
                # own while Lab's draft is still incomplete (#1699): neither waits for or carries the other.
                added = await call('drive_r2_destination', {'action': 'add', 'label': 'Second R2'})
                self.assertTrue(added['ok'], added)
                self.assertEqual((added['result']['provider'], added['result']['label']), ('cloudflare-r2', 'Second R2'))
                second = added['result']['profileId']
                pasted = await call('drive_r2_destination', {'action': 'paste', 'text': 'https://00112233445566778899aabbccddeeff.eu.r2.cloudflarestorage.com/second-bucket',
                                                             'profile_id': second})
                self.assertTrue(pasted['ok'], pasted)
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    saved = next((entry for entry in (await configuration())['document']['profiles'] if entry['id'] == second), None)
                    if saved and saved['provider'] == 'cloudflare-r2' and saved['settings']['bucket'] == 'second-bucket':
                        break
                    await asyncio.sleep(.1)
                self.assertEqual((saved['settings']['accountId'], saved['settings']['jurisdiction'], saved['settings']['bucket']),
                                 ('00112233445566778899aabbccddeeff', 'eu', 'second-bucket'))
                self.assertEqual((await profile())['settings']['accountId'], 'fedcba9876543210fedcba9876543210')
                # Typed back to the saved account: nothing left to save, and no message.
                await form({'action': 'set', 'field': 'accountId', 'value': 'fedcba9876543210fedcba9876543210'}, lambda result: result['errors']['accountId'] == '' and result['saved'])
                # A Link domain clears the warning.
                state = await form({'action': 'set', 'field': 'publicUrlBase', 'value': 'https://pub-0123456789abcdef.r2.dev'}, lambda result: result['linkDomainWarning'] is None)
                self.assertIsNone(state['linkDomainWarning'])
                # The S3 API address is not a Link domain: R2's own message says so at Link domain.
                state = await form({'action': 'set', 'field': 'publicUrlBase', 'value': self.ENDPOINT}, lambda result: result['errors']['publicUrlBase'] != '')
                self.assertIn('S3 API address', state['errors']['publicUrlBase'])
                await form({'action': 'set', 'field': 'publicUrlBase', 'value': 'https://pub-0123456789abcdef.r2.dev'}, lambda result: result['errors']['publicUrlBase'] == '')

                # Everything a person can reach: who can open files, Links last and the advanced connection values.
                await form({'action': 'set', 'field': 'linkMode', 'value': 'presigned'}, lambda result: result['linkMode'] == 'presigned')
                await form({'action': 'set', 'field': 'linkValidityDays', 'value': '3'}, lambda result: result['linkValidityDays'] == 3)
                hidden = await call('drive_r2_destination', {'action': 'set', 'field': 'connectTimeoutMs', 'value': '9', 'profile_id': 'lab'})
                self.assertEqual(hidden['error']['code'], 'control_unavailable', hidden)
                await form({'action': 'set', 'field': 'advanced', 'value': 'open'}, lambda result: result['advancedOpen'])
                for key, value in (('connectTimeoutMs', '9'), ('requestTimeoutMs', '90'), ('lowSpeedTimeSeconds', '45')):
                    await form({'action': 'set', 'field': key, 'value': value}, lambda result: True)
                await form({'action': 'inspect'}, lambda result: result['saved'])
                settings = (await profile())['settings']
                self.assertEqual((settings['linkMode'], settings['linkValidityDays'], settings['connectTimeoutMs'], settings['requestTimeoutMs'], settings['lowSpeedTimeSeconds']),
                                 ('presigned', 3, 9000, 90000, 45))

if __name__ == '__main__':
    unittest.main()
