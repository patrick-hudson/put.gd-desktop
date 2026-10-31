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
# The editor window's view and selection controls, one per drive_editor call.
EDITOR_VIEW_ACTIONS = {'inspect', 'tool', 'select', 'selectNext', 'selectPrevious', 'selectAffectedText', 'removeMark', 'place', 'geometry', 'applyCrop',
                       'cancelCrop', 'zoom', 'fit', 'actualSize', 'pan', 'showSavedCopy', 'copySavedPath', 'dismissSavedCopy', 'discard',
                       'keepEditing', 'confirmDiscard', 'keyboardHelp', 'geometryDetails', 'saveDetails', 'propertiesWidth'}
EDITOR_TOOLS = {'select', 'redact', 'blur', 'crop', 'arrow', 'rectangle', 'highlight', 'text'}
RECORDING_TARGETS = {'monitor', 'desktop', 'region', 'window'}
RECORDING_FORMATS = {'mp4', 'webm', 'gif', 'webp'}
RECORDING_CODECS = {'h264', 'h265', 'vp9'}
RECORDING_CONTROLS = {'start', 'pause', 'resume', 'stop', 'cancel', 'raise'}
RECORDING_EDIT_OPS = {'setTrim', 'resetTrim', 'addMask', 'updateMask', 'removeMask'}
# The review window's controls (#283), one per drive_recording_review call.
REVIEW_ACTIONS = {'inspect', 'seek', 'play', 'pause', 'speed', 'setIn', 'setOut', 'resetTrim', 'addMask', 'showMaskOptions', 'closeMaskOptions', 'nextMaskLanes', 'previousMaskLanes', 'selectMask',
                  'setMaskIn', 'setMaskOut', 'strength', 'removeMask', 'format', 'saveCopyDialog', 'closeFileDialog',
                  'placeMask', 'useSolidCover', 'trimToLimit', 'dismissStatus', 'dismissBlurTip', 'showAdvice', 'showFormatTips', 'showInFolder', 'copyPath', 'dismissSavedCopy', 'retryExport', 'retryPreview', 'discard', 'keepReviewing',
                  'confirmDiscard'}
REVIEW_SPEEDS = {0.5, 1, 2, 4}
# The put.gd window's file choosers (#259) and the actions on each.
FILE_DIALOGS = {'editImage', 'historySaveAs', 'outputFolder', 'diagnosticsFolder'}
FILE_DIALOG_ACTIONS = {'open', 'inspect', 'cancel'}
HISTORY_ACTIONS = {'inspect', 'copyDeletionDetails', 'retryLoading', 'clearFilters', 'select', 'clearSelection', 'openDetails', 'closeDetails', 'expandPreview', 'closePreview', 'retryPreview', 'showSavedCopy', 'copySavedPath', 'dismissSavedCopy', 'cancelAll', 'confirmCancelAll', 'keepUploading'}
OUTPUT_FOLDER_FIELDS = {'root', 'images', 'videos', 'text', 'other'}
# A backstop above the CLI's own bounded wait, whose longest case is 3 s to connect,
# 3 s to send and 45 s for recording.stop (LocalServer::replyDeadlineMs).
CLI_TIMEOUT = 60


def safe_recording_target(target: Any) -> bool:
    # Geometry/identity semantics are validated by the resident; this keeps the
    # forwarded target a small typed object.
    if not isinstance(target, dict) or target.get('kind') not in RECORDING_TARGETS:
        return False
    if not set(target) <= {'kind', 'identity', 'rect'}:
        return False
    if 'identity' in target and not identifier(target['identity']):
        return False
    if 'rect' in target and not (isinstance(target['rect'], list) and len(target['rect']) == 4 and
                                 all(type(value) is int for value in target['rect'])):
        return False
    return True


