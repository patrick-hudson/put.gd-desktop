"""#283 end to end through MCP: official SDK client -> putgd-mcp (this worktree) -> production putgd-cli on the
lab host (an SSH wrapper) -> resident -> putgd-record's review window.

    CLI=wrapper SOCKET=/path/session.sock REGION=x,y,w,h OUT=/remote/dir HOSTRUN=wrapper-for-host-commands \
      python r283_mcp_e2e.py EVIDENCE_DIR [--control]

Records the lab test card, works the review window with drive_recording_review
(trim in/out at the playhead, a blur and a solid cover, the blur's strength and
end), previews, saves a copy and (unless --control) uploads it to the loopback
S3 fixture. Motion scores come from the host's putgd-record --motion.
"""
import asyncio, base64, json, os, subprocess, sys, time
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

EVIDENCE = Path(sys.argv[1]); EVIDENCE.mkdir(parents=True, exist_ok=True)
CONTROL = '--control' in sys.argv
NAME = 'control' if CONTROL else 'e2e'
REGION = [int(v) for v in os.environ['REGION'].split(',')]
OUT = os.environ['OUT']
log = {'schemaVersion': 1, 'scenario': 'r283-review-' + NAME, 'ok': False, 'checks': {}, 'calls': []}


def check(name, ok, detail=None):
    log['checks'][name] = {'ok': bool(ok), **({'detail': detail} if detail is not None else {})}
    if not ok:
        raise RuntimeError(f'{name}: {detail}')


def host(*args):
    return subprocess.run([os.environ['HOSTRUN'], *args], capture_output=True, text=True, timeout=600).stdout.strip()


