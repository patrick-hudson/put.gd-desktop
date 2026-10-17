"""MCP end-to-end against a real resident: official SDK client -> putgd-mcp ->
production putgd-cli on the target (a small SSH wrapper) -> resident -> visible editor.

    CLI=/path/to/target-putgd-cli-wrapper [SHOT=/path/to/screenshot-cmd NAME] \
    SIZE=1920x1080 HOMEDIR=/home/user \
    [SOCKET=/run/user/1000/putgd/session.sock] \
      tools/mcp/.venv/bin/python tools/lab/mcp_editor_e2e.py EVIDENCE_DIR

The wrapper must run the target's putgd-cli with identical arguments."""
import asyncio, base64, json, os, subprocess, sys, time, uuid
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

EVIDENCE = Path(sys.argv[-1]); EVIDENCE.mkdir(parents=True, exist_ok=True)
COPY = os.environ.get('HOMEDIR', '/home/lab') + '/putgd-lab/mcp-copy-' + uuid.uuid4().hex[:8] + '.png'
SOCKET = os.environ.get('SOCKET', '/run/user/1000/putgd/session.sock')
log = {'schemaVersion': 1, 'scenario': 'mcp-editor-e2e', 'ok': False, 'checks': {}, 'calls': []}

def check(name, ok, detail=None):
    log['checks'][name] = {'ok': bool(ok), **({'detail': detail} if detail is not None else {})}
    if not ok:
        raise RuntimeError(f'{name}: {detail}')

def shot(name):
    command = os.environ.get('SHOT')
    if command:
        subprocess.run([command, 'mcp-' + name], check=True, capture_output=True)

