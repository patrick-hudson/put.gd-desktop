#!/usr/bin/env python3
"""Cinnamon memory checks for Edit & upload, agent-driven through the production CLI.

Run from the source root on the QEMU host:  python3 tools/lab/edit_memory.py RUN_DIR [SAMPLER]
Gate: whole-family RSS+swap incremental peak <= 3R + 96 MiB per case, and the
resident returns within 8 MiB of its baseline once another client owns the
clipboard. Clipboard-held results are reported separately (charged, by design).
Synthetic high-entropy inputs; local output; the family sampler runs in the guest.
"""
import json, shlex, sys, time
from pathlib import Path
sys.path.insert(0, 'tools/lab')
import lab

run = Path(sys.argv[1]).resolve()
sampler = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).with_name('family_sampler.py')).resolve()
out = run / 'editor-memory'; out.mkdir(exist_ok=True)
MiB = 1024 * 1024
result = {'schemaVersion': 1, 'scenario': 'edit-memory', 'ok': False, 'cases': [], 'host': None}
user = lab.state(run)['guest_user']
lab.put(run, str(sampler), f'/home/{user}/putgd-lab/family_sampler.py')

def sh(body):
    return lab.guest_session(run, body)

def envelope(method, params):
    return json.loads(sh('putgd-cli ' + shlex.quote(method) + ' ' + shlex.quote(json.dumps(params)) + ' || true\n'))

def ok(method, params):
    reply = envelope(method, params)
    if not reply['ok']:
        raise lab.LabError(f'{method}: {reply["error"]}')
    return reply['result']

