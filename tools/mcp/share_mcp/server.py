"""The MCP layer validates arguments and forwards; the resident owns all effects."""
import argparse
import asyncio
import base64
import binascii
import json
import math
from typing import Any
from uuid import uuid4

from jsonschema import ValidationError, validate
from mcp.server.fastmcp import FastMCP
from mcp.server.fastmcp.utilities.types import Image
from mcp.types import ToolAnnotations


Json = dict[str, Any]


def failure(code: str, message: str, request_id: str = '') -> Json:
    return {'schemaVersion': 1, 'requestId': request_id, 'ok': False,
            'result': {}, 'error': {'code': code, 'message': message, 'details': {}}}


def invalid() -> Json:
    return failure('invalid_request', 'Invalid tool argument fields or values')


def identifier(value: str, maximum: int = 256) -> bool:
    return isinstance(value, str) and 0 < len(value) <= maximum


def safe_patch(value: Any) -> bool:
    # Never forward raw authentication fields into a process command line.
    # Complete configuration semantics remain the resident validator's responsibility.
    forbidden = {'accesskey', 'accesskeyid', 'secretkey', 'secretaccesskey',
                 'awsaccesskeyid', 'awssecretaccesskey', 'awssessiontoken',
                 'sessiontoken', 'password', 'authorization', 'token'}
    if isinstance(value, dict):
        return all(isinstance(key, str) and
                   key.lower().replace('_', '').replace('-', '') not in forbidden and
                   safe_patch(child) for key, child in value.items())
    if isinstance(value, list):
        return all(safe_patch(child) for child in value)
    return value is None or isinstance(value, (str, bool, int)) or (
        isinstance(value, float) and math.isfinite(value))


EDITOR_SOURCES = {'region', 'window', 'desktop', 'clipboard', 'file'}
EDITOR_OPS = {'add', 'update', 'remove', 'crop', 'resetCrop', 'undo', 'redo'}


def safe_commands(commands: Any) -> bool:
    # Shape semantics and bounds are validated by the editor; this keeps the
    # forwarded argument a bounded list of plain JSON command objects.
    return (isinstance(commands, list) and 0 < len(commands) <= 100 and
            all(isinstance(command, dict) and command.get('op') in EDITOR_OPS and
                safe_patch(command) for command in commands))


class StrictFastMCP(FastMCP):
    async def list_tools(self):
        tools = await super().list_tools()
        for tool in tools:
            tool.inputSchema['additionalProperties'] = False
        return tools

    async def call_tool(self, name: str, arguments: dict[str, Any]):
        # FastMCP deliberately disables lowlevel schema checks and coerces values.
        # Validate the public tool schema before its function argument conversion.
        for tool in await self.list_tools():
            if tool.name == name:
                try:
                    validate(arguments, tool.inputSchema)
                except ValidationError:
                    return invalid()
                break
        return await super().call_tool(name, arguments)