def safe_recording_commands(commands: Any) -> bool:
    return (isinstance(commands, list) and 0 < len(commands) <= 100 and
            all(isinstance(command, dict) and command.get('op') in RECORDING_EDIT_OPS and
                safe_patch(command) for command in commands))


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
                 timeout: float = CLI_TIMEOUT):
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError('CLI timeout must be finite and positive')
        self.cli, self.socket, self.timeout = cli, socket, timeout

    async def call(self, method: str, params: Json, timeout: float | None = None) -> Json:
        # A longer deadline for methods whose resident reply waits on the network.
        timeout = max(self.timeout, timeout or 0)
        request_id = str(uuid4())
        try:
            encoded = json.dumps(params, ensure_ascii=False, allow_nan=False, separators=(',', ':'))
        except (ValueError, TypeError):
            return invalid()
        args = [self.cli, '--request-id', request_id]
        if self.socket is not None:
            args += ['--socket', self.socket]
        args += ['--params-stdin', method]
        try:
            process = await asyncio.create_subprocess_exec(
                *args, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        except OSError:
            return failure('cli_unavailable', 'Cannot launch the configured production CLI', request_id)
        try:
            stdout, _ = await asyncio.wait_for(process.communicate(encoded.encode()), timeout)
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
        'Capture region & edit (the edit action) opens a visible editor; use edit_image/preview_editor, then submit_editor or cancel_editor. '
        'Use redact, never blur, for private information. '
        'Screen recording: prepare_recording, control_recording start/pause/resume/stop, then review with edit_recording/preview_recording, or work the review window itself with drive_recording_review; only upload_recording or retryExport after a failed Upload publishes. '
        'save_recording_copy and upload_recording return once the export starts; poll get_recording until state leaves exporting. '
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

    @server.tool(annotations=read)
    async def get_view_state() -> Json:
        """Read the Settings window's last checkpointed capture routing: for the default output and each action, the destination and sharing it shows, the resolved sharing mode, whether it inherits the default and whether each control is enabled. Without an open or retained Settings view only the interface state is returned."""
        return await adapter.call('ui.viewState', {})

    @server.tool(annotations=effect)
    async def perform_action(action: str) -> Json:
        """Acquire region/window/desktop/clipboard, then configured local save or public upload; may copy result. `edit` opens the editor after a region selection instead; use open_editor for other sources. `record` starts a screen recording from a dragged region (or raises the active one); drive it with the recording tools."""
        if action not in {'region', 'window', 'desktop', 'clipboard', 'edit', 'record'}:
            return invalid()
        return await adapter.call('action.invoke', {'action': action})

    @server.tool(annotations=effect)
    async def open_editor(source: str = 'region', path: str | None = None) -> Json:
        """Start Capture region & edit (the edit action) from region/window/desktop/clipboard, or an absolute PNG/JPEG file path. Returns the operation ID; the editor window opens and nothing uploads until submit_editor. Region/window selection may need the user."""
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
        """Read the open editor: image size, crop [x,y,w,h], shapes with ids/geometry/styles, undo/redo availability, phase (editing/finalizing), preparation (null, or the last Upload that could not be prepared: status failed, code edit_export_failed or input_excessive, reason; editing resumed) and the window's view (as drive_editor inspect returns it)."""
        return await adapter.call('editor.inspect', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def edit_image(id: str, commands: list[Json]) -> Json:
        """Apply 1-100 edits atomically in the open editor, shown live in its window. Ops: add {shape}, update {shapeId, shape}, remove {shapeId}, crop {rect}, resetCrop, undo, redo. Shapes use source-image pixels: redact/blur/rectangle/highlight {kind, rect:[x,y,w,h]}, arrow {kind, from:[x,y], to:[x,y]}, text {kind, at:[x,y], text}; optional color '#RRGGBB' (not redact/blur), width 1-64, strength 2-64 (blur), fontPixels 8-200 and font family name or '' for the default (text). A redaction removes the pixels it covers; blur is only a visual effect."""
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
    async def editor_file_dialog(id: str, action: str) -> Json:
        """Open (action open) the editor's Save a copy… chooser as the button does, read its state (inspect) or close it as a cancel (cancel). It is the desktop's own file chooser through the XDG portal, or the themed fallback dialog; the state shows open, backend portal|fallback, fallbackReason and the last result. A file picked there gets the sanitized PNG; save_editor_copy saves to a path without a dialog."""
        if not identifier(id) or action not in FILE_DIALOG_ACTIONS:
            return invalid()
        return await adapter.call('editor.fileDialog', {'id': id, 'action': action})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def drive_editor(id: str, action: str, tool: str | None = None, shape_id: int | None = None,
                           rect: list[int] | None = None, percent: float | None = None,
                           delta: list[int] | None = None, pixels: int | None = None) -> Json:
        """Work the open editor window's view and selection controls as a user does, one action per call; each reply is the window's view: tool, selectedShapeId, geometry [x,y,w,h] of the selection or pending crop (an arrow's tail at x,y and head at x+w,y+h), cropPending, the accessible selection summary, zoomPercent, fitted, visibleRect (image pixels shown), canvasSize, geometryDetails {open, visible}, propertiesWidth (logical pixels), editingText, busy, the tool style, the footer message, lastSave (saved/failed, stage chooser/write, path, reason; a saved copy's current is false once the draft changed after it), lastShowInFolder, lastCopiedPath, saveFeedbackVisible, saveDetailsOpen, cancelLabel (Cancel, or Discard… once there are edits), destination, preparationFailure and discardConfirmation {open, text}. Actions: inspect, tool {tool select|redact|blur|crop|arrow|rectangle|highlight|text}, select {shape_id, or none to clear}, selectNext / selectPrevious (] and [; Tab leaves the canvas with the selection kept), selectAffectedText (cycle annotation text clipped by the saved image or pending crop, preserving that crop), removeMark (Remove mark / Delete, one undoable step on the selection), place (the current drawing tool's mark centred at a quarter of the image, Insert), geometry {rect [x,y,w,h] in image pixels for the selection or the pending crop; one undo step}, applyCrop / cancelCrop, zoom {percent 2-3200}, fit, actualSize, pan {delta [dx,dy] canvas pixels}, propertiesWidth {pixels 200-10000, capped at one third of the window}, showSavedCopy (Show in folder for the last saved copy), geometryDetails (toggle Position and size for annotation marks; crop fields stay visible), keyboardHelp (toggle the named keyboard help disclosure), copySavedPath (Copy path: its full path to the clipboard), saveDetails (toggle the save failure's full path and reason), dismissSavedCopy (dismiss the footer feedback, keeping its saved path), discard (the Cancel / Discard… button: closes at once without edits, else opens the discard confirmation), keepEditing (answers that confirmation, or dismisses a preparation failure) and confirmDiscard (discards; nothing uploads). Other actions wait while the confirmation is open. Other shape edits go through edit_image; nothing uploads; cancel_editor still discards without asking."""
        if not identifier(id) or action not in EDITOR_VIEW_ACTIONS:
            return invalid()
        params: Json = {'id': id, 'action': action}
        if tool is not None:
            if tool not in EDITOR_TOOLS:
                return invalid()
            params['tool'] = tool
        if shape_id is not None:
            if type(shape_id) is not int or shape_id < 1:
                return invalid()
            params['shapeId'] = shape_id
        elif action == 'select':
            params['shapeId'] = None
        if rect is not None:
            if not (isinstance(rect, list) and len(rect) == 4 and all(type(value) is int and abs(value) <= 1000000 for value in rect)):
                return invalid()
            params['rect'] = rect
        if percent is not None:
            if type(percent) not in (int, float) or not math.isfinite(percent) or not 2 <= percent <= 3200:
                return invalid()
            params['percent'] = percent
        if delta is not None:
            if not (isinstance(delta, list) and len(delta) == 2 and all(type(value) is int and abs(value) <= 100000 for value in delta)):
                return invalid()
            params['delta'] = delta
        if pixels is not None:
            if type(pixels) is not int or not 200 <= pixels <= 10000:
                return invalid()
            params['pixels'] = pixels
        return await adapter.call('editor.view', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def raise_editor(id: str) -> Json:
        """Bring the open editor window to the front."""
        return await adapter.call('editor.raise', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=read)
    async def get_recording_capabilities() -> Json:
        """Read screen-recording support: availability, target kinds, formats/codecs, audio sources, the active recording ID, limits and the microsecond time unit. Starts nothing and shows no prompt."""
        return await adapter.call('recording.capabilities', {})

    @server.tool(annotations=effect)
    async def prepare_recording(target: Json, format: str, codec: str | None = None, fps: int | None = None,
                                system_audio: bool = False, max_edge: int | None = None, adaptive_fps: bool | None = None) -> Json:
        """Create one recording session in the ready state: target {kind: monitor|desktop|region|window, identity?, rect?: [x,y,w,h]}, format mp4|webm|gif|webp, optional codec (mp4: h264|h265, webm: vp9), fps 1-60 (omitted: preferences.recordingFrameRate, 30 by default), adaptive_fps (default true: the recorder lowers the rate, 60 to 30 to 15, when the machine can't keep up; get_recording progress then shows frameRate and frameRateReduced {from, to, reason, atUs}), system_audio (MP4/WebM only; the computer's playback, never a microphone) and max_edge to downscale the output. Shows the ready bounds/controls and may ask the user for OS consent; nothing is captured until control_recording start. Returns recordingId = operationId; poll get_recording until state is ready. Refused with disk_space_insufficient (details freeBytes, reserveBytes, requiredBytes, shortfallBytes) when the disk can't hold a recording that lasts; nothing is accepted then."""
        if not safe_recording_target(target) or format not in RECORDING_FORMATS:
            return invalid()
        if codec is not None and codec not in RECORDING_CODECS:
            return invalid()
        if fps is not None and (type(fps) is not int or not 1 <= fps <= 60):
            return invalid()
        if adaptive_fps is not None and type(adaptive_fps) is not bool:
            return invalid()
        if type(system_audio) is not bool or (max_edge is not None and (type(max_edge) is not int or not 16 <= max_edge <= 8192)):
            return invalid()
        params: Json = {'target': target, 'format': format}
        if codec is not None:
            params['codec'] = codec
        if fps is not None:
            params['fps'] = fps
        if adaptive_fps is not None:
            params['adaptiveFps'] = adaptive_fps
        if system_audio:
            params['audio'] = {'system': True}
        if max_edge is not None:
            params['maxEdge'] = max_edge
        return await adapter.call('recording.prepare', params)

    @server.tool(annotations=effect)
    async def control_recording(id: str, command: str) -> Json:
        """start (ready), pause (recording), resume (paused), stop (recording/paused: always opens private review, uploads nothing), cancel (discards without upload) or raise the overlay. Acknowledged only once the helper reached the state; a duplicate in-flight command returns the current state."""
        if not identifier(id) or command not in RECORDING_CONTROLS:
            return invalid()
        return await adapter.call('recording.' + command, {'id': id})

    @server.tool(annotations=read)
    async def get_recording(id: str) -> Json:
        """Read the session: state, pending command, whether a region selector is open (selecting), target, config, canvas, retained durationUs, trim/masks with their revision, helper progress (exportFraction while exporting), where the control bar went (overlayExcluded, overlayPlacement), lastExport (the latest copy/upload export: status exporting|saved|failed, revision, path, bytes, error), stopReason (budget when the recording stopped itself) with stopDetails (reason disk_space|max_file_bytes, message, hint, budgetBytes, maxFileBytes, durationUs, and freeBytes/reserveBytes/availableBytes for the disk) and any error. An ended session is recording_unavailable; get_operation has its outcome."""
        return await adapter.call('recording.inspect', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=effect)
    async def select_recording_region(id: str, rect: list[int]) -> Json:
        """Complete the open region selector of a preparing recording (get_recording shows selecting: true; the tray, shortcut and dashboard open one) with rect [x, y, width, height], exactly as a drag would: X11 root pixels, logical desktop pixels on Wayland. The session then reaches ready; nothing is captured until control_recording start. Cancel it with control_recording cancel."""
        if not identifier(id) or not (isinstance(rect, list) and len(rect) == 4 and all(type(value) is int for value in rect)) or rect[2] < 1 or rect[3] < 1:
            return invalid()
        return await adapter.call('recording.select', {'id': id, 'rect': rect})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def set_recording_overlay(id: str, visible: bool) -> Json:
        """Hide or show the floating control overlay while ready/recording/paused; the recording indicator stays visible."""
        if not identifier(id) or type(visible) is not bool:
            return invalid()
        return await adapter.call('recording.overlay', {'id': id, 'visible': visible})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def edit_recording(id: str, revision: int, commands: list[Json]) -> Json:
        """Apply 1-100 review edits atomically at the current edit revision (from get_recording). Times are microseconds on the recorded clip (pauses removed); rects are output-canvas pixels. Ops: setTrim {start, end|null}, resetTrim, addMask {mask}, updateMask {maskId, mask}, removeMask {maskId}; mask = {kind: blur|solid, rect: [x,y,w,h], start, end, strength?}. Masks keep their source times when the trim changes. A stale revision is refused."""
        if not identifier(id) or type(revision) is not int or revision < 1 or not safe_recording_commands(commands):
            return invalid()
        return await adapter.call('recording.edit', {'id': id, 'revision': revision, 'commands': commands})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=True))
    async def drive_recording_review(id: str, action: str, time_us: int | None = None, speed: float | None = None,
                                     kind: str | None = None, rect: list[int] | None = None, mask_id: str | None = None,
                                     strength: int | None = None, format: str | None = None, codec: str | None = None) -> Json:
        """Work the open Review recording window's controls as a user does, one action per call: inspect (playhead, playing, speed, trim, masks with their timeline bars, selectedMaskId, which contextual mask controls show, status, size, the unobscured video's rect and share of the window), seek {time_us}, play, pause, speed {0.5|1|2|4}, setIn / setOut (trim start/end at the playhead, keys I/O), resetTrim, addMask {kind blur|solid, optional rect [x,y,w,h] canvas pixels, else centred at half the frame's width and height as Enter places it; starts at the playhead, runs to the trim end}, showMaskOptions / closeMaskOptions (opens or closes blur radius and Delete mask), nextMaskLanes / previousMaskLanes (scrolls two lanes without changing selection), selectMask {mask_id, or none to clear}, placeMask {rect; the selected mask's rectangle, as a drag or the frame's arrow keys set it}, setMaskIn / setMaskOut (the selected mask's start/end at the playhead, Shift+I/O), strength {2-64, selected blur}, removeMask (selected, Delete), useSolidCover (the visible Use Redact action: turns the selected blur, or an armed Add blur, into a redaction; serialized kind stays solid), format {format, codec}, trimToLimit (keeps the first part a GIF/WebP length limit allows), dismissStatus (clears an error or result in the status row), dismissBlurTip (Got it on the blur privacy guidance, hidden for the rest of this review; inspect's statusRow lists each line's text, tone and its actions as the drive calls they make), retryExport (Try again: repeats the last failed Save a copy target or explicit Upload; an Upload retry publishes), retryPreview (decodes the playhead frame again; inspect's preview shows error, stale and frameTimeUs), discard (asks as Discard… and the close button do; inspect's discardConfirmation), keepReviewing / confirmDiscard (answer it; control_recording cancel discards without asking), saveCopyDialog (opens Save copy… as the button does: the desktop's file chooser through the XDG portal, or the themed fallback dialog; inspect's fileDialog shows open, backend portal|fallback, fallbackReason and the last result, and a file picked there is saved like recording.saveCopy) and closeFileDialog (closes it as a cancel); showAdvice opens error details when an error is present; showFormatTips opens compatibility and audio guidance; showInFolder and copyPath open the saved copy in its folder or copy its absolute path; dismissSavedCopy hides that result. Inspect includes savedCopy (path, revision, current and folder-open result), formatAdvice (the compact format tips control) and stable mask names. Edits go through the resident like the window's own; the reply is the window state plus the resident's edit after it. Only retryExport can repeat a previously requested Upload; use upload_recording for a new upload."""
        if not identifier(id) or action not in REVIEW_ACTIONS:
            return invalid()
        params: Json = {'id': id, 'action': action}
        if time_us is not None:
            if type(time_us) is not int or time_us < 0:
                return invalid()
            params['time'] = time_us
        if speed is not None:
            if type(speed) not in (int, float) or speed not in REVIEW_SPEEDS:
                return invalid()
            params['speed'] = speed
        if kind is not None:
            if kind not in {'blur', 'solid'}:
                return invalid()
            params['kind'] = kind
        if rect is not None:
            if not (isinstance(rect, list) and len(rect) == 4 and all(type(value) is int and value >= 0 for value in rect)):
                return invalid()
            params['rect'] = rect
        if mask_id is not None:
            if not identifier(mask_id):
                return invalid()
            params['maskId'] = mask_id
        elif action == 'selectMask':
            params['maskId'] = None
        if strength is not None:
            if type(strength) is not int or not 2 <= strength <= 64:
                return invalid()
            params['strength'] = strength
        if format is not None:
            if format not in RECORDING_FORMATS or (codec is not None and (not isinstance(codec, str) or len(codec) > 8)):
                return invalid()
            params['format'] = format
            params['codec'] = codec or ''
        required = {'seek': 'time', 'speed': 'speed', 'addMask': 'kind', 'placeMask': 'rect', 'strength': 'strength', 'format': 'format'}
        if action in required and required[action] not in params:
            return invalid()
        return await adapter.call('recording.review', params)

    @server.tool(annotations=read, structured_output=False)
    async def preview_recording(id: str, time_us: int, max_edge: int = 1024) -> list[Any]:
        """Render one frame at a clip time (microseconds) with the current trim and masks applied, scaled to at most max_edge (64-1600); returns the PNG plus metadata (retained, outputTimeUs, revision)."""
        if not identifier(id) or type(time_us) is not int or time_us < 0 or type(max_edge) is not int or not 64 <= max_edge <= 1600:
            return [invalid()]
        reply = await adapter.call('recording.preview', {'id': id, 'time': time_us, 'maxEdge': max_edge})
        if not reply['ok']:
            return [reply]
        result = dict(reply['result'])
        try:
            data = base64.b64decode(result.pop('png'), validate=True)
        except (KeyError, TypeError, binascii.Error):
            return [failure('invalid_response', 'Recording preview is not a PNG', reply['requestId'])]
        if not data.startswith(b'\x89PNG\r\n\x1a\n'):
            return [failure('invalid_response', 'Recording preview is not a PNG', reply['requestId'])]
        return [Image(data=data, format='png'), {**reply, 'result': result}]

    @server.tool(annotations=effect)
    async def save_recording_copy(id: str, path: str, overwrite: bool = False) -> Json:
        """Export the reviewed result (trim and masks applied) to an absolute path with the format's extension, without uploading; refuses an existing file unless overwrite is true. Returns once the export starts (state exporting); poll get_recording until state is reviewing, then lastExport.status is saved (path, bytes) or failed (error). Exports can take minutes."""
        if not identifier(id) or not identifier(path, 4096) or not path.startswith('/') or type(overwrite) is not bool:
            return invalid()
        params: Json = {'id': id, 'path': path}
        if overwrite:
            params['overwrite'] = True
        return await adapter.call('recording.saveCopy', params)

    @server.tool(annotations=effect)
    async def upload_recording(id: str, revision: int) -> Json:
        """The only action that publishes: export the result at the reviewed revision, validate it and send it through the configured destination (may publish a public link and copy it). Returns once the export starts (state exporting, with operationId); exports can take minutes. If get_recording shows reviewing again, lastExport.error says why and nothing was uploaded; once the session is handed to the upload it is recording_unavailable and get_operation on operationId has the outcome. Never falls back to the unedited clip."""
        if not identifier(id) or type(revision) is not int or revision < 1:
            return invalid()
        return await adapter.call('recording.upload', {'id': id, 'revision': revision})

    @server.tool(annotations=read)
    async def get_operation(id: str, kind: str = 'operation') -> Json:
        """Inspect upload, separately persisted deletion or sharing (kind share: a shareOperationId; its status and the confirmed link expiresAt) operation; does not fetch remote content."""
        if not identifier(id) or kind not in {'operation', 'delete', 'share'}:
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
    async def cancel_operation(id: str, kind: str = 'operation', item_id: str | None = None) -> Json:
        """Cancel operation/delete work; already sent remote mutations cannot be rolled back. With item_id (an operation item's artifact.id), cancel only that file of an active upload batch; the other files continue and Retry uploads the cancelled one."""
        if not identifier(id) or kind not in {'operation', 'delete'} or (item_id is not None and (kind != 'operation' or not identifier(item_id))):
            return invalid()
        return await adapter.call(kind + '.cancel', {'id': id} if item_id is None else {'id': id, 'itemId': item_id})

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

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def make_history_preview(id: str, retry: bool | None = None) -> Json:
        """Make a missing image/video preview (never made, failed or cleaned up) from the retained file or verified local copy; returns previewPath and state ready/made, or why there is none. retry=True clears an earlier refusal and regenerates an invalid app-owned cache entry."""
        if not identifier(id) or (retry is not None and type(retry) is not bool):
            return invalid()
        params: Json = {'id': id}
        if retry is not None:
            params['retry'] = retry
        return await adapter.call('history.preview', params)

    @server.tool(annotations=effect)
    async def copy_history_link(id: str) -> Json:
        """Write the stored direct URL to clipboard; no upload or target reconstruction."""
        return await adapter.call('history.copyLink', {'id': id}) if identifier(id) else invalid()

    @server.tool(annotations=effect)
    async def copy_history_links(ids: list[str]) -> Json:
        """Write several stored direct URLs to the clipboard, one per line in the order given, as History's Copy links does for a selection; link_unavailable (with historyId) copies nothing. No upload or target reconstruction."""
        if not ids or len(ids) > 1000 or len(set(ids)) != len(ids) or any(not identifier(id, 128) for id in ids):
            return invalid()
        return await adapter.call('history.copyLinks', {'ids': ids})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=True))
    async def open_history_link(id: str) -> Json:
        """Open the stored direct URL in the desktop's default browser, as clicking the link-ready notification does: the XDG portal's OpenURI, else gio open with the host's environment. Returns route (portal or launcher), opened and fallbackFrom; link_not_opened when nothing could open it."""
        return await adapter.call('history.openLink', {'id': id}) if identifier(id) else invalid()

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
    async def create_diagnostics_bundle(operation_id: str | None = None, since: str | None = None,
                                        include_names: bool = False, output: str | None = None,
                                        preview: bool = False, notify: bool = False,
                                        upload: bool = False, profile_id: str | None = None,
                                        copy_link: bool = False) -> Json:
        """Save a diagnostics package (.zip) locally for a maintainer: manifest with SHA-256s, environment, diagnostics, redacted configuration, failed/partial/interrupted operations with HTTP/provider codes and S3 request IDs, coredumpctl stack traces where available, and log records, at most 20 MiB. operation_id packages one operation and the log around it; otherwise since (1m-365d, default 7d) picks failures and logs. File names, object keys and URLs are hashed unless include_names; credentials, tokens, signed URLs, file bytes, clipboard and the raw history database are never included. output is an absolute .zip path (never overwritten) or folder; the default <archive>/diagnostics keeps only the newest 10 packages for 30 days. The local copy is always saved first. preview returns the files, redaction, operations, log record count and folder without writing or uploading anything; notify makes the resident show a "diagnostics saved" notification with Show in folder. upload then sends the saved package to a working destination (profile_id, default the capture destination; skipped with uploadError when its last upload failed with a credential or configuration error or Test connection fails) as putgd-diagnostics-<uuid>.zip, recorded in history as a diagnostics upload; on a public-read bucket anyone with the link can download it, and a Proton Drive link expires after 7 days. copy_link also copies the link. Returns path, bytes, sha256, counts, and with upload: upload (destination, warning, operationId, historyId), link or uploadError."""
        if operation_id is not None and not identifier(operation_id, 128):
            return invalid()
        if since is not None and not (isinstance(since, str) and 2 <= len(since) <= 8):
            return invalid()
        if type(include_names) is not bool or (output is not None and (not identifier(output, 4096) or not output.startswith('/'))):
            return invalid()
        if type(preview) is not bool or type(notify) is not bool or (preview and notify):
            return invalid()
        if type(upload) is not bool or type(copy_link) is not bool or (profile_id is not None and not identifier(profile_id, 64)):
            return invalid()
        # A preview writes and uploads nothing.
        if (profile_id is not None or copy_link) and not upload or (preview and upload):
            return invalid()
        params: Json = {}
        if operation_id is not None:
            params['operation'] = operation_id
        if since is not None:
            params['since'] = since
        if include_names:
            params['includeNames'] = True
        if output is not None:
            params['output'] = output
        if preview:
            params['preview'] = True
        if notify:
            params['notify'] = True
        if upload:
            params['upload'] = True
            if profile_id is not None:
                params['profileId'] = profile_id
            if copy_link:
                params['copyLink'] = True
        # The CLI waits up to 600 s for a package that is uploaded.
        return await adapter.call('diagnostics.bundle', params, timeout=630 if upload else None)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=True))
    async def get_diagnostics_upload_target(profile_id: str | None = None) -> Json:
        """Where a diagnostics package would upload (profile_id, default the capture destination) and whether that destination works now: its last upload did not fail with a credential or configuration error and Test connection passes (S3 Test connection writes and removes a small test object). Returns available, profileId, label, provider, publicLink, warning and shareExpirationDays (Proton), or available false with reason and message. Settings' diagnostics dialog uses this to offer "Also upload and copy a link"."""
        if profile_id is not None and not identifier(profile_id, 64):
            return invalid()
        return await adapter.call('diagnostics.uploadTarget', {} if profile_id is None else {'profileId': profile_id}, timeout=330)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def show_diagnostics_package(path: str) -> Json:
        """Show in folder: open the file manager with a saved diagnostics package selected (org.freedesktop.FileManager1.ShowItems); without one, the XDG portal's OpenDirectory, else gio open on its folder with the host's environment. path is the absolute path create_diagnostics_bundle returned; put.gd's own putgd-diagnostics-*.zip names are accepted, and any other path a diagnostics package was saved to by this resident."""
        if not identifier(path, 4096) or not path.startswith('/'):
            return invalid()
        return await adapter.call('diagnostics.reveal', {'path': path})

    @server.tool(annotations=read)
    async def get_update_status() -> Json:
        """The daily update check (#293): build (version, kind, channel, packageTarget, supported, reason), check and channelSetting (preferences.updates), the effective channel and channelSource (setting, or repository: a DEB/RPM follows its pkg.put.gd repository's suite, whatever channelSetting says, #386), enabled, notifies (stable announces each new version once; nightly only reports it; a repository install never, since its system updater does), action, checking, attemptedAt, checkedAt, nextCheckAt, result (current, latest, tag, publishedAt, notesUrl, downloadUrl, updateAvailable; latest null when the channel has no release yet), the last error and install (the AppImage's own install: kind update|switch, switch {from, to, version, tag, older} when it would switch channels, available and reason, state and progress; see install_update). action is {kind: software-updates, manager apt|dnf, file, channel, command, updater {id, name}|null, otherChannel {channel, commands, docs}|null, older {version, command}|null} while the package's pkg.put.gd repository file is present and enabled (open it with open_software_updates); otherChannel lists the terminal commands that move to the other channel and older the one that installs the repository's older release, which the user runs themselves: put.gd never changes the repository, otherwise {kind: download, repository: absent|disabled|other|none}. Development, source and Flatpak builds report supported false with a reason. Turn checks off or pick a channel with patch_configuration's preferences.updates."""
        return await adapter.call('update.status', {})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=True))
    async def check_for_updates() -> Json:
        """Check for a newer version now, as Settings → General → Updates → Check now does, even with daily checks off: one GET of https://app.put.gd/update/v1/<channel>.json with nothing identifying, compared on this computer. Returns get_update_status's object; a failed check returns its error (update_feed_unavailable, update_feed_invalid, update_check_failed or update_check_unavailable). Nothing is downloaded or installed."""
        return await adapter.call('update.check', {}, timeout=70)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def open_software_updates() -> Json:
        """Open the system's software updater, as Settings → General → Updates → Open software updates does, for a DEB/RPM install that updates from put.gd's apt or dnf repository: Update Manager on Cinnamon, Discover on Plasma, GNOME Software on GNOME, else whichever is installed. Returns {opened, action}. Errors: software_updates_unavailable (not a repository install, or no updater installed; the details carry the apt/dnf command) or software_updates_failed. Nothing is installed by put.gd."""
        return await adapter.call('update.openSoftwareUpdates', {})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=True))
    async def install_update(confirm: bool, method: str = 'auto', channel: str | None = None, allow_older: bool = False) -> Json:
        """Install the newest version over the running AppImage, as Settings → General → Updates → Download and install does (#293 phase 3). Needs confirm true; nothing installs otherwise. AppImage builds only, while no recording or upload runs. When get_update_status's install.kind is switch (the watched channel isn't this build's, #344), this is Settings' Switch to <channel> <version>: it installs install.switch {from, to, version, tag, older}, the other channel's release the last check found, checked with that channel's own key and update permalink; channel must name install.switch.to, and allow_older true is needed when install.switch.older (the target version is older than this build). install.reason no_release means that channel has no release yet, not_checked that it hasn't been checked (check_for_updates first). method auto tries appimageupdatetool or zsync2 on the host with the release's mirrored .zsync, then downloads the whole image from the GitHub release (then pkg.put.gd); zsync or download forces one path. The file must match the release's SHA256SUMS line before it is renamed onto the same path; the old image stays as a hidden .putgd-update-backup-<version>.AppImage until the new version has started. Returns get_update_status's object at once; follow install (state running|installed|failed, phase checking|verifying|zsync|download|installing|done, method, bytes, total, error, result with method, zsync.fallbackReason and backup, restartRequired) with get_update_status, then restart_after_update. Errors: update_confirm_required, update_install_unavailable, update_install_busy, update_restart_required, update_switch_confirm_required (a switch without its channel), update_switch_older (an older target without allow_older); the child's own (update_not_available, update_no_release, update_image_not_writable, update_disk_full, update_sums_unavailable, update_download_failed, update_verification_failed, update_zsync_failed, update_install_failed) arrive in install.error."""
        if not isinstance(confirm, bool) or method not in ('auto', 'zsync', 'download') or channel not in (None, 'stable', 'nightly') or not isinstance(allow_older, bool):
            return invalid()
        params: dict[str, Any] = {'confirm': confirm}
        if method != 'auto':
            params['method'] = method
        if channel is not None:
            params['channel'] = channel
        if allow_older:
            params['allowOlder'] = True
        return await adapter.call('update.install', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def restart_after_update(confirm: bool) -> Json:
        """Restart put.gd into the version install_update put in place, as Settings → General → Updates → Restart put.gd does: starts the AppImage again from the host once this resident has exited, then quits it (open windows close). Needs confirm true and get_update_status's install.restartRequired; refused while a recording or upload runs. Returns {restarting, version}; the new resident answers this MCP server's next call. Errors: update_confirm_required, update_restart_unavailable, update_install_busy, update_restart_failed."""
        if not isinstance(confirm, bool):
            return invalid()
        return await adapter.call('update.restart', {'confirm': confirm})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False))
    async def clear_logs() -> Json:
        """Delete every persistent log file in put.gd's state folder and start a new one; returns removedFiles and removedBytes. Uploads, history, settings and saved diagnostics packages are untouched. inspect shows the log folder size, file count and oldest record under diagnostics.logging."""
        return await adapter.call('logs.clear', {})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def patch_configuration(patch: Json, expected_revision: str) -> Json:
        """Patch non-secret settings with expected file revision; conflicts require reread/rebase. Null is explicit. limits.maxFileBytes, maxBatchBytes and pendingArtifactBytes take a byte count, "auto" or null (no byte limit of put.gd's own; free disk and the destination's maximum still apply); inspect shows the resolved values and disk figures under configuration.result.effectiveLimits. preferences.regionSelectionMode picks how region screenshots select (#629): "live" (default; the desktop keeps updating and an open menu must close first) or "screenshot_first" (every monitor is captured before the selector opens and the region is cut from that frozen frame, so open menus are included); recording always selects live."""
        allowed = {'schemaVersion', 'actions', 'captureDefaults', 'profiles', 'archive', 'naming', 'limits', 'retention', 'preferences'}
        if (not isinstance(patch, dict) or set(patch) - allowed or not safe_patch(patch) or
            not isinstance(expected_revision, str) or len(expected_revision) > 256):
            return invalid()
        return await adapter.call('settings.patch', {'patch': patch, 'expectedRevision': expected_revision})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def reorder_destinations(order: list[str], expected_revision: str) -> Json:
        """Set the display order of destinations (Settings list and Destination dropdowns) by listing every profile ID exactly once. Routing, exceptions and history keep their IDs. A stale revision or a missing, extra or duplicate ID changes nothing."""
        if (not isinstance(order, list) or len(order) > 64 or not all(identifier(item, 64) for item in order) or
                not isinstance(expected_revision, str) or len(expected_revision) > 256):
            return invalid()
        return await adapter.call('settings.reorderProfiles', {'order': order, 'expectedRevision': expected_revision})

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def move_destination(direction: str, profile_id: str | None = None) -> Json:
        """Drive the open Settings window like Move up / Move down: select profile_id (or keep the selection), move it one place and save. Returns the new order; read settings via inspect to confirm the save."""
        if direction not in {'up', 'down'} or (profile_id is not None and not identifier(profile_id, 64)):
            return invalid()
        params: Json = {'direction': direction}
        if profile_id is not None:
            params['profileId'] = profile_id
        return await adapter.call('ui.moveDestination', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, openWorldHint=False))
    async def settings_diagnostics(action: str, enabled: bool | None = None, include_names: bool | None = None,
                                   folder: str | None = None, operation_id: str | None = None,
                                   upload: bool | None = None) -> Json:
        """Drive Settings → Advanced → Diagnostics in the open Settings window as a user does. action: inspect (debug logging and when it ends, log folder size/files/oldest record, logging degraded, last unclean exit, package dialog state); setDebugLogging with enabled; setDesktopDetails with enabled (expand/collapse the desktop support readout); copyDetails (copy exact diagnostics); showConfigFolder (show the configuration file in its folder); refreshDesktopSupport; clearLogs (deletes every log file, without the confirmation dialog); openPackage (Create diagnostics package…, optional operation_id and include_names; answers with the preview); setPackage (include_names, an existing absolute folder, which Settings remembers, and/or upload: tick or clear "Also upload and copy a link", offered only when package.uploadTarget is available; uploadTarget.checking means the destination check is still running, otherwise reason and message say why it is unavailable); savePackage (saves locally and returns the path; with upload it answers once saving starts, and inspect shows package.saving until package.saved has link, linkCopied or uploadError; the local file is kept either way); showInFolder; closePackage. Every answer carries the group's state."""
        actions = {'inspect', 'setDebugLogging', 'setDesktopDetails', 'copyDetails', 'showConfigFolder', 'refreshDesktopSupport', 'clearLogs', 'openPackage', 'setPackage', 'savePackage', 'showInFolder', 'closePackage'}
        if action not in actions or (enabled is not None and type(enabled) is not bool) or (include_names is not None and type(include_names) is not bool) \
                or (upload is not None and (type(upload) is not bool or action != 'setPackage')):
            return invalid()
        if (folder is not None and (not identifier(folder, 4096) or not folder.startswith('/'))) or (operation_id is not None and not identifier(operation_id, 128)):
            return invalid()
        if (action in {'setDebugLogging', 'setDesktopDetails'}) != (enabled is not None):
            return invalid()
        if (folder is not None and action != 'setPackage') or (operation_id is not None and action != 'openPackage'):
            return invalid()
        if include_names is not None and action not in {'openPackage', 'setPackage'}:
            return invalid()
        params: Json = {'action': action}
        for key, value in (('enabled', enabled), ('includeNames', include_names), ('folder', folder), ('operation', operation_id), ('upload', upload)):
            if value is not None:
                params[key] = value
        return await adapter.call('ui.diagnostics', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def drive_file_dialog(dialog: str, action: str, field: str | None = None, id: str | None = None) -> Json:
        """Open (action open) a file chooser in the open put.gd window as its button does, read its state (inspect) or close it as a cancel (cancel). dialog: editImage (Edit an image file…), historySaveAs (History → Save as… for the selected file, or the row with history or operation id), outputFolder (Settings → Capture → Local copies: Browse… for field root, images, videos, text or other) or diagnosticsFolder (Choose folder… in an open Create diagnostics package dialog). The chooser is the desktop's own through the XDG portal, or the themed fallback dialog; the reply shows open, backend portal|fallback, parentWindow, fallbackReason and the last result (status chosen|cancelled|failed, path)."""
        if dialog not in FILE_DIALOGS or action not in FILE_DIALOG_ACTIONS:
            return invalid()
        if field is not None and (dialog != 'outputFolder' or field not in OUTPUT_FOLDER_FIELDS):
            return invalid()
        if id is not None and (dialog != 'historySaveAs' or not identifier(id, 128)):
            return invalid()
        params: Json = {'dialog': dialog, 'action': action}
        if field is not None:
            params['field'] = field
        if id is not None:
            params['id'] = id
        return await adapter.call('ui.fileDialog', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def drive_history(action: str, scope: str | None = None, ids: list[str] | None = None) -> Json:
        """Drive the open History page as its controls do and read its state. action: inspect; copyDeletionDetails (copy the exact preparation shown by an open deletion confirmation, without deleting); retryLoading (retry the current history query after a load failure; canRetryLoading reports availability); clearFilters (Clear filters; scope all also clears the search and shows All items, as the empty view's reset does); select (ids: history, operation or row IDs on the page shown, selected together); clearSelection; openDetails or closeDetails (the inspector over the list in the compact layout). Returns the applied query, activeFilters, filtersLabel ("Filters · N"), selectedIds, selectedRows, selectionActions (copyLinks, delete, clearSelection), deletableIds, layout (split or compact), detailsOpen and inspectorVisible. retryPreview retries a failed preview from retained source (canRetryPreview reports availability). expandPreview/closePreview open or close the selected image or recording poster in a larger dialog; canExpandPreview and previewExpanded report availability and state. dismissSavedCopy hides the saved-copy strip without losing its path; savedCopyVisible and copyJob (id, filename, path, status, error) report feedback and pending work. showSavedCopy/copySavedPath reveal or copy the last saved copy path (savedPath; savedCopyFolder reports the folder result). cancelAll opens the same confirmation as Cancel all (N)…; inspect cancelAllConfirmation.operationIds, then confirmCancelAll or keepUploading. Only those captured operation IDs are canceled; newer uploads stay running. selectionSummary includes counts, total bytes, destinations and upload-state tallies. Delete selected files with delete_history_uploads using deletableIds."""
        if action not in HISTORY_ACTIONS or (scope is not None and (action != 'clearFilters' or scope not in {'filters', 'all'})):
            return invalid()
        if (action == 'select') != (ids is not None) or (ids is not None and (not ids or len(ids) > 1000 or any(not identifier(id, 138) for id in ids))):
            return invalid()
        params: Json = {'action': action}
        if scope is not None:
            params['scope'] = scope
        if ids is not None:
            params['ids'] = ids
        return await adapter.call('ui.history', params)

    @server.tool(annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, openWorldHint=False))
    async def history_save_diagnostics(id: str) -> Json:
        """Drive the open History window: select the failed upload with this operation ID (or a file's history ID) on the page shown and click Save diagnostics for this upload. Saves a package for that one operation locally (nothing is uploaded), shows the "diagnostics saved" notification and returns its path."""
        return await adapter.call('ui.historySaveDiagnostics', {'id': id}) if identifier(id, 128) else invalid()

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description='Local stdio MCP adapter for the production put.gd CLI')
    parser.add_argument('--cli', default='putgd-cli')
    parser.add_argument('--socket')
    parser.add_argument('--timeout', type=float, default=CLI_TIMEOUT)
    args = parser.parse_args()
    try:
        adapter = CliAdapter(args.cli, args.socket, args.timeout)
    except ValueError as error:
        parser.error(str(error))
    create_server(adapter).run(transport='stdio')


if __name__ == '__main__':
    main()