def wait(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= deadline:
            raise lab.LabError('timeout: ' + what)
        time.sleep(.25)

def editor_up(op):
    reply = envelope('editor.inspect', {'id': op})
    if reply['ok']:
        return reply['result']
    if reply['error']['code'] != 'editor_starting':
        raise lab.LabError(str(reply['error']))
    return None

def settled(op):
    o = ok('operation.inspect', {'id': op})
    return o if o['state'] in ('completed', 'failed', 'cancelled', 'partial') and o.get('copyStatus') != 'pending' else None

GENERATOR = r"""
import os, struct, sys, zlib
w, h, path = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
packer = zlib.compressobj(1)
with open(path + '.part', 'wb') as f:
    f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
    for top in range(0, h, 256):
        rows = min(256, h - top)
        data = b''.join(b'\x00' + os.urandom(w * 3) for _ in range(rows))
        f.write(chunk(b'IDAT', packer.compress(data)))
    f.write(chunk(b'IDAT', packer.flush()) + chunk(b'IEND', b''))
os.replace(path + '.part', path)
"""

def make_input(name, width, height):
    # High-entropy RGB noise written as a real PNG by the guest's own Python.
    path = f'/home/{user}/putgd-lab/memory/{name}.png'
    sh('mkdir -p $HOME/putgd-lab/memory\npython3 -c ' + shlex.quote(GENERATOR) + f' {width} {height} {path}\nhead -c 8 {path} | od -An -tx1\n')
    return path

READY = lab.read_json(run / 'ready.json')
SCREEN = READY['display_layout'][0]

def resident_kib():
    return int(sh("grep VmRSS /proc/$(pgrep -x putgd | head -1)/status | awk '{print $2}'\n").strip())

def release_clipboard():
    # Local image results stay on the clipboard, owned by the resident (charged).
    # Let the independent fixture take ownership through real input to measure the rest.
    paint = dict(READY['paint'])
    if READY['session'] == 'wayland':
        # Physical VNC frames include the cursor; park it before locating.
        lab.pointer(run, SCREEN['width'] - 40, 40, SCREEN['width'], SCREEN['height']); time.sleep(.3)
        located = lab.locate_fixture_paint(lab.screenshot(run, 'memory-fixture')['path'])
        if not located:
            raise lab.LabError('fixture not visible; cannot hand it clipboard ownership')
        paint.update(x=located['x'], y=located['y'])
    lab.pointer(run, paint['x'] + 30, paint['y'] + 30, SCREEN['width'], SCREEN['height'], True)
    lab.pointer(run, paint['x'] + 30, paint['y'] + 30, SCREEN['width'], SCREEN['height'], False)
    time.sleep(.5); lab.key(run, ['ctrl', 'alt', 'shift', 't']); time.sleep(4)

def sample(label, body):
    remote = f'/home/{user}/putgd-lab/memory/{label}.jsonl'; stop = remote + '.stop'
    sh(f'rm -f {remote} {stop}\nnohup python3 $HOME/putgd-lab/family_sampler.py {remote} {stop} > /dev/null 2>&1 < /dev/null &\n')
    time.sleep(3)
    marks = {'start': None}
    clipboard = {}
    try:
        marks = body()
    finally:
        time.sleep(12)
        sh(f'touch {stop}\nsleep .3\n')
        held = resident_kib(); release_clipboard(); released = resident_kib()
        clipboard = {'residentWithClipboardMiB': round(held / 1024, 1), 'residentAfterClipboardReleaseMiB': round(released / 1024, 1)}
        lab.get(run, f'putgd-lab/memory/{label}.jsonl', out / f'{label}.jsonl')
    rows = [json.loads(line) for line in (out / f'{label}.jsonl').read_text().splitlines() if line.strip()]
    t0 = rows[0]['t']
    baseline = sorted(r['rssSwapKiB'] for r in rows if r['t'] - t0 < 2.5)
    base = baseline[len(baseline) // 2]
    peak = max(rows, key=lambda r: r['rssSwapKiB'])
    tail = sorted(r['rssSwapKiB'] for r in rows if rows[-1]['t'] - r['t'] < 5)
    pss_base = sorted(r['pssKiB'] for r in rows if r['t'] - t0 < 2.5)[len(baseline) // 2]
    def resident(r):
        return next((m[2] for m in r['members'] if m[1] == 'putgd'), 0)
    res_base = sorted(resident(r) for r in rows if r['t'] - t0 < 2.5)
    res_tail = sorted(resident(r) for r in rows if rows[-1]['t'] - r['t'] < 5)
    residentBase, residentSettled = res_base[len(res_base) // 2], res_tail[len(res_tail) // 2]
    return {'baselineMiB': round(base / 1024, 1), 'peakMiB': round(peak['rssSwapKiB'] / 1024, 1),
            'incrementalPeakMiB': round((peak['rssSwapKiB'] - base) / 1024, 1),
            'peakPssMiB': round(max(r['pssKiB'] for r in rows) / 1024, 1), 'baselinePssMiB': round(pss_base / 1024, 1),
            'settledMiB': round(tail[len(tail) // 2] / 1024, 1), 'settledDeltaMiB': round((tail[len(tail) // 2] - base) / 1024, 1),
            'residentBaselineMiB': round(residentBase / 1024, 1), 'residentSettledMiB': round(residentSettled / 1024, 1),
            'residentSettledDeltaMiB': round((residentSettled - residentBase) / 1024, 1),
            **clipboard,
            'peakMembers': peak['members'], 'samples': len(rows), **(marks or {})}

def edit_cycle(path, width, height):
    def body():
        marks = {}
        start = time.monotonic()
        op = ok('editor.open', {'source': 'file', 'path': path})['operationId']
        state = wait(lambda: editor_up(op), 120, 'editor open')
        marks['openSeconds'] = round(time.monotonic() - start, 2)
        assert (state['width'], state['height']) == (width, height)
        commands = [{'op': 'add', 'shape': {'kind': 'redact', 'rect': [i * width // 12, height // 10, width // 14, height // 12]}} for i in range(10)]
        commands += [{'op': 'add', 'shape': {'kind': 'blur', 'rect': [0, height // 2, width // 2, height // 2], 'strength': 32}},
                     {'op': 'add', 'shape': {'kind': 'arrow', 'from': [width - 40, height - 40], 'to': [width // 2, height // 2]}},
                     {'op': 'add', 'shape': {'kind': 'text', 'at': [40, 40], 'text': 'synthetic memory case'}},
                     {'op': 'crop', 'rect': [8, 8, width - 16, height - 16]}]
        ok('editor.apply', {'id': op, 'commands': commands})
        ok('editor.preview', {'id': op, 'maxEdge': 1600})
        start = time.monotonic()
        ok('editor.submit', {'id': op})
        final = wait(lambda: settled(op), 300, 'operation')
        marks['submitToCompletedSeconds'] = round(time.monotonic() - start, 2)
        marks['operationState'] = final['state']; marks['artifactBytes'] = final['items'][0]['artifact']['bytes'] if final['items'] else 0
        return marks
    return body

try:
    result['host'] = {'versions': json.loads(sh('cat /etc/os-release | grep -E "^(PRETTY_NAME)=" | sed "s/.*=//" | python3 -c "import json,sys;print(json.dumps(sys.stdin.read().strip()))"')),
                      'memTotalKiB': int(sh('grep MemTotal /proc/meminfo | awk \'{print $2}\'').strip())}
    for name, width, height in [('fhd', 1920, 1080), ('uhd', 3840, 2160), ('mp24', 6000, 4000)]:
        path = make_input(name, width, height)
        raster = width * height * 4
        case = {'name': name, 'width': width, 'height': height, 'rasterMiB': round(raster / MiB, 2),
                'targetMiB': round((3 * raster + 96 * MiB) / MiB, 1)}
        case.update(sample(name, edit_cycle(path, width, height)))
        case['withinTarget'] = case['incrementalPeakMiB'] <= case['targetMiB'] and case['operationState'] == 'completed' and case['residentAfterClipboardReleaseMiB'] - case['residentBaselineMiB'] <= 8
        result['cases'].append(case)
        print(json.dumps({k: case[k] for k in ('name', 'incrementalPeakMiB', 'targetMiB', 'residentBaselineMiB', 'residentWithClipboardMiB', 'residentAfterClipboardReleaseMiB', 'withinTarget', 'openSeconds', 'submitToCompletedSeconds')}), flush=True)

    # 100 edits plus undo/redo churn on 4K; history and blur caches stay bounded.
    uhd = f'/home/{user}/putgd-lab/memory/uhd.png'
    def churn():
        op = ok('editor.open', {'source': 'file', 'path': uhd})['operationId']
        wait(lambda: editor_up(op), 120, 'editor open')
        for batch in range(4):
            ok('editor.apply', {'id': op, 'commands': [{'op': 'add', 'shape': {'kind': 'blur' if i % 5 == 0 else 'rectangle',
                'rect': [(batch * 25 + i) * 30 % 3500, (i * 70) % 1900, 300, 200]}} for i in range(25)]})
        for _ in range(5):
            ok('editor.apply', {'id': op, 'commands': [{'op': 'undo'}] * 50})
            ok('editor.apply', {'id': op, 'commands': [{'op': 'redo'}] * 50})
        state = ok('editor.inspect', {'id': op})
        ok('editor.cancel', {'id': op})
        wait(lambda: settled(op), 60, 'cancel')
        return {'shapes': len(state['shapes']), 'historyTruncated': state['historyTruncated']}
    case = {'name': 'uhd-100-edits-undo-redo', 'rasterMiB': round(3840 * 2160 * 4 / MiB, 2), 'targetMiB': round((3 * 3840 * 2160 * 4 + 96 * MiB) / MiB, 1)}
    case.update(sample('churn', churn)); case['withinTarget'] = case['incrementalPeakMiB'] <= case['targetMiB'] and case['residentAfterClipboardReleaseMiB'] - case['residentBaselineMiB'] <= 8
    result['cases'].append(case)
    print(json.dumps({k: case.get(k) for k in ('name', 'incrementalPeakMiB', 'targetMiB', 'residentBaselineMiB', 'residentWithClipboardMiB', 'residentAfterClipboardReleaseMiB', 'shapes', 'withinTarget')}), flush=True)

    # Repeated open/cancel must not accumulate resident memory.
    def cycles():
        for _ in range(5):
            op = ok('editor.open', {'source': 'file', 'path': uhd})['operationId']
            wait(lambda: editor_up(op), 120, 'editor open')
            ok('editor.cancel', {'id': op})
            wait(lambda: settled(op), 60, 'cancel')
        return {'cycles': 5}
    case = {'name': 'uhd-open-cancel-x5', 'targetMiB': round((3 * 3840 * 2160 * 4 + 96 * MiB) / MiB, 1)}
    case.update(sample('cycles', cycles)); case['withinTarget'] = case['incrementalPeakMiB'] <= case['targetMiB'] and case['residentAfterClipboardReleaseMiB'] - case['residentBaselineMiB'] <= 8
    result['cases'].append(case)
    print(json.dumps({k: case.get(k) for k in ('name', 'incrementalPeakMiB', 'residentBaselineMiB', 'residentWithClipboardMiB', 'residentAfterClipboardReleaseMiB', 'withinTarget')}), flush=True)

    # Above admission: rejected before any editor process starts.
    big = make_input('over', 6000, 4001)
    op = ok('editor.open', {'source': 'file', 'path': big})['operationId']
    final = wait(lambda: settled(op), 120, 'oversize')
    result['oversize'] = {'state': final['state'], 'error': final['error']['code'], 'items': len(final['items'])}
    print(json.dumps(result['oversize']), flush=True)
    result['ok'] = all(c['withinTarget'] for c in result['cases']) and result['oversize']['error'] == 'edit_input_too_large' and result['oversize']['items'] == 0
except BaseException as error:
    result['error'] = repr(error)
finally:
    lab.write_json(out / 'results.json', result)
    print(json.dumps({'ok': result['ok'], 'error': result.get('error')}))