async def main():
    params = StdioServerParameters(command=str(Path('tools/mcp/.venv/bin/putgd-mcp').resolve()),
                                   args=['--cli', os.environ['CLI'], '--socket', SOCKET, '--timeout', '60'])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = {tool.name for tool in (await session.list_tools()).tools}
            check('editorToolsDiscovered', {'open_editor', 'get_editor', 'edit_image', 'preview_editor', 'save_editor_copy', 'submit_editor', 'cancel_editor', 'raise_editor'} <= tools, sorted(tools))

            async def call(name, args, expect_ok=True):
                reply = await session.call_tool(name, args)
                data = reply.structuredContent if reply.structuredContent is not None else json.loads(reply.content[-1].text)
                log['calls'].append({'tool': name, 'ok': data.get('ok'), 'error': data.get('error', {}).get('code')})
                if expect_ok and not data.get('ok'):
                    raise RuntimeError(f'{name} failed: {data.get("error")}')
                return data

            caps = (await call('inspect', {}))['result']['capabilities']['result']['editor']
            check('editorAvailable', caps['available'] and not caps['open'], caps)
            opened = await call('open_editor', {'source': 'desktop'})
            op = opened['result']['operationId']; log['operationId'] = op
            second = await call('open_editor', {'source': 'desktop'}, expect_ok=False)
            check('secondOpenRaisesExisting', second['error']['code'] == 'editor_active' and second['error']['details'].get('operationId') == op, second['error'])
            state = None
            for _ in range(150):
                reply = await call('get_editor', {'id': op}, expect_ok=False)
                if reply['ok']:
                    state = reply['result']; break
                if reply['error']['code'] != 'editor_starting':
                    raise RuntimeError(reply['error'])
                await asyncio.sleep(.2)
            check('editorOpenOnDesktopCapture', state and (state['width'], state['height']) == tuple(map(int, os.environ.get('SIZE', '1280x800').split('x'))) and state['phase'] == 'editing', state and [state['width'], state['height'], state['phase']])
            await asyncio.sleep(1); shot('01-open')
            applied = await call('edit_image', {'id': op, 'commands': [
                {'op': 'add', 'shape': {'kind': 'redact', 'rect': [0, int(os.environ.get('SIZE', '1280x800').split('x')[1]) - 40, 1280, 40]}},
                {'op': 'add', 'shape': {'kind': 'blur', 'rect': [900, 100, 300, 200], 'strength': 40}},
                {'op': 'add', 'shape': {'kind': 'arrow', 'from': [700, 600], 'to': [520, 420], 'color': '#ef4444', 'width': 8}},
                {'op': 'add', 'shape': {'kind': 'rectangle', 'rect': [400, 300, 300, 160], 'color': '#22c55e', 'width': 6}},
                {'op': 'add', 'shape': {'kind': 'highlight', 'rect': [100, 100, 400, 40], 'color': '#facc15'}},
                {'op': 'add', 'shape': {'kind': 'text', 'at': [100, 200], 'text': 'Edited through MCP', 'fontPixels': 44, 'color': '#ffffff'}},
                {'op': 'crop', 'rect': [0, 60, 1280, 740]},
                {'op': 'undo'}, {'op': 'redo'}]})
            ids = applied['result']['created']
            check('allShapeKindsAndCrop', len(ids) == 6 and applied['result']['state']['crop'] == [0, 60, 1280, 740], applied['result']['state']['crop'])
            updated = await call('edit_image', {'id': op, 'commands': [{'op': 'update', 'shapeId': ids[5], 'shape': {'text': 'Edited via MCP', 'color': '#60a5fa'}},
                                                                       {'op': 'remove', 'shapeId': ids[4]}]})
            check('updateAndRemove', len(updated['result']['state']['shapes']) == 5, len(updated['result']['state']['shapes']))
            bad = await call('edit_image', {'id': op, 'commands': [{'op': 'add', 'shape': {'kind': 'redact', 'rect': [1, 1, 2, 2], 'color': '#ffffff'}}]}, expect_ok=False)
            check('invalidShapeRejected', bad['error']['code'] == 'edit_command_invalid', bad['error'])
            preview = await session.call_tool('preview_editor', {'id': op, 'max_edge': 800})
            image, meta = preview.content
            png = base64.b64decode(image.data); (EVIDENCE / 'mcp-preview.png').write_bytes(png)
            meta = json.loads(meta.text)
            check('previewIsMcpImage', image.type == 'image' and image.mimeType == 'image/png' and png.startswith(b'\x89PNG') and meta['result']['sourceWidth'] == 1280 and meta['result']['sourceHeight'] == 740, meta['result'])
            saved = await call('save_editor_copy', {'id': op, 'path': COPY})
            check('saveCopy', saved['result']['saved'] and saved['result']['bytes'] > 0, saved['result'])
            again = await call('save_editor_copy', {'id': op, 'path': COPY}, expect_ok=False)
            check('saveCopyRefusesOverwrite', again['error']['code'] == 'file_exists', again['error'])
            await call('raise_editor', {'id': op})
            await asyncio.sleep(1); shot('02-edited')
            await call('submit_editor', {'id': op})
            final = None
            for _ in range(200):
                final = (await call('get_operation', {'id': op}))['result']
                if final['state'] in ('completed', 'failed', 'cancelled', 'partial') and final.get('copyStatus') != 'pending':
                    break
                await asyncio.sleep(.25)
            item = final['items'][0] if final['items'] else {}
            check('submittedOperationCompleted', final['state'] == 'completed' and len(final['items']) == 1 and item['artifact']['mime'] == 'image/png'
                  and item['artifact']['sha256'] == saved['result']['sha256'], {'state': final['state'], 'sha': item.get('artifact', {}).get('sha256'), 'copy': saved['result']['sha256']})
            await asyncio.sleep(1); shot('03-submitted')
            discard = (await call('open_editor', {'source': 'desktop'}))['result']['operationId']
            for _ in range(150):
                reply = await call('cancel_editor', {'id': discard}, expect_ok=False)
                if reply['ok'] or reply['error']['code'] != 'editor_starting':
                    break
                await asyncio.sleep(.2)
            check('cancelAccepted', reply['ok'], reply.get('error'))
            for _ in range(100):
                cancelled = (await call('get_operation', {'id': discard}))['result']
                if cancelled['state'] == 'cancelled':
                    break
                await asyncio.sleep(.2)
            check('cancelStoresNothing', cancelled['state'] == 'cancelled' and not cancelled['items'], {'state': cancelled['state'], 'items': len(cancelled['items'])})
            log['ok'] = True

try:
    asyncio.run(main())
except BaseException as error:
    log['error'] = repr(error)
finally:
    (EVIDENCE / 'mcp-results.json').write_text(json.dumps(log, indent=2))
    print(json.dumps({'ok': log['ok'], 'error': log.get('error'), 'checks': {k: v['ok'] for k, v in log['checks'].items()}}))