async def main():
    tools_dir = Path(__file__).resolve().parents[0] / 'mcp'
    params = StdioServerParameters(command=sys.executable, args=['-m', 'share_mcp.server', '--cli', os.environ['CLI'],
                                                                 '--socket', os.environ['SOCKET'], '--timeout', '90'],
                                   cwd=os.environ['MCP_DIR'])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = {tool.name for tool in (await session.list_tools()).tools}
            check('reviewToolsDiscovered', {'prepare_recording', 'control_recording', 'get_recording', 'drive_recording_review',
                                            'edit_recording', 'preview_recording', 'save_recording_copy', 'upload_recording',
                                            'get_operation'} <= tools, sorted(tools))

            async def call(name, args, expect_ok=True):
                started = time.monotonic()
                reply = await session.call_tool(name, args)
                data = reply.structuredContent if reply.structuredContent is not None else None
                if data is None:
                    texts = [c.text for c in reply.content if getattr(c, 'type', '') == 'text']
                    data = json.loads(texts[-1]) if texts else {}
                if 'result' in data and isinstance(data['result'], dict) and 'ok' in data['result'] and 'requestId' in data['result']:
                    data = data['result']
                log['calls'].append({'tool': name, 'args': args, 'ok': data.get('ok'), 'ms': round(1000 * (time.monotonic() - started)),
                                     'error': data.get('error') if not data.get('ok') else None})
                if expect_ok and not data.get('ok'):
                    raise RuntimeError(f'{name} failed: {data.get("error")}')
                return data.get('result') or {}

            async def wait(rid, states, timeout=180):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    state = await call('get_recording', {'id': rid})
                    if state['state'] in states:
                        return state
                    await asyncio.sleep(0.5)
                raise RuntimeError(f'{rid} never reached {states}')

            async def review(action, **args):
                return await call('drive_recording_review', {'id': rid, 'action': action, **args})

            prepared = await call('prepare_recording', {'target': {'kind': 'region', 'rect': REGION}, 'format': 'mp4', 'fps': 30})
            rid = prepared['recordingId']
            await wait(rid, ('ready',))
            await asyncio.sleep(1)
            await call('control_recording', {'id': rid, 'command': 'start'})
            await asyncio.sleep(9)
            stopped = await call('control_recording', {'id': rid, 'command': 'stop'})
            state = await wait(rid, ('reviewing',))
            log['durationUs'] = state['durationUs']
            check('recordedLongEnough', state['durationUs'] >= 8000000, state['durationUs'])

            empty = await review('inspect')
            log['inspectEmpty'] = {k: empty[k] for k in ('visible', 'size', 'video', 'maskControls', 'timeline', 'scheme')}
            check('reviewWindowOpen', empty['visible'], empty['size'])
            check('noMaskControlsWithoutMasks', not any(empty['maskControls'].values()), empty['maskControls'])
            # Trim in/out at the playhead (the I and O keys).
            await review('seek', time_us=1000000)
            after_in = await review('setIn')
            check('setInTrims', after_in['edit']['trimStart'] == 1000000, after_in['edit'])
            await review('seek', time_us=7000000)
            after_out = await review('setOut')
            check('setOutTrims', after_out['edit'].get('trimEnd') == 7000000, after_out['edit'])
            # Two masks at the playhead, as Add blur / Add solid cover and a drag place them.
            await review('seek', time_us=1500000)
            await review('addMask', kind='blur', rect=[380, 205, 260, 310])
            two = await review('addMask', kind='solid', rect=[660, 250, 240, 140])
            masks = two['edit']['masks']
            check('twoMasks', len(masks) == 2 and {m['kind'] for m in masks} == {'blur', 'solid'}, masks)
            check('masksStartAtPlayhead', all(m['start'] == 1500000 for m in masks), masks)
            solid = next(m for m in masks if m['kind'] == 'solid'); blur = next(m for m in masks if m['kind'] == 'blur')
            check('newMaskSelectedWithContextualControls', two['selectedMaskId'] == solid['id'] and two['maskControls'] ==
                  {'list': True, 'times': True, 'strength': False, 'remove': True}, [two['selectedMaskId'], two['maskControls']])
            check('timelineLanes', two['timeline']['lanes'] == 2, two['timeline'])
            picked = await review('selectMask', mask_id=blur['id'])
            check('blurShowsStrength', picked['maskControls']['strength'], picked['maskControls'])
            strong = await review('strength', strength=24)
            check('strengthApplied', next(m for m in strong['edit']['masks'] if m['id'] == blur['id'])['strength'] == 24, strong['edit'])
            await review('seek', time_us=5000000)
            ended = await review('setMaskOut')
            check('maskOutAtPlayhead', next(m for m in ended['edit']['masks'] if m['id'] == blur['id'])['end'] == 5000000, ended['edit'])
            log['inspectEdited'] = {k: ended[k] for k in ('video', 'maskControls', 'timeline', 'selectedMaskId', 'playheadUs', 'trim')}
            log['edit'] = ended['edit']
            revision = int(ended['edit']['revision'])
            # A frame with both masks active, from the authoritative edit.
            frame = await session.call_tool('preview_recording', {'id': rid, 'time_us': 3000000, 'max_edge': 960})
            image = next(c for c in frame.content if getattr(c, 'type', '') == 'image')
            (EVIDENCE / f'{NAME}-preview-3s.png').write_bytes(base64.b64decode(image.data))
            copy = f'{OUT}/{NAME}-copy.mp4'
            await call('save_recording_copy', {'id': rid, 'path': copy, 'overwrite': True})
            saved = await wait(rid, ('reviewing',), 600)
            check('copySaved', (saved.get('lastExport') or {}).get('status') == 'saved', saved.get('lastExport'))
            log['copy'] = saved['lastExport']
            check('copyIsTrimmed', abs(saved['lastExport']['durationUs'] - 6000000) <= 100000, saved['lastExport']['durationUs'])
            log['copyMotion'] = host('motion', copy)
            if CONTROL:
                await call('control_recording', {'id': rid, 'command': 'cancel'})
            else:
                uploaded = await call('upload_recording', {'id': rid, 'revision': revision})
                operation = uploaded.get('operationId') or rid
                deadline = time.monotonic() + 600
                while time.monotonic() < deadline:
                    op = await call('get_operation', {'id': operation})
                    if op.get('state') in ('completed', 'failed', 'cancelled', 'partial'):
                        break
                    await asyncio.sleep(1)
                log['upload'] = {'state': op.get('state'), 'items': [{k: i.get(k) for k in ('uploadStatus', 'artifact', 'objectReference')} | {'directUrl': (i.get('receipt') or {}).get('directUrl')} for i in op.get('items', [])]}
                check('uploadCompleted', op.get('state') == 'completed', op.get('state'))
                urls = [(item.get('receipt') or {}).get('directUrl') for item in op.get('items', [])]
                url = next((u for u in urls if u), None)
                check('uploadHasLink', url is not None, op.get('items'))
                log['uploadedMotion'] = host('fetch-motion', url)
    log['ok'] = True


try:
    asyncio.run(main())
finally:
    (EVIDENCE / f'{NAME}.json').write_text(json.dumps(log, indent=1))
    print(json.dumps({'ok': log['ok'], 'checks': {k: v['ok'] for k, v in log['checks'].items()}}))
