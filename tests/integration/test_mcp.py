"""Official SDK / production CLI contract tests; synthetic local IPC only.

Run with PUTGD_CLI pointing at the built production binary. No desktop/cloud proof.
"""
import asyncio
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from share_mcp.server import CliAdapter, create_server


class AdapterTests(unittest.IsolatedAsyncioTestCase):
    async def test_invalid_input_never_spawns_cli(self):
        adapter = CliAdapter('/does/not/exist')
        server = create_server(adapter)
        for name, args in [
            ('perform_action', {'action': 'shell'}),
            ('perform_action', {'action': 'clipboard', 'shell': 'ignored-extra'}),
            ('delete_history_uploads', {'ids': [], 'acknowledgement': ''}),
            ('patch_configuration', {'patch': {'accessKey': 'synthetic'}, 'expected_revision': 'r'}),
            ('patch_configuration', {'patch': {'captureDefaults': {'accessKey': 'synthetic'}}, 'expected_revision': 'r'}),
            ('list_history', {'limit': 201}),
        ]:
            result = await server.call_tool(name, args)
            self.assertIn('invalid_request', str(result), name)

    async def test_malformed_timeout_and_unavailable_cli(self):
        with tempfile.TemporaryDirectory(prefix='putgd mcp ') as directory:
            script = Path(directory) / 'fake cli'
            script.write_text('#!/usr/bin/env python3\nprint("not JSON")\n')
            script.chmod(0o700)
            result = await CliAdapter(str(script)).call('capabilities', {})
            self.assertEqual(result['error']['code'], 'invalid_response')
            script.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(5)\n')
            result = await CliAdapter(str(script), timeout=0.05).call('capabilities', {})
            self.assertEqual(result['error']['code'], 'cli_timeout')
        result = await CliAdapter('/does/not/exist').call('capabilities', {})
        self.assertEqual(result['error']['code'], 'cli_unavailable')


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
        if method == 'settings.get':
            result = {'document': {'actions': {'clipboard': {'destination': 'upload'}}},
                      'metadata': {'path': '/synthetic/config.json', 'acceptedRevision': 'synthetic-revision', 'observedRevision': 'synthetic-revision'}}
        if method == 'delete.prepare':
            result = {'acknowledgement': 'synthetic-effect', 'targets': request['params']['ids']}
        error = {'code': '', 'message': '', 'details': {}}
        if method == 'settings.patch':
            error['code'] = 'config_conflict'
            error['message'] = 'Synthetic concurrent editor'
        reply = {'schemaVersion': 1, 'requestId': request['requestId'],
                 'ok': not error['code'], 'result': result if not error['code'] else {}, 'error': error}
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
                expected = {'inspect', 'perform_action', 'get_operation', 'retry_operation',
                            'copy_result', 'cancel_operation', 'list_history', 'get_history_entry',
                            'copy_history_link', 'delete_history_uploads', 'reload_configuration',
                            'patch_configuration'}
                self.assertEqual({tool.name for tool in discovered}, expected)
                for tool in discovered:
                    self.assertEqual(tool.inputSchema['type'], 'object')
                    self.assertFalse(tool.inputSchema['additionalProperties'])
                async def call(name, args):
                    reply = await session.call_tool(name, args)
                    self.assertFalse(reply.isError, reply)
                    return reply.structuredContent
                for name, args in [
                    ('perform_action', {'action': 'clipboard', 'shell': 'extra'}),
                    ('perform_action', {'action': 17}),
                    ('list_history', {'limit': True}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': False}),
                ]:
                    rejected = await session.call_tool(name, args)
                    self.assertTrue(rejected.isError or (
                        rejected.structuredContent and
                        rejected.structuredContent['error']['code'] == 'invalid_request'), rejected)
                    self.assertEqual(self.calls, [], 'Invalid wire arguments must issue no IPC')
                inspected = await call('inspect', {})
                self.assertTrue(inspected['ok'])
                metadata = inspected['result']['configuration']['result']['metadata']
                self.assertEqual(metadata['path'], '/synthetic/config.json')
                self.assertEqual(metadata['observedRevision'], 'synthetic-revision')
                self.assertEqual([r['method'] for r in self.calls],
                                 ['capabilities', 'diagnostics', 'settings.get'])
                self.calls.clear()
                action = await call('perform_action', {'action': 'clipboard'})
                self.assertEqual(action['result']['operationId'], 'synthetic-operation')
                self.assertEqual(self.calls[-1]['method'], 'action.invoke')
                self.assertEqual(self.calls[-1]['params'], {'action': 'clipboard'})
                self.assertEqual(action['requestId'], self.calls[-1]['requestId'])
                cases = [
                    ('get_operation', {'id': 'synthetic-operation'}, 'operation.inspect', {'id': 'synthetic-operation'}),
                    ('get_operation', {'id': 'synthetic-delete', 'kind': 'delete'}, 'delete.inspect', {'id': 'synthetic-delete'}),
                    ('retry_operation', {'id': 'synthetic-operation'}, 'operation.retry', {'id': 'synthetic-operation'}),
                    ('cancel_operation', {'id': 'synthetic-operation'}, 'operation.cancel', {'id': 'synthetic-operation'}),
                    ('retry_operation', {'id': 'synthetic-operation', 'stage': 'local_save'}, 'operation.retryLocalSave', {'id': 'synthetic-operation'}),
                    ('retry_operation', {'id': 'synthetic-delete', 'stage': 'delete'}, 'delete.retry', {'id': 'synthetic-delete'}),
                    ('cancel_operation', {'id': 'synthetic-delete', 'kind': 'delete'}, 'delete.cancel', {'id': 'synthetic-delete'}),
                    ('copy_result', {'id': 'synthetic-operation'}, 'operation.copy', {'id': 'synthetic-operation'}),
                    ('get_history_entry', {'id': 'synthetic-entry'}, 'history.detail', {'id': 'synthetic-entry'}),
                    ('copy_history_link', {'id': 'synthetic-entry'}, 'history.copyLink', {'id': 'synthetic-entry'}),
                    ('list_history', {'limit': 2, 'offset': 1, 'search': 'name with spaces', 'remote_status': 'removed'}, 'history.list', {'limit': 2, 'offset': 1, 'search': 'name with spaces', 'remoteStatus': 'removed'}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry']}, 'delete.prepare', {'ids': ['synthetic-entry']}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'}, 'delete.confirm', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'}),
                    ('patch_configuration', {'patch': {'captureDefaults': {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}, 'actions': {'clipboard': {'destination': 'default', 'profileId': None}}}, 'expected_revision': 'synthetic-revision'}, 'settings.patch', {'patch': {'captureDefaults': {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}, 'actions': {'clipboard': {'destination': 'default', 'profileId': None}}}, 'expectedRevision': 'synthetic-revision'}),
                    ('reload_configuration', {}, 'settings.reload', {}),
                ]
                for name, args, method, params in cases:
                    await call(name, args)
                    self.assertEqual(self.calls[-1]['method'], method)
                    self.assertEqual(self.calls[-1]['params'], params)
                conflict = await call('patch_configuration', {'patch': {'preferences': {'notifications': False}}, 'expected_revision': 'stale'})
                self.assertEqual(conflict['error']['code'], 'config_conflict')
                self.assertTrue(all(r['schemaVersion'] == 1 and r['session'] == self.socket for r in self.calls))
        self.listener.close()
        await self.listener.wait_closed()
        # Python 3.13+ removes the Unix socket when the asyncio server closes.
        Path(self.socket).unlink(missing_ok=True)
        unavailable = await CliAdapter(self.cli, self.socket).call('capabilities', {})
        self.assertEqual(unavailable['error']['code'], 'session_unavailable')


if __name__ == '__main__':
    unittest.main()
