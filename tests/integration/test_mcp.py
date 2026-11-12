"""Official SDK / production CLI contract tests; synthetic local IPC only.

Run with PUTGD_CLI pointing at the built production binary. No desktop/cloud proof.
"""
import asyncio
import base64
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
            # Delayed capture (#1363): 3, 5 or 10 seconds, on the screenshots, repeat and edit only.
            ('perform_action', {'action': 'monitor', 'delay_seconds': 4}),
            ('perform_action', {'action': 'region', 'delay_seconds': -3}),
            ('perform_action', {'action': 'clipboard', 'delay_seconds': 3}),
            ('perform_action', {'action': 'edit', 'delay_seconds': 4}),
            ('perform_action', {'action': 'record', 'delay_seconds': 10}),
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
            ('import_custom_uploader', {'path': 'relative.sxcu', 'expected_revision': 'r'}),
            ('import_custom_uploader', {'path': '/synthetic/a.sxcu', 'expected_revision': 'r', 'credential_mode': 'file'}),
            ('import_custom_uploader', {'path': '/synthetic/a.sxcu', 'expected_revision': 'r', 'profile_id': ''}),
            ('import_custom_uploader', {'path': '/synthetic/a.sxcu', 'expected_revision': 'r', 'label': ''}),
            ('get_destination_credentials', {'profile_id': '', 'expected_revision': 'r'}),
            ('test_destination', {'profile_id': 'a' * 65, 'expected_revision': 'r'}),
            ('get_setup_job', {'id': ''}),
            ('drive_custom_uploader', {'action': 'reveal'}),
            ('drive_custom_uploader', {'action': 'import'}),
            ('drive_custom_uploader', {'action': 'import', 'path': 'relative.sxcu'}),
            ('drive_custom_uploader', {'action': 'inspect', 'path': '/synthetic/a.sxcu'}),
            ('drive_custom_uploader', {'action': 'store', 'profile_id': ''}),
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
            # The History refinement's controls: each parameter belongs to its own action.
            ('drive_history', {'action': 'goToPage'}),
            ('drive_history', {'action': 'goToPage', 'page': 0}),
            ('drive_history', {'action': 'nextPage', 'page': 2}),
            ('drive_history', {'action': 'setFilter', 'key': 'mime'}),
            ('drive_history', {'action': 'setFilter', 'key': 'shell', 'value': 'x'}),
            ('drive_history', {'action': 'removeFilter', 'key': 'mime', 'value': 'x'}),
            ('drive_history', {'action': 'showCategory', 'category': 'documents'}),
            ('drive_history', {'action': 'setView', 'view': 'table'}),
            ('drive_history', {'action': 'inspect', 'view': 'list'}),
            ('count_history', {'days': [{'from': '2026-11-10T00:00:00.000Z'}]}),
            ('count_history', {'days': [{'from': 'a', 'to': 'b', 'shell': 'c'}]}),
            ('count_history', {'days': [{'from': 'a', 'to': 'b'}] * 65}),
            ('count_history', {'search': 'x' * 257}),
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
            ('perform_action', {'action': 'region', 'delay_seconds': '3'}),
            ('perform_action', {'action': 'region', 'delay_seconds': 3.5}),
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
                expected = {'inspect', 'get_view_state', 'get_desktop_setup', 'recheck_desktop_setup', 'perform_action', 'get_operation', 'retry_operation',
                            'copy_result', 'cancel_operation', 'list_history', 'count_history', 'get_history_entry',
                            'make_history_preview', 'copy_history_link', 'unlock_history_link_credentials', 'copy_history_links', 'open_history_link', 'delete_history_uploads', 'reload_configuration',
                            'patch_configuration', 'open_editor', 'get_editor', 'edit_image',
                            'preview_editor', 'save_editor_copy', 'copy_editor_image', 'submit_editor', 'cancel_editor',
                            'raise_editor', 'get_recording_capabilities', 'list_recording_windows', 'list_recording_microphones', 'prepare_recording', 'control_recording',
                            'get_recording', 'select_recording_region', 'set_recording_overlay', 'set_recording_audio', 'edit_recording', 'preview_recording', 'drive_recording_review',
                            'save_recording_copy', 'upload_recording', 'reorder_destinations', 'move_destination',
                            'clear_logs', 'create_diagnostics_bundle', 'get_diagnostics_upload_target', 'show_diagnostics_package',
                            'settings_diagnostics', 'history_save_diagnostics', 'drive_history', 'get_update_status', 'check_for_updates',
                            'open_software_updates', 'install_update', 'restart_after_update', 'drive_file_dialog', 'editor_file_dialog',
                            'drive_editor', 'upload_files', 'import_custom_uploader', 'get_destination_credentials',
                            'test_destination', 'get_setup_job', 'drive_custom_uploader'}
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
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'blurStyle', 'style': 'mosaic'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'stepNumber', 'number': 0}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'stepNumber', 'number': 1000}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'spray'}),
                    ('copy_editor_image', {'id': ''}),
                ]:
                    rejected = await session.call_tool(name, args)
                    self.assertFalse(rejected.is_error, rejected)
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
                    ('unlock_history_link_credentials', {'id': 'synthetic-entry'}, 'history.unlockLinkCredentials', {'id': 'synthetic-entry'}),
                    ('patch_configuration', {'patch': {'profiles': [{'id': 'synthetic-s3', 'provider': 's3', 'settings': {'linkMode': 'presigned', 'linkValidityDays': 3}}]}, 'expected_revision': 'synthetic-revision'}, 'settings.patch', {'patch': {'profiles': [{'id': 'synthetic-s3', 'provider': 's3', 'settings': {'linkMode': 'presigned', 'linkValidityDays': 3}}]}, 'expectedRevision': 'synthetic-revision'}),
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
                    ('drive_history', {'action': 'toggleDetails'}, 'ui.history', {'action': 'toggleDetails'}),
                    ('drive_history', {'action': 'goToPage', 'page': 3}, 'ui.history', {'action': 'goToPage', 'page': 3}),
                    ('drive_history', {'action': 'nextPage'}, 'ui.history', {'action': 'nextPage'}),
                    ('drive_history', {'action': 'setFilter', 'key': 'profileId', 'value': 'synthetic-s3'}, 'ui.history', {'action': 'setFilter', 'key': 'profileId', 'value': 'synthetic-s3'}),
                    ('drive_history', {'action': 'removeFilter', 'key': 'includeCanceled'}, 'ui.history', {'action': 'removeFilter', 'key': 'includeCanceled'}),
                    ('drive_history', {'action': 'showCategory', 'category': 'text+other'}, 'ui.history', {'action': 'showCategory', 'category': 'text+other'}),
                    ('drive_history', {'action': 'setView', 'view': 'gallery'}, 'ui.history', {'action': 'setView', 'view': 'gallery'}),
                    ('drive_history', {'action': 'deleteSelected'}, 'ui.history', {'action': 'deleteSelected'}),
                    ('count_history', {'category': 'images', 'search': 'Screenshot', 'days': [{'from': '2026-11-09T00:00:00.000Z', 'to': '2026-11-09T23:59:59.999Z'}]},
                     'history.counts', {'category': 'images', 'search': 'Screenshot', 'days': [{'from': '2026-11-09T00:00:00.000Z', 'to': '2026-11-09T23:59:59.999Z'}]}),
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
                    ('import_custom_uploader', {'path': '/synthetic dir/service.sxcu', 'expected_revision': 'synthetic-revision'},
                     'settings.importUploader', {'path': '/synthetic dir/service.sxcu', 'expectedRevision': 'synthetic-revision', 'credentialMode': 'keychain'}),
                    ('import_custom_uploader', {'path': '/synthetic dir/service.sxcu', 'expected_revision': 'synthetic-revision', 'label': 'Lab files', 'profile_id': 'lab-files',
                                                'credential_mode': 'session', 'allow_insecure_http': True},
                     'settings.importUploader', {'path': '/synthetic dir/service.sxcu', 'expectedRevision': 'synthetic-revision', 'credentialMode': 'session',
                                                 'allowInsecureHttp': True, 'label': 'Lab files', 'profileId': 'lab-files'}),
                    ('get_destination_credentials', {'profile_id': 'lab-files', 'expected_revision': 'synthetic-revision'},
                     'setup.credentialHint', {'profileId': 'lab-files', 'expectedRevision': 'synthetic-revision'}),
                    ('test_destination', {'profile_id': 'lab-files', 'expected_revision': 'synthetic-revision'},
                     'setup.testDestination', {'profileId': 'lab-files', 'expectedRevision': 'synthetic-revision'}),
                    ('get_setup_job', {'id': 'synthetic-setup'}, 'setup.inspect', {'id': 'synthetic-setup'}),
                    ('drive_custom_uploader', {'action': 'inspect'}, 'ui.customUploader', {'action': 'inspect'}),
                    ('drive_custom_uploader', {'action': 'import', 'path': '/synthetic dir/service.sxcu', 'profile_id': 'lab-files'},
                     'ui.customUploader', {'action': 'import', 'path': '/synthetic dir/service.sxcu', 'profileId': 'lab-files'}),
                    ('drive_custom_uploader', {'action': 'store'}, 'ui.customUploader', {'action': 'store'}),
                    ('get_desktop_setup', {}, 'desktop.setup', {}),
                    ('recheck_desktop_setup', {}, 'desktop.setup.recheck', {}),
                    ('perform_action', {'action': 'edit'}, 'action.invoke', {'action': 'edit'}),
                    # Capture monitor, Repeat last region and the capture delay (#1363).
                    ('perform_action', {'action': 'monitor'}, 'action.invoke', {'action': 'monitor'}),
                    ('perform_action', {'action': 'repeat'}, 'action.invoke', {'action': 'repeat'}),
                    ('perform_action', {'action': 'region', 'delay_seconds': 10}, 'action.invoke', {'action': 'region', 'delaySeconds': 10}),
                    ('perform_action', {'action': 'window', 'delay_seconds': 3}, 'action.invoke', {'action': 'window', 'delaySeconds': 3}),
                    ('perform_action', {'action': 'monitor', 'delay_seconds': 5}, 'action.invoke', {'action': 'monitor', 'delaySeconds': 5}),
                    ('perform_action', {'action': 'repeat', 'delay_seconds': 3}, 'action.invoke', {'action': 'repeat', 'delaySeconds': 3}),
                    ('perform_action', {'action': 'edit', 'delay_seconds': 5}, 'action.invoke', {'action': 'edit', 'delaySeconds': 5}),
                    ('perform_action', {'action': 'desktop', 'delay_seconds': 0}, 'action.invoke', {'action': 'desktop', 'delaySeconds': 0}),
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
                    ('copy_editor_image', {'id': 'synthetic-operation'}, 'editor.copy', {'id': 'synthetic-operation'}),
                    ('edit_image', {'id': 'synthetic-operation', 'commands': [
                        {'op': 'add', 'shape': {'kind': 'step', 'at': [40, 40], 'number': 3, 'size': 48, 'color': '#3b82f6'}},
                        {'op': 'add', 'shape': {'kind': 'pen', 'points': [[1, 2], [5, 9], [12, 4]], 'width': 8}},
                        {'op': 'add', 'shape': {'kind': 'ellipse', 'rect': [10, 10, 60, 30]}},
                        {'op': 'add', 'shape': {'kind': 'line', 'from': [0, 0], 'to': [40, 40], 'width': 2}},
                        {'op': 'add', 'shape': {'kind': 'blur', 'rect': [1, 1, 20, 20], 'style': 'pixelate', 'strength': 8}}]},
                     'editor.apply', {'id': 'synthetic-operation', 'commands': [
                        {'op': 'add', 'shape': {'kind': 'step', 'at': [40, 40], 'number': 3, 'size': 48, 'color': '#3b82f6'}},
                        {'op': 'add', 'shape': {'kind': 'pen', 'points': [[1, 2], [5, 9], [12, 4]], 'width': 8}},
                        {'op': 'add', 'shape': {'kind': 'ellipse', 'rect': [10, 10, 60, 30]}},
                        {'op': 'add', 'shape': {'kind': 'line', 'from': [0, 0], 'to': [40, 40], 'width': 2}},
                        {'op': 'add', 'shape': {'kind': 'blur', 'rect': [1, 1, 20, 20], 'style': 'pixelate', 'strength': 8}}]}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'step'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'tool', 'tool': 'step'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'blurStyle', 'style': 'pixelate'}, 'editor.view', {'id': 'synthetic-operation', 'action': 'blurStyle', 'style': 'pixelate'}),
                    ('drive_editor', {'id': 'synthetic-operation', 'action': 'stepNumber', 'number': 12}, 'editor.view', {'id': 'synthetic-operation', 'action': 'stepNumber', 'number': 12}),
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
                    ('list_recording_windows', {}, 'recording.windows', {}),
                    # #1364: the inputs a recording can narrate from, the microphone and click highlights.
                    ('list_recording_microphones', {}, 'recording.microphones', {}),
                    ('list_recording_microphones', {'device': 'alsa_input.usb-Synthetic_Mic-00.mono-fallback'}, 'recording.microphones', {'device': 'alsa_input.usb-Synthetic_Mic-00.mono-fallback'}),
                    ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'microphone': True},
                     'recording.prepare', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'audio': {'microphone': True}}),
                    ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'webm', 'system_audio': True, 'microphone': True, 'microphone_device': 'putgd_lab_mic', 'click_highlights': True},
                     'recording.prepare', {'target': {'kind': 'desktop'}, 'format': 'webm', 'audio': {'system': True, 'microphone': True, 'microphoneDevice': 'putgd_lab_mic'}, 'clickHighlights': True}),
                    ('prepare_recording', {'target': {'kind': 'window', 'identity': '{2baa3a00-9994-4798-88f3-e2b2d9b47ea0}'}, 'format': 'mp4'},
                     'recording.prepare', {'target': {'kind': 'window', 'identity': '{2baa3a00-9994-4798-88f3-e2b2d9b47ea0}'}, 'format': 'mp4'}),
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
                    # #1364: the ready bar's audio toggles; an omitted one is not sent.
                    ('set_recording_audio', {'id': 'synthetic-operation', 'microphone': True}, 'recording.audio', {'id': 'synthetic-operation', 'microphone': True}),
                    ('set_recording_audio', {'id': 'synthetic-operation', 'microphone': False, 'system': True}, 'recording.audio', {'id': 'synthetic-operation', 'microphone': False, 'system': True}),
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
                                   # #1364: a device without the microphone, an unusable device name, a non-boolean toggle.
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'microphone_device': 'putgd_lab_mic'}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'microphone': True, 'microphone_device': 'bad\nname'}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'microphone': True, 'microphone_device': ''}),
                                   ('prepare_recording', {'target': {'kind': 'desktop'}, 'format': 'mp4', 'click_highlights': 'yes'}),
                                   ('list_recording_microphones', {'device': 'x' * 257}),
                                   ('set_recording_audio', {'id': 'synthetic-operation'}),
                                   ('set_recording_audio', {'id': 'synthetic-operation', 'microphone': 'yes'}),
                                   ('set_recording_audio', {'id': 'synthetic-operation', 'system': 1}),
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
