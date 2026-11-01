"""Official SDK / production CLI contract tests; synthetic local IPC only.

Run with PUTGD_CLI pointing at the built production binary. No desktop/cloud proof.
"""
import asyncio
import base64
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from share_mcp.server import CLI_TIMEOUT, CliAdapter, create_server


def synthetic_png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return len(data).to_bytes(4, 'big') + kind + data + zlib.crc32(kind + data).to_bytes(4, 'big')
    header = (2).to_bytes(4, 'big') + (1).to_bytes(4, 'big') + bytes([8, 2, 0, 0, 0])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(b'\x00' + bytes(6))) + chunk(b'IEND', b''))


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
            ('perform_action', {'action': 'recording'}),
            ('open_editor', {'source': 'shell'}),
            ('open_editor', {'source': 'file'}),
            ('open_editor', {'source': 'file', 'path': 'relative.png'}),
            ('open_editor', {'source': 'region', 'path': '/synthetic.png'}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': []}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'shell'}]}),
            ('edit_image', {'id': 'synthetic-operation', 'commands': [{'op': 'add', 'shape': {'password': 'synthetic'}}]}),
            ('preview_editor', {'id': 'synthetic-operation', 'max_edge': 10}),
            ('save_editor_copy', {'id': 'synthetic-operation', 'path': 'relative.png'}),
            ('submit_editor', {'id': ''}),
            ('reorder_destinations', {'order': ['a', ''], 'expected_revision': 'r'}),
            ('reorder_destinations', {'order': ['a' * 65], 'expected_revision': 'r'}),
            ('move_destination', {'direction': 'left'}),
            ('move_destination', {'direction': 'up', 'profile_id': ''}),
            ('settings_diagnostics', {'action': 'shell'}),
            ('settings_diagnostics', {'action': 'setDebugLogging'}),
            ('settings_diagnostics', {'action': 'setDesktopDetails'}),
            ('settings_diagnostics', {'action': 'copyDetails', 'enabled': True}),
            ('settings_diagnostics', {'action': 'inspect', 'enabled': True}),
            ('settings_diagnostics', {'action': 'setPackage', 'folder': 'relative'}),
            ('settings_diagnostics', {'action': 'savePackage', 'folder': '/synthetic'}),
            ('settings_diagnostics', {'action': 'clearLogs', 'operation_id': 'synthetic-operation'}),
            ('settings_diagnostics', {'action': 'savePackage', 'upload': True}),
            ('settings_diagnostics', {'action': 'setPackage', 'upload': 'yes'}),
            ('history_save_diagnostics', {'id': ''}),
            ('copy_history_links', {'ids': []}),
            ('copy_history_links', {'ids': ['same', 'same']}),
            ('drive_history', {'action': 'scroll'}),
            ('drive_history', {'action': 'select'}),
            ('drive_history', {'action': 'inspect', 'ids': ['synthetic-entry']}),
            ('drive_history', {'action': 'clearSelection', 'scope': 'all'}),
            ('drive_history', {'action': 'clearFilters', 'scope': 'everything'}),
            ('make_history_preview', {'id': ''}),
            ('make_history_preview', {'id': 'synthetic-entry', 'retry': 'yes'}),
            ('make_history_preview', {'id': 'synthetic-entry', 'retry': 1}),
            ('drive_file_dialog', {'dialog': 'shell', 'action': 'open'}),
            ('drive_file_dialog', {'dialog': 'editImage', 'action': 'accept'}),
            ('drive_file_dialog', {'dialog': 'editImage', 'action': 'open', 'field': 'root'}),
            ('drive_file_dialog', {'dialog': 'outputFolder', 'action': 'open', 'field': 'music'}),
            ('drive_file_dialog', {'dialog': 'outputFolder', 'action': 'open', 'id': 'synthetic-operation'}),
            ('drive_file_dialog', {'dialog': 'historySaveAs', 'action': 'open', 'id': ''}),
            ('editor_file_dialog', {'id': 'synthetic-operation', 'action': 'accept'}),
            ('editor_file_dialog', {'id': '', 'action': 'open'}),
            ('show_diagnostics_package', {'path': 'relative.zip'}),
            ('create_diagnostics_bundle', {'preview': True, 'notify': True}),
            ('check_for_updates', {'channel': 'nightly'}),
            ('get_update_status', {'force': True}),
            ('open_software_updates', {'updater': 'gnome-software'}),
            ('install_update', {'confirm': True, 'method': 'torrent'}),
            ('install_update', {'confirm': True, 'image': '/tmp/x.AppImage'}),
            ('install_update', {'confirm': True, 'channel': 'beta'}),
            ('install_update', {'confirm': True, 'channel': 'stable', 'allow_older': 'maybe'}),
            ('restart_after_update', {'confirm': True, 'version': '0.1.2'}),
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
            ('perform_action', {'action': 17}),
            ('perform_action', {}),
            ('perform_action', {'action': 'clipboard', 'extra': True}),
            ('list_history', {'limit': '10'}),
            ('list_history', {'limit': True}),
            ('list_history', {'limit': 1.5}),
            ('make_history_preview', {'id': 'synthetic-entry', 'retry': 'true'}),
            ('make_history_preview', {'id': 'synthetic-entry', 'retry': 1}),
            ('delete_history_uploads', {'ids': '["synthetic-entry"]'}),
            ('delete_history_uploads', {'ids': [17]}),
            ('patch_configuration', {'patch': '{}', 'expected_revision': 'r'}),
            ('select_recording_region', {'id': 'synthetic-operation', 'rect': '[0, 0, 10, 10]'}),
            ('select_recording_region', {'id': 'synthetic-operation', 'rect': [0, '0', 10, 10]}),
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

    def test_default_deadline_outlasts_the_cli(self):
        # The CLI's longest bounded wait: 3 s to connect, 3 s to send, 45 s for recording.stop.
        self.assertGreater(CLI_TIMEOUT, 3 + 3 + 45)
        self.assertEqual(CliAdapter().timeout, CLI_TIMEOUT)


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
        if method == 'recording.stop' and request['params'].get('id') == 'slow-stop':
            await asyncio.sleep(16)
        result = {'operationId': 'synthetic-operation', 'method': method, 'params': request['params']}
        if method == 'settings.get':
            result = {'document': {'actions': {'clipboard': {'destination': 'upload'}}},
                      'metadata': {'path': '/synthetic/config.json', 'acceptedRevision': 'synthetic-revision', 'observedRevision': 'synthetic-revision'}}
        if method == 'recording.preview':
            result = {'operationId': 'synthetic-operation', 'timeUs': request['params']['time'], 'retained': True, 'outputTimeUs': 0, 'revision': 1,
                      'png': base64.b64encode(synthetic_png()).decode()}
        if method == 'editor.preview':
            result = {'operationId': 'synthetic-operation', 'mime': 'image/png', 'width': 2, 'height': 1,
                      'sourceWidth': 2, 'sourceHeight': 1, 'png': base64.b64encode(synthetic_png()).decode()}
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

    async def test_slow_recording_stop_reaches_the_caller(self):
        # The resident answers stop only after the helper, whose deadline is 30 s: the
        # production CLI must wait past its ordinary 15 s, and the adapter past the CLI.
        reply = await CliAdapter(self.cli, self.socket).call('recording.stop', {'id': 'slow-stop'})
        self.assertTrue(reply['ok'], reply)
        self.assertEqual(reply['result']['method'], 'recording.stop')

    async def test_official_sdk_discovery_and_cli_parity(self):
        parameters = StdioServerParameters(command=sys.executable, args=[
            '-m', 'share_mcp.server', '--cli', self.cli, '--socket', self.socket])
        async with stdio_client(parameters) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                discovered = (await session.list_tools()).tools
                expected = {'inspect', 'get_view_state', 'perform_action', 'get_operation', 'retry_operation',
                            'copy_result', 'cancel_operation', 'list_history', 'get_history_entry',
                            'make_history_preview', 'copy_history_link', 'copy_history_links', 'open_history_link', 'delete_history_uploads', 'reload_configuration',
                            'patch_configuration', 'open_editor', 'get_editor', 'edit_image',
                            'preview_editor', 'save_editor_copy', 'submit_editor', 'cancel_editor',
                            'raise_editor', 'get_recording_capabilities', 'prepare_recording', 'control_recording',
                            'get_recording', 'select_recording_region', 'set_recording_overlay', 'edit_recording', 'preview_recording', 'drive_recording_review',
                            'save_recording_copy', 'upload_recording', 'reorder_destinations', 'move_destination',
                            'clear_logs', 'create_diagnostics_bundle', 'get_diagnostics_upload_target', 'show_diagnostics_package',
                            'settings_diagnostics', 'history_save_diagnostics', 'drive_history', 'get_update_status', 'check_for_updates',
                            'open_software_updates', 'install_update', 'restart_after_update', 'drive_file_dialog', 'editor_file_dialog',
                            'drive_editor', 'upload_files'}
                self.assertEqual({tool.name for tool in discovered}, expected)
                review = next(tool for tool in discovered if tool.name == 'drive_recording_review')
                self.assertIn('Use Redact', review.description)
                self.assertIn('serialized kind stays solid', review.description)
                for tool in discovered:
                    self.assertEqual(tool.input_schema['type'], 'object')
                    self.assertFalse(tool.input_schema['additionalProperties'])
                inspection = next(tool for tool in discovered if tool.name == 'inspect')
                self.assertTrue(inspection.annotations.read_only_hint)
                self.assertFalse(inspection.annotations.destructive_hint)
                self.assertFalse(inspection.annotations.open_world_hint)
                async def call(name, args):
                    reply = await session.call_tool(name, args)
                    self.assertFalse(reply.is_error, reply)
                    return reply.structured_content
                for name, args in [
                    ('perform_action', {'action': 'clipboard', 'shell': 'extra'}),
                    ('perform_action', {'action': 17}),
                    ('create_diagnostics_bundle', {'output': 'relative.zip'}),
                    ('create_diagnostics_bundle', {'include_names': 'yes'}),
                    ('create_diagnostics_bundle', {'profile_id': 'synthetic-s3'}),
                    ('create_diagnostics_bundle', {'copy_link': True}),
                    ('create_diagnostics_bundle', {'upload': 'yes'}),
                    ('create_diagnostics_bundle', {'preview': True, 'upload': True}),
                    ('list_history', {'limit': True}),
                    ('list_history', {'limit': '10'}),
                    ('list_history', {'limit': 1.5}),
                    ('delete_history_uploads', {'ids': '["synthetic-entry"]'}),
                    ('delete_history_uploads', {'ids': [17]}),
                    ('patch_configuration', {'patch': '{}', 'expected_revision': 'r'}),
                    ('select_recording_region', {'id': 'synthetic-operation', 'rect': '[0, 0, 10, 10]'}),
                    ('make_history_preview', {'id': 'synthetic-entry', 'retry': 'true'}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': False}),
                    ('cancel_operation', {'id': 'synthetic-delete', 'kind': 'delete', 'item_id': 'synthetic-item'}),
                    ('cancel_operation', {'id': 'synthetic-operation', 'item_id': ''}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'submit'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'lasso'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'zoom', 'percent': 1}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'geometry', 'rect': [1, 2, 3]}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'select', 'shape_id': 0}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'pan', 'delta': [1.5, 2]}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'propertiesWidth', 'pixels': 199}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'propertiesWidth', 'pixels': 10001}),
                ]:
                    rejected = await session.call_tool(name, args)
                    self.assertIsNotNone(rejected.structured_content, rejected)
                    self.assertEqual(rejected.structured_content['error']['code'], 'invalid_request', rejected)
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
                    ('get_operation', {'id': 'synthetic-share', 'kind': 'share'}, 'share.inspect', {'id': 'synthetic-share'}),
                    ('retry_operation', {'id': 'synthetic-operation'}, 'operation.retry', {'id': 'synthetic-operation'}),
                    ('cancel_operation', {'id': 'synthetic-operation'}, 'operation.cancel', {'id': 'synthetic-operation'}),
                    ('retry_operation', {'id': 'synthetic-operation', 'stage': 'local_save'}, 'operation.retryLocalSave', {'id': 'synthetic-operation'}),
                    ('retry_operation', {'id': 'synthetic-delete', 'stage': 'delete'}, 'delete.retry', {'id': 'synthetic-delete'}),
                    ('cancel_operation', {'id': 'synthetic-delete', 'kind': 'delete'}, 'delete.cancel', {'id': 'synthetic-delete'}),
                    ('cancel_operation', {'id': 'synthetic-operation', 'item_id': 'synthetic-item'}, 'operation.cancel', {'id': 'synthetic-operation', 'itemId': 'synthetic-item'}),
                    ('patch_configuration', {'patch': {'limits': {'maxFileBytes': None, 'pendingArtifactBytes': 'auto', 'concurrentUploads': 4}}, 'expected_revision': 'synthetic-revision'}, 'settings.patch', {'patch': {'limits': {'maxFileBytes': None, 'pendingArtifactBytes': 'auto', 'concurrentUploads': 4}}, 'expectedRevision': 'synthetic-revision'}),
                    ('copy_result', {'id': 'synthetic-operation'}, 'operation.copy', {'id': 'synthetic-operation'}),
                    ('get_history_entry', {'id': 'synthetic-entry'}, 'history.detail', {'id': 'synthetic-entry'}),
                    ('make_history_preview', {'id': 'synthetic-entry'}, 'history.preview', {'id': 'synthetic-entry'}),
                    ('make_history_preview', {'id': 'synthetic-entry', 'retry': True}, 'history.preview', {'id': 'synthetic-entry', 'retry': True}),
                    ('make_history_preview', {'id': 'synthetic-entry', 'retry': False}, 'history.preview', {'id': 'synthetic-entry', 'retry': False}),
                    ('drive_history', {'action': 'cancelAll'}, 'ui.history', {'action': 'cancelAll'}),
                    ('drive_history', {'action': 'confirmCancelAll'}, 'ui.history', {'action': 'confirmCancelAll'}),
                    ('drive_history', {'action': 'keepUploading'}, 'ui.history', {'action': 'keepUploading'}),
                    ('copy_history_link', {'id': 'synthetic-entry'}, 'history.copyLink', {'id': 'synthetic-entry'}),
                    ('copy_history_links', {'ids': ['synthetic-entry', 'synthetic-second']}, 'history.copyLinks', {'ids': ['synthetic-entry', 'synthetic-second']}),
                    ('open_history_link', {'id': 'synthetic-entry'}, 'history.openLink', {'id': 'synthetic-entry'}),
                    ('list_history', {'limit': 2, 'offset': 1, 'search': 'name with spaces', 'remote_status': 'removed'}, 'history.list', {'limit': 2, 'offset': 1, 'search': 'name with spaces', 'remoteStatus': 'removed'}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry']}, 'delete.prepare', {'ids': ['synthetic-entry']}),
                    ('delete_history_uploads', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'}, 'delete.confirm', {'ids': ['synthetic-entry'], 'acknowledgement': 'synthetic-effect'}),
                    ('patch_configuration', {'patch': {'captureDefaults': {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}, 'actions': {'clipboard': {'destination': 'default', 'profileId': None}}}, 'expected_revision': 'synthetic-revision'}, 'settings.patch', {'patch': {'captureDefaults': {'destination': 'local', 'profileId': None, 'publicationMode': 'default'}, 'actions': {'clipboard': {'destination': 'default', 'profileId': None}}}, 'expectedRevision': 'synthetic-revision'}),
                    ('patch_configuration', {'patch': {'preferences': {'regionSelectionMode': 'screenshot_first'}}, 'expected_revision': 'synthetic-revision'}, 'settings.patch', {'patch': {'preferences': {'regionSelectionMode': 'screenshot_first'}}, 'expectedRevision': 'synthetic-revision'}),
                    ('reload_configuration', {}, 'settings.reload', {}),
                    ('reorder_destinations', {'order': ['second', 'first'], 'expected_revision': 'synthetic-revision'}, 'settings.reorderProfiles', {'order': ['second', 'first'], 'expectedRevision': 'synthetic-revision'}),
                    ('move_destination', {'direction': 'down'}, 'ui.moveDestination', {'direction': 'down'}),
                    ('move_destination', {'direction': 'up', 'profile_id': 'second'}, 'ui.moveDestination', {'direction': 'up', 'profileId': 'second'}),
                    ('clear_logs', {}, 'logs.clear', {}),
                    ('get_update_status', {}, 'update.status', {}),
                    ('check_for_updates', {}, 'update.check', {}),
                    ('open_software_updates', {}, 'update.openSoftwareUpdates', {}),
                    ('install_update', {'confirm': True}, 'update.install', {'confirm': True}),
                    ('install_update', {'confirm': True, 'method': 'download'}, 'update.install', {'confirm': True, 'method': 'download'}),
                    ('install_update', {'confirm': True, 'channel': 'stable'}, 'update.install', {'confirm': True, 'channel': 'stable'}),
                    ('install_update', {'confirm': True, 'channel': 'nightly', 'allow_older': True}, 'update.install', {'confirm': True, 'channel': 'nightly', 'allowOlder': True}),
                    ('restart_after_update', {'confirm': True}, 'update.restart', {'confirm': True}),
                    ('create_diagnostics_bundle', {}, 'diagnostics.bundle', {}),
                    ('create_diagnostics_bundle', {'operation_id': 'synthetic-operation', 'since': '24h', 'include_names': True, 'output': '/synthetic dir/package.zip'},
                     'diagnostics.bundle', {'operation': 'synthetic-operation', 'since': '24h', 'includeNames': True, 'output': '/synthetic dir/package.zip'}),
                    ('create_diagnostics_bundle', {'preview': True}, 'diagnostics.bundle', {'preview': True}),
                    ('create_diagnostics_bundle', {'operation_id': 'synthetic-operation', 'notify': True}, 'diagnostics.bundle', {'operation': 'synthetic-operation', 'notify': True}),
                    ('show_diagnostics_package', {'path': '/synthetic dir/putgd-diagnostics-20261020-010203Z.zip'}, 'diagnostics.reveal', {'path': '/synthetic dir/putgd-diagnostics-20261020-010203Z.zip'}),
                    ('settings_diagnostics', {'action': 'inspect'}, 'ui.diagnostics', {'action': 'inspect'}),
                    ('settings_diagnostics', {'action': 'setDesktopDetails', 'enabled': True}, 'ui.diagnostics', {'action': 'setDesktopDetails', 'enabled': True}),
                    ('settings_diagnostics', {'action': 'copyDetails'}, 'ui.diagnostics', {'action': 'copyDetails'}),
                    ('settings_diagnostics', {'action': 'showConfigFolder'}, 'ui.diagnostics', {'action': 'showConfigFolder'}),
                    ('settings_diagnostics', {'action': 'refreshDesktopSupport'}, 'ui.diagnostics', {'action': 'refreshDesktopSupport'}),
                    ('settings_diagnostics', {'action': 'setDebugLogging', 'enabled': False}, 'ui.diagnostics', {'action': 'setDebugLogging', 'enabled': False}),
                    ('settings_diagnostics', {'action': 'openPackage', 'operation_id': 'synthetic-operation', 'include_names': True}, 'ui.diagnostics', {'action': 'openPackage', 'operation': 'synthetic-operation', 'includeNames': True}),
                    ('settings_diagnostics', {'action': 'setPackage', 'folder': '/synthetic dir'}, 'ui.diagnostics', {'action': 'setPackage', 'folder': '/synthetic dir'}),
                    ('settings_diagnostics', {'action': 'setPackage', 'upload': True}, 'ui.diagnostics', {'action': 'setPackage', 'upload': True}),
                    ('settings_diagnostics', {'action': 'savePackage'}, 'ui.diagnostics', {'action': 'savePackage'}),
                    ('history_save_diagnostics', {'id': 'synthetic-operation'}, 'ui.historySaveDiagnostics', {'id': 'synthetic-operation'}),
                    ('drive_history', {'action': 'inspect'}, 'ui.history', {'action': 'inspect'}),
                    ('drive_history', {'action': 'retryPreview'}, 'ui.history', {'action': 'retryPreview'}),
                    ('drive_history', {'action': 'retryLoading'}, 'ui.history', {'action': 'retryLoading'}),
                    ('drive_history', {'action': 'copyDeletionDetails'}, 'ui.history', {'action': 'copyDeletionDetails'}),
                    ('drive_history', {'action': 'expandPreview'}, 'ui.history', {'action': 'expandPreview'}),
                    ('drive_history', {'action': 'closePreview'}, 'ui.history', {'action': 'closePreview'}),
                    ('drive_history', {'action': 'showSavedCopy'}, 'ui.history', {'action': 'showSavedCopy'}),
                    ('drive_history', {'action': 'copySavedPath'}, 'ui.history', {'action': 'copySavedPath'}),
                    ('drive_history', {'action': 'dismissSavedCopy'}, 'ui.history', {'action': 'dismissSavedCopy'}),
                    ('drive_history', {'action': 'clearFilters', 'scope': 'all'}, 'ui.history', {'action': 'clearFilters', 'scope': 'all'}),
                    ('drive_history', {'action': 'select', 'ids': ['synthetic-entry', 'operation:synthetic-operation']}, 'ui.history', {'action': 'select', 'ids': ['synthetic-entry', 'operation:synthetic-operation']}),
                    ('drive_file_dialog', {'dialog': 'editImage', 'action': 'open'}, 'ui.fileDialog', {'dialog': 'editImage', 'action': 'open'}),
                    ('drive_file_dialog', {'dialog': 'outputFolder', 'action': 'open', 'field': 'videos'}, 'ui.fileDialog', {'dialog': 'outputFolder', 'action': 'open', 'field': 'videos'}),
                    ('drive_file_dialog', {'dialog': 'historySaveAs', 'action': 'open', 'id': 'synthetic-entry'}, 'ui.fileDialog', {'dialog': 'historySaveAs', 'action': 'open', 'id': 'synthetic-entry'}),
                    ('drive_file_dialog', {'dialog': 'diagnosticsFolder', 'action': 'cancel'}, 'ui.fileDialog', {'dialog': 'diagnosticsFolder', 'action': 'cancel'}),
                    ('create_diagnostics_bundle', {'upload': True}, 'diagnostics.bundle', {'upload': True}),
                    ('create_diagnostics_bundle', {'upload': True, 'profile_id': 'synthetic-s3', 'copy_link': True},
                     'diagnostics.bundle', {'upload': True, 'profileId': 'synthetic-s3', 'copyLink': True}),
                    ('get_diagnostics_upload_target', {}, 'diagnostics.uploadTarget', {}),
                    ('get_diagnostics_upload_target', {'profile_id': 'synthetic-s3'}, 'diagnostics.uploadTarget', {'profileId': 'synthetic-s3'}),
                    ('get_view_state', {}, 'ui.viewState', {}),
                    ('perform_action', {'action': 'edit'}, 'action.invoke', {'action': 'edit'}),
                    ('upload_files', {'paths': ['/synthetic dir/a.png', '/synthetic dir/folder']}, 'upload.files', {'paths': ['/synthetic dir/a.png', '/synthetic dir/folder']}),
                    ('upload_files', {'paths': ['/synthetic dir/a.png'], 'profile_id': 'synthetic-s3'}, 'upload.files', {'paths': ['/synthetic dir/a.png'], 'profileId': 'synthetic-s3'}),
                    ('open_editor', {}, 'editor.open', {'source': 'region'}),
                    ('open_editor', {'source': 'file', 'path': '/synthetic dir/image.png'}, 'editor.open', {'source': 'file', 'path': '/synthetic dir/image.png'}),
                    ('get_editor', {'id': 'synthetic-operation'}, 'editor.inspect', {'id': 'synthetic-operation'}),
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [
                        {'op': 'add', 'shape': {'kind': 'redact', 'rect': [1, 2, 30, 40]}},
                        {'op': 'add', 'shape': {'kind': 'text', 'at': [5, 5], 'text': 'synthetic', 'color': '#ffffff'}},
                        {'op': 'crop', 'rect': [0, 0, 100, 50]}, {'op': 'undo'}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [
                        {'op': 'add', 'shape': {'kind': 'redact', 'rect': [1, 2, 30, 40]}},
                        {'op': 'add', 'shape': {'kind': 'text', 'at': [5, 5], 'text': 'synthetic', 'color': '#ffffff'}},
                        {'op': 'crop', 'rect': [0, 0, 100, 50]}, {'op': 'undo'}]}),
                    ('save_editor_copy', {'id': 'synthetic-operation', 'path': '/synthetic/copy.png'}, 'editor.saveCopy', {'id': 'synthetic-operation', 'path': '/synthetic/copy.png'}),
                    ('save_editor_copy', {'id': 'synthetic-operation', 'path': '/synthetic/copy.png', 'overwrite': True}, 'editor.saveCopy', {'id': 'synthetic-operation', 'path': '/synthetic/copy.png', 'overwrite': True}),
                    ('submit_editor', {'id': 'synthetic-operation'}, 'editor.submit', {'id': 'synthetic-operation'}),
                    ('cancel_editor', {'id': 'synthetic-operation'}, 'editor.cancel', {'id': 'synthetic-operation'}),
                    ('raise_editor', {'id': 'synthetic-operation'}, 'editor.raise', {'id': 'synthetic-operation'}),
                    ('editor_file_dialog', {'id': 'synthetic-operation', 'action': 'inspect'}, 'editor.fileDialog', {'id': 'synthetic-operation', 'action': 'inspect'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'inspect'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'inspect'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'crop'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'crop'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'select', 'shape_id': 3}, 'editor.view', {'id': 'synthetic-operation', 'action': 'select', 'shapeId': 3}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'select'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'select', 'shapeId': None}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'geometry', 'rect': [10, 20, -30, 40]}, 'editor.view', {'id': 'synthetic-operation', 'action': 'geometry', 'rect': [10, 20, -30, 40]}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'zoom', 'percent': 150}, 'editor.view', {'id': 'synthetic-operation', 'action': 'zoom', 'percent': 150}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'pan', 'delta': [-40, 25]}, 'editor.view', {'id': 'synthetic-operation', 'action': 'pan', 'delta': [-40, 25]}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'propertiesWidth', 'pixels': 240}, 'editor.view', {'id': 'synthetic-operation', 'action': 'propertiesWidth', 'pixels': 240}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'removeMark'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'removeMark'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'selectAffectedText'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'selectAffectedText'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'saveDetails'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'saveDetails'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'copySavedPath'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'copySavedPath'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'discard'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'discard'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'dismissSavedCopy'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'dismissSavedCopy'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'keyboardHelp'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'keyboardHelp'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'geometryDetails'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'geometryDetails'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'keepEditing'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'keepEditing'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'confirmDiscard'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'confirmDiscard'}),
                    ('get_recording_capabilities', {}, 'recording.capabilities', {}),
                    ('prepare_recording', {'target': {'kind': 'region', 'rect': [100, 100, 1280, 720]}, 'format': 'mp4', 'system_audio': True, 'fps': 24},
                     'recording.prepare', {'target': {'kind': 'region', 'rect': [100, 100, 1280, 720]}, 'format': 'mp4', 'audio': {'system': True}, 'fps': 24}),
                    ('prepare_recording', {'target': {'kind': 'monitor', 'identity': 'DP-3'}, 'format': 'gif', 'max_edge': 640},
                     'recording.prepare', {'target': {'kind': 'monitor', 'identity': 'DP-3'}, 'format': 'gif', 'maxEdge': 640}),
                    ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'webm', 'codec': 'vp9'},
                     'recording.prepare', {'target': {'kind': 'desktop'}, 'format': 'webm', 'codec': 'vp9'}),
                    # #984: 60 fps without the adaptive back-off (lab A/B).
                    ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'fps': 60, 'adaptive_fps': False},
                     'recording.prepare', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'fps': 60, 'adaptiveFps': False}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'start'}, 'recording.start', {'id': 'synthetic-operation'}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'pause'}, 'recording.pause', {'id': 'synthetic-operation'}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'resume'}, 'recording.resume', {'id': 'synthetic-operation'}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'stop'}, 'recording.stop', {'id': 'synthetic-operation'}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'cancel'}, 'recording.cancel', {'id': 'synthetic-operation'}),
                    ('control_recording', {'id': 'synthetic-operation', 'command': 'raise'}, 'recording.raise', {'id': 'synthetic-operation'}),
                    ('get_recording', {'id': 'synthetic-operation'}, 'recording.inspect', {'id': 'synthetic-operation'}),
                    ('select_recording_region', {'id': 'synthetic-operation', 'rect': [40, 50, 640, 360]}, 'recording.select', {'id': 'synthetic-operation', 'rect': [40, 50, 640, 360]}),
                    ('set_recording_overlay', {'id': 'synthetic-operation', 'visible': False}, 'recording.overlay', {'id': 'synthetic-operation', 'visible': False}),
                    ('edit_recording', {'id': 'synthetic-operation', 'revision': 1, 'commands': [
                        {'op': 'setTrim', 'start': 10000000, 'end': 30000000},
                        {'op': 'addMask', 'mask': {'kind': 'blur', 'rect': [100, 100, 200, 50], 'start': 12000000, 'end': 15000000}},
                        {'op': 'removeMask', 'maskId': 'synthetic-mask'}]},
                     'recording.edit', {'id': 'synthetic-operation', 'revision': 1, 'commands': [
                        {'op': 'setTrim', 'start': 10000000, 'end': 30000000},
                        {'op': 'addMask', 'mask': {'kind': 'blur', 'rect': [100, 100, 200, 50], 'start': 12000000, 'end': 15000000}},
                        {'op': 'removeMask', 'maskId': 'synthetic-mask'}]}),
                    ('save_recording_copy', {'id': 'synthetic-operation', 'path': '/synthetic/clip.mp4'}, 'recording.saveCopy', {'id': 'synthetic-operation', 'path': '/synthetic/clip.mp4'}),
                    ('save_recording_copy', {'id': 'synthetic-operation', 'path': '/synthetic/clip.mp4', 'overwrite': True}, 'recording.saveCopy', {'id': 'synthetic-operation', 'path': '/synthetic/clip.mp4', 'overwrite': True}),
                    ('upload_recording', {'id': 'synthetic-operation', 'revision': 2}, 'recording.upload', {'id': 'synthetic-operation', 'revision': 2}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'inspect'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'inspect'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'seek', 'time_us': 2500000}, 'recording.review', {'id': 'synthetic-operation', 'action': 'seek', 'time': 2500000}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'showMaskOptions'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'showMaskOptions'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'closeMaskOptions'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'closeMaskOptions'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'nextMaskLanes'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'nextMaskLanes'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'previousMaskLanes'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'previousMaskLanes'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'setIn'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'setIn'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'saveCopyDialog'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'saveCopyDialog'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'speed', 'speed': 0.5}, 'recording.review', {'id': 'synthetic-operation', 'action': 'speed', 'speed': 0.5}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'addMask', 'kind': 'solid', 'rect': [10, 20, 300, 80]},
                     'recording.review', {'id': 'synthetic-operation', 'action': 'addMask', 'kind': 'solid', 'rect': [10, 20, 300, 80]}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'selectMask', 'mask_id': 'synthetic-mask'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'selectMask', 'maskId': 'synthetic-mask'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'selectMask'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'selectMask', 'maskId': None}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'strength', 'strength': 24}, 'recording.review', {'id': 'synthetic-operation', 'action': 'strength', 'strength': 24}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'format', 'format': 'webm', 'codec': 'vp9'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'format', 'format': 'webm', 'codec': 'vp9'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'addMask', 'kind': 'blur'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'addMask', 'kind': 'blur'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'placeMask', 'rect': [8, 8, 100, 50]}, 'recording.review', {'id': 'synthetic-operation', 'action': 'placeMask', 'rect': [8, 8, 100, 50]}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'useSolidCover'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'useSolidCover'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'dismissBlurTip'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'dismissBlurTip'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'dismissStatus'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'dismissStatus'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'showAdvice'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'showAdvice'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'showFormatTips'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'showFormatTips'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'copyPath'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'copyPath'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'showInFolder'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'showInFolder'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'dismissSavedCopy'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'dismissSavedCopy'}),

                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'retryExport'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'retryExport'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'retryPreview'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'retryPreview'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'discard'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'discard'}),
                    ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'confirmDiscard'}, 'recording.review', {'id': 'synthetic-operation', 'action': 'confirmDiscard'}),
                ]
                for name, args, method, params in cases:
                    await call(name, args)
                    self.assertEqual(self.calls[-1]['method'], method)
                    self.assertEqual(self.calls[-1]['params'], params)
                preview = await session.call_tool('preview_editor', {'id': 'synthetic-operation', 'max_edge': 800})
                self.assertFalse(preview.is_error, preview)
                self.assertEqual(self.calls[-1]['method'], 'editor.preview')
                self.assertEqual(self.calls[-1]['params'], {'id': 'synthetic-operation', 'maxEdge': 800})
                image, envelope = preview.content
                self.assertEqual(image.type, 'image')
                self.assertEqual(image.mime_type, 'image/png')
                self.assertEqual(base64.b64decode(image.data), synthetic_png())
                metadata = json.loads(envelope.text)
                self.assertTrue(metadata['ok'])
                self.assertNotIn('png', metadata['result'])
                self.assertEqual(metadata['result']['sourceWidth'], 2)
                clip = await session.call_tool('preview_recording', {'id': 'synthetic-operation', 'time_us': 4000000, 'max_edge': 640})
                self.assertFalse(clip.is_error, clip)
                self.assertEqual(self.calls[-1]['method'], 'recording.preview')
                self.assertEqual(self.calls[-1]['params'], {'id': 'synthetic-operation', 'time': 4000000, 'maxEdge': 640})
                frame, clip_envelope = clip.content
                self.assertEqual(frame.mime_type, 'image/png')
                self.assertEqual(base64.b64decode(frame.data), synthetic_png())
                clip_metadata = json.loads(clip_envelope.text)
                self.assertNotIn('png', clip_metadata['result'])
                self.assertEqual(clip_metadata['result']['timeUs'], 4000000)
                for name, args in [('prepare_recording', {'target': {'kind': 'screen'}, 'format': 'mp4'}),
                                   ('prepare_recording', {'target': {'kind': 'region', 'rect': [1, 2, 3]}, 'format': 'mp4'}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mkv'}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'fps': 0}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'adaptive_fps': 'no'}),
                                   ('control_recording', {'id': 'synthetic-operation', 'command': 'explode'}),
                                   ('select_recording_region', {'id': 'synthetic-operation', 'rect': [1, 2, 0, 4]}),
                                   ('select_recording_region', {'id': 'synthetic-operation', 'rect': [1, 2, 3]}),
                                   ('edit_recording', {'id': 'synthetic-operation', 'revision': 0, 'commands': [{'op': 'resetTrim'}]}),
                                   ('edit_recording', {'id': 'synthetic-operation', 'revision': 1, 'commands': [{'op': 'crop'}]}),
                                   ('save_recording_copy', {'id': 'synthetic-operation', 'path': 'relative.mp4'}),
                                   ('upload_recording', {'id': 'synthetic-operation', 'revision': 0}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'upload'}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'seek'}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'speed', 'speed': 3}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'addMask'}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'placeMask'}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'addMask', 'kind': 'pixelate', 'rect': [0, 0, 4, 4]}),
                                   ('drive_recording_review', {'id': 'synthetic-operation', 'action': 'strength', 'strength': 99})]:
                    before = len(self.calls)
                    self.assertEqual((await call(name, args))['error']['code'], 'invalid_request', name)
                    self.assertEqual(len(self.calls), before, 'Invalid recording arguments must issue no IPC')
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