class CliAdapter:
    def __init__(self, cli: str = 'putgd-cli', socket: str | None = None,
                 timeout: float = 20):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('CLI timeout must be finite and positive')
        self.cli, self.socket, self.timeout = cli, socket, timeout

    async def call(self, method: str, params: Json) -> Json:
        request_id = str(uuid4())
        try:
            encoded = json.dumps(params, ensure_ascii=False, allow_nan=False, separators=(',', ':'))
        except (ValueError, TypeError):
            return invalid()
        args = [self.cli, '--request-id', request_id]
        if self.socket is not None:
            args += ['--socket', self.socket]
        args += [method, encoded]
        try:
            process = await asyncio.create_subprocess_exec(
                *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        except OSError:
            return failure('cli_unavailable', 'Cannot launch the configured production CLI', request_id)
        try:
            stdout, _ = await asyncio.wait_for(process.communicate(), self.timeout)
        except TimeoutError:
            if process.returncode is None:
                process.kill()
            await process.communicate()
            return failure('cli_timeout', 'CLI deadline expired; a sent resident action may still run', request_id)
        except asyncio.CancelledError:
            if process.returncode is None:
                process.kill()
            await process.communicate()
            raise
        try:
            reply = json.loads(stdout)
            if not isinstance(reply, dict) or set(reply) != {'schemaVersion', 'requestId', 'ok', 'result', 'error'}:
                raise ValueError()
            error = reply['error']
            if (type(reply['schemaVersion']) is not int or reply['schemaVersion'] != 1 or
                reply['requestId'] != request_id or type(reply['ok']) is not bool or
                not isinstance(reply['result'], dict) or not isinstance(error, dict) or
                set(error) != {'code', 'message', 'details'} or
                not isinstance(error['code'], str) or not isinstance(error['message'], str) or
                not isinstance(error['details'], dict) or reply['ok'] != (error['code'] == '') or
                process.returncode != (0 if reply['ok'] else 1)):
                raise ValueError()
        except (ValueError, KeyError, UnicodeDecodeError):
            return failure('invalid_response', 'CLI output does not match the versioned response contract', request_id)
        return reply


def create_server(adapter: CliAdapter) -> FastMCP:
    server = StrictFastMCP('put.gd', instructions=(
        'Inspect actual capabilities and configuration before acting. Actions use the configured '
        'local-save/public-upload destination and may write the clipboard. Preserve operation IDs. '
        'Deletion requires exact history IDs and acknowledgement of their original destination/effect. '
        'Edit & upload opens a visible editor; use edit_image/preview_editor, then submit_editor or cancel_editor. '
        'Use redact, never blur, for private information. '
        'Supply only non-secret configuration and credential references.'))
    read = ToolAnnotations(readOnlyHint=True, destructiveHint=False, openWorldHint=False)
    effect = ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=True)

    @server.tool(annotations=read)
    async def inspect() -> Json:
        """Read capabilities, redacted diagnostics and active non-secret configuration metadata only."""
        responses = {}
        for name, method in [('capabilities', 'capabilities'), ('diagnostics', 'diagnostics'),
                             ('configuration', 'settings.get')]:
            reply = await adapter.call(method, {})
            if not reply['ok']:
                return reply
            responses[name] = reply
        return {'schemaVersion': 1, 'requestId': responses['configuration']['requestId'],
                'ok': True, 'result': responses, 'error': {'code': '', 'message': '', 'details': {}}}

    @server.tool(annotations=effect)
    async def perform_action(action: str) -> Json:
        """Acquire region/window/desktop/clipboard, then configured local save or public upload; may copy result. `edit` opens the editor after a region selection instead; use open_editor for other sources."""
        if action not in {'region', 'window', 'desktop', 'clipboard', 'edit'}:
            return invalid()
        return await adapter.call('action.invoke', {'action': action})

    @server.tool(annotations=effect)
    async def open_editor(source: str = 'region', path: str | None = None) -> Json:
        """Start Edit & upload from region/window/desktop/clipboard, or an absolute PNG/JPEG file path. Returns the operation ID; the editor window opens and nothing uploads until submit_editor. Region/window selection may need the user."""
        if source not in EDITOR_SOURCES or (source == 'file') != (path is not None):
            return invalid()
        if path is not None and (not identifier(path, 4096) or not path.startswith('/')):
            return invalid()
        params: Json = {'source': source}
        if path is not None:
            params['path'] = path
        return await adapter.call('editor.open', params)

    @server.tool(annotations=read)
    async def get_editor(id: str) -> Json:
        """Read the open editor: image size, crop [x,y,w,h], shapes with ids/geometry/styles, undo/redo availability and phase (editing/finalizing)."""
        return await adapter.call('editor.inspect', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def edit_image(id: str, commands: list[Json]) -> Json:
        """Apply 1-100 edits atomically in the open editor, shown live in its window. Ops: add {shape}, update {shapeId, shape}, remove {shapeId}, crop {rect}, resetCrop, undo, redo. Shapes use source-image pixels: redact/blur/rectangle/highlight {kind, rect:[x,y,w,h]}, arrow {kind, from:[x,y], to:[x,y]}, text {kind, at:[x,y], text}; optional color '#RRGGBB' (not redact/blur), width 1-64, strength 2-64 (blur), fontPixels 8-200 (text). Redact removes covered pixels; blur is only a visual effect."""
        if not identifier(id) or not safe_commands(commands):
            return invalid()
        return await adapter.call('editor.apply', {'id': id, 'commands': commands})

    @server.tool(annotations=read, structured_output=False)
    async def preview_editor(id: str, max_edge: int = 1024) -> list[Any]:
        """Render the current sanitized result (the exact pixels Upload would send) scaled to at most max_edge (64-1600) pixels; returns the PNG image plus its metadata envelope."""
        if not identifier(id) or type(max_edge) is not int or not 64 <= max_edge <= 1600:
            return [invalid()]
        reply = await adapter.call('editor.preview', {'id': id, 'maxEdge': max_edge})
        if not reply['ok']:
            return [reply]
        result = dict(reply['result'])
        try:
            data = base64.b64decode(result.pop('png'), validate=True)
        except (KeyError, TypeError, binascii.Error):
            return [failure('invalid_response', 'Editor preview is not a PNG', reply['requestId'])]
        if not data.startswith(b'\x89PNG\r\n\x1a\n'):
            return [failure('invalid_response', 'Editor preview is not a PNG', reply['requestId'])]
        return [Image(data=data, format='png'), {**reply, 'result': result}]

    @server.tool(annotations=effect)
    async def save_editor_copy(id: str, path: str, overwrite: bool = False) -> Json:
        """Write the same sanitized export as Upload to an absolute .png path; refuses an existing file unless overwrite is true. Does not upload."""
        if not identifier(id) or not identifier(path, 4096) or not path.startswith('/') or type(overwrite) is not bool:
            return invalid()
        params: Json = {'id': id, 'path': path}
        if overwrite:
            params['overwrite'] = True
        return await adapter.call('editor.saveCopy', params)

    @server.tool(annotations=effect)
    async def submit_editor(id: str) -> Json:
        """Finalize the open edit and send only the sanitized result through the configured destination (may publish a public link and copy it). Inspect the operation afterwards."""
        return await adapter.call('editor.submit', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False))
    async def cancel_editor(id: str) -> Json:
        """Discard the open edit without confirmation; nothing is uploaded or archived."""
        return await adapter.call('editor.cancel', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def raise_editor(id: str) -> Json:
        """Bring the open editor window to the front."""
        return await adapter.call('editor.raise', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=read)
    async def get_operation(id: str, kind: str = 'operation') -> Json:
        """Inspect upload or separately persisted deletion operation; does not fetch remote content."""
        if not identifier(id) or kind not in {'operation', 'delete'}:
            return invalid()
        return await adapter.call(kind + '.inspect', {'id': id})

    @server.tool(annotations=effect)
    async def retry_operation(id: str, stage: str = 'upload') -> Json:
        """Explicitly retry upload/local_save/delete using saved identity; completed uploads are not repeated."""
        methods = {'upload': 'operation.retry', 'local_save': 'operation.retryLocalSave', 'delete': 'delete.retry'}
        if not identifier(id) or stage not in methods:
            return invalid()
        return await adapter.call(methods[stage], {'id': id})

    @server.tool(annotations=effect)
    async def copy_result(id: str) -> Json:
        """Write an existing operation artifact/receipt to clipboard without uploading again."""
        return await adapter.call('operation.copy', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=effect)
    async def cancel_operation(id: str, kind: str = 'operation') -> Json:
        """Cancel operation/delete work; already sent remote mutations cannot be rolled back."""
        if not identifier(id) or kind not in {'operation', 'delete'}:
            return invalid()
        return await adapter.call(kind + '.cancel', {'id': id})

    @server.tool(annotations=read)
    async def list_history(limit: int | None = None, offset: int | None = None,
                           search: str | None = None, category: str | None = None,
                           mime: str | None = None, profile_id: str | None = None,
                           upload_status: str | None = None, local_status: str | None = None,
                           remote_status: str | None = None, from_date: str | None = None,
                           to_date: str | None = None) -> Json:
        """Read offline history with pagination, literal name search, category/MIME/destination/outcome/date filters."""
        if (limit is not None and (type(limit) is not int or not 1 <= limit <= 200) or
            offset is not None and (type(offset) is not int or not 0 <= offset <= 2147483647)):
            return invalid()
        params = {k: v for k, v in {'limit': limit, 'offset': offset, 'search': search,
                  'category': category, 'mime': mime, 'profileId': profile_id,
                  'uploadStatus': upload_status, 'localStatus': local_status,
                  'remoteStatus': remote_status, 'from': from_date, 'to': to_date}.items() if v is not None}
        if any(not isinstance(v, str) or len(v) > 256 for k, v in params.items() if k not in {'limit', 'offset'}):
            return invalid()
        return await adapter.call('history.list', params)

    @server.tool(annotations=read)
    async def get_history_entry(id: str) -> Json:
        """Read stored original receipt, local-copy status and independent remote-deletion status."""
        return await adapter.call('history.detail', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=effect)
    async def copy_history_link(id: str) -> Json:
        """Write the stored direct URL to clipboard; no upload or target reconstruction."""
        return await adapter.call('history.copyLink', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=True))
    async def delete_history_uploads(ids: list[str], acknowledgement: str | None = None) -> Json:
        """Prepare original targets/effects without deletion when acknowledgement is omitted; exact acknowledgement confirms remote deletion and preserves local copies/history."""
        if not ids or len(ids) > 1000 or any(not identifier(id, 128) for id in ids):
            return invalid()
        if acknowledgement is not None and not identifier(acknowledgement, 64):
            return invalid()
        params: Json = {'ids': ids}
        if acknowledgement is not None:
            params['acknowledgement'] = acknowledgement
        return await adapter.call('delete.prepare' if acknowledgement is None else 'delete.confirm', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def reload_configuration() -> Json:
        """Validate/reload the active file; invalid edits preserve accepted settings; no cloud/capture activity."""
        return await adapter.call('settings.reload', {})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def patch_configuration(patch: Json, expected_revision: str) -> Json:
        """Patch non-secret settings with expected file revision; conflicts require reread/rebase. Null is explicit."""
        allowed = {'schemaVersion', 'actions', 'captureDefaults', 'profiles', 'archive', 'naming', 'limits', 'retention', 'preferences'}
        if (not isinstance(patch, dict) or set(patch) - allowed or not safe_patch(patch) or
            not isinstance(expected_revision, str) or len(expected_revision) > 256):
            return invalid()
        return await adapter.call('settings.patch', {'patch': patch, 'expectedRevision': expected_revision})

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description='Local stdio MCP adapter for the production put.gd CLI')
    parser.add_argument('--cli', default='putgd-cli')
    parser.add_argument('--socket')
    parser.add_argument('--timeout', type=float, default=20)
    args = parser.parse_args()
    try:
        adapter = CliAdapter(args.cli, args.socket, args.timeout)
    except ValueError as error:
        parser.error(str(error))
    create_server(adapter).run(transport='stdio')


if __name__ == '__main__':
    main()
