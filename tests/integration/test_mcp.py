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
from share_mcp.server import CliAdapter, create_server


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
            ('perform_action', {'action': 'shell'}),
            ('delete_history_uploads', {'ids': [], 'acknowledgement': ''}),
            ('patch_configuration', {'patch': {'accessKey': 'synthetic'}, 'expected_revision': 'r'}),
            ('patch_configuration', {'patch': {'captureDefaults': {'accessKey': 'synthetic'}}, 'expected_revision': 'r'}),
            ('install_update', {'confirm': True, 'image': '/tmp/x.AppImage'}),
        ]:
            result = await server.call_tool(name, args)
            envelope = result.structured_content or json.loads(result.content[0].text)
            self.assertEqual(envelope['error']['code'], 'invalid_request', name)

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
                Draft202012Validator(json.loads((Path(__file__).resolve().parents[2] / 'schemas/config.schema.json').read_text())).validate(configuration['document'])
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
                retried = await call('retry_operation', {'id': operation})
                self.assertTrue(retried['ok'], retried)
                finished = await until('get_operation', {'id': operation}, lambda result: result['state'] in ('completed', 'failed', 'cancelled'))
                self.assertEqual(finished['result']['state'], 'completed', finished)
                self.assertEqual(len(self.stored()), 2)

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


if __name__ == '__main__':
    unittest.main()
