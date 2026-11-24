#!/usr/bin/env python3
"""#1362 editor tools on a real desktop session, driven through putgd-cli.

Run on the target, inside the desktop session's environment, against a running
resident (stdlib only):

    python3 editor_tools_check.py --cli /usr/bin/putgd-cli OUT_DIR \
        [--socket /run/user/1000/putgd/session.sock] [--submit] [--keep-open]

It writes a synthetic 1200x760 PNG (a fake settings page with a fake token),
opens it in the editor (`editor.open` source file), then:

1. draws every new mark with `editor.apply`: steps 1-3 (one with a chosen
   number), a pen stroke, an ellipse, a line, a pixelation, plus a redaction
   over the fake token; checks the shapes, numbering and styles in
   `editor.inspect`;
2. works the window's own controls with `editor.view`: each new tool, the
   Blur/Pixelate style, the Step tool's Next number and `place`, then undo and
   redo through `editor.apply`;
3. `editor.copy`, then reads the desktop clipboard back (wl-paste on Wayland,
   xclip on X11) and requires the same sha256 as the reply and as
   `editor.saveCopy` of the same draft, and the redacted token pixels black;
4. with --submit, `editor.submit` and waits for the operation to complete.

Every reply is logged to OUT_DIR/log.json; the preview, the saved copy and the
pasted clipboard image are kept there. Nothing is uploaded without --submit.
"""
import argparse, base64, hashlib, json, os, shutil, struct, subprocess, sys, time, zlib
from pathlib import Path

OVERLAP_TOKEN = (900, 570, 200, 100)
TOKEN = (300, 214, 420, 36)  # the fake token box the redaction covers


def png(width, height, pixel):
    rows = b''.join(b'\x00' + b''.join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b'')


def synthetic(path, variant=False):
    # A light page with panels, text-like bars and a magenta "token" that
    # must never survive the redaction.
    def pixel(x, y):
        if any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in (TOKEN, OVERLAP_TOKEN)):
            return (0, 255, 255) if variant else (255, 0, 255)
        if y < 56:
            return (36, 40, 48)
        if 80 <= y < 680 and 60 <= x < 1140:
            if (y - 110) % 70 < 14 and 100 <= x < 100 + 120 + (y * 37 % 600):
                return (60, 64, 72)
            return (248, 248, 246)
        return (226, 228, 232)
    path.write_bytes(png(1200, 760, pixel))


def decode(data):
    """Minimal PNG decoder (8-bit RGB/RGBA, non-interlaced) for the checks."""
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour = struct.unpack('>IIBB', body[:10])
            assert depth == 8 and colour in (2, 6), (depth, colour)
            channels = 3 if colour == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    raw, stride, rows, prev = zlib.decompress(idat), width * channels, [], bytearray(width * channels)
    for y in range(height):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prev[i], prev[i - channels] if i >= channels else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line)); prev = line
    return width, height, channels, rows


class Driver:
    def __init__(self, cli, socket, out):
        self.cli, self.socket, self.out = cli, socket, out
        self.cancelled_editors = set()
        self.log = {'schemaVersion': 1, 'scenario': 'editor-tools-1362', 'ok': False, 'checks': {}, 'calls': []}

    def call(self, method, params=None, retry_starting=True):
        args = [self.cli] + (['--socket', self.socket] if self.socket else []) + ['--params-stdin', method]
        for _ in range(200):
            done = subprocess.run(args, input=json.dumps(params or {}), capture_output=True, text=True, timeout=60)
            reply = json.loads(done.stdout or '{}')
            error = reply.get('error', {})
            own_closing_editor = (method == 'editor.open' and error.get('code') == 'editor_active'
                                  and error.get('details', {}).get('operationId') in self.cancelled_editors)
            if retry_starting and not reply.get('ok') and (error.get('code') == 'editor_starting' or own_closing_editor):
                time.sleep(0.1); continue
            break
        if method == 'editor.cancel' and reply.get('ok'):
            self.cancelled_editors.add(params['id'])
        logged = json.loads(json.dumps(reply))
        if isinstance(logged.get('result'), dict) and 'png' in logged['result']:
            logged['result']['png'] = '<%d base64 chars>' % len(logged['result']['png'])
        self.log['calls'].append({'method': method, 'params': params, 'reply': logged})
        return reply

    def ok(self, method, params=None):
        reply = self.call(method, params)
        self.check(method + ' ok', reply.get('ok'), reply.get('error'))
        return reply['result']

    def check(self, name, ok, detail=None):
        self.log['checks'][name] = {'ok': bool(ok), **({'detail': detail} if detail is not None else {})}
        if not ok:
            raise RuntimeError('%s: %s' % (name, detail))

    def save(self):
        (self.out / 'log.json').write_text(json.dumps(self.log, indent=1))


def clipboard_png():
    env = os.environ
    attempts = []
    if env.get('WAYLAND_DISPLAY') and shutil.which('wl-paste'):
        attempts.append(['wl-paste', '--no-newline', '--type', 'image/png'])
    if env.get('DISPLAY') and shutil.which('xclip'):
        attempts.append(['xclip', '-selection', 'clipboard', '-t', 'image/png', '-o'])
    for command in attempts:
        try:
            done = subprocess.run(command, capture_output=True, timeout=5)
        except subprocess.TimeoutExpired:
            # GNOME may not serve an unfocused Wayland data-device reader;
            # let its XWayland clipboard bridge provide the independent read.
            continue
        if done.returncode == 0 and done.stdout.startswith(b'\x89PNG'):
            return done.stdout, command[0]
    return None, ' / '.join(c[0] for c in attempts) or 'no wl-paste or xclip'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True)
    parser.add_argument('--socket')
    parser.add_argument('--submit', action='store_true')
    parser.add_argument('--keep-open', action='store_true', help='leave the editor open for pictures')
    parser.add_argument('out')
    args = parser.parse_args()
    out = Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=True)
    d = Driver(args.cli, args.socket, out)
    source = out / 'synthetic-source.png'; synthetic(source)
    try:
        op = d.ok('editor.open', {'source': 'file', 'path': str(source)})['operationId']
        d.log['operationId'] = op
        state = d.ok('editor.inspect', {'id': op})
        d.check('opened at full size', (state['width'], state['height']) == (1200, 760), state.get('width'))
        add = lambda shape: {'op': 'add', 'shape': shape}
        commands = [
            add({'kind': 'redact', 'rect': list(TOKEN)}),
            add({'kind': 'step', 'at': [120, 140]}),
            add({'kind': 'step', 'at': [120, 280], 'color': '#facc15'}),
            add({'kind': 'step', 'at': [120, 420], 'color': '#3b82f6', 'size': 56}),
            add({'kind': 'pen', 'width': 4, 'color': '#dc2626', 'points': [[160 + 30 * i, 600 + (18 if i % 2 else -18)] for i in range(14)]}),
            add({'kind': 'ellipse', 'rect': [760, 120, 320, 180], 'width': 4}),
            add({'kind': 'line', 'from': [180, 520], 'to': [1000, 520], 'color': '#22c55e', 'width': 4}),
            add({'kind': 'blur', 'rect': [700, 360, 380, 140], 'style': 'pixelate', 'strength': 12}),
            add({'kind': 'redact', 'rect': list(OVERLAP_TOKEN)}),
            add({'kind': 'rectangle', 'rect': [940, 590, 80, 40], 'color': '#ff0000', 'width': 4}),
        ]
        created = d.ok('editor.apply', {'id': op, 'commands': commands})['created']
        shapes = {s['id']: s for s in d.ok('editor.inspect', {'id': op})['shapes']}
        kinds = [shapes[i]['kind'] for i in created]
        d.check('every new kind was created', kinds == ['redact', 'step', 'step', 'step', 'pen', 'ellipse', 'line', 'blur', 'redact', 'rectangle'], kinds)
        d.check('steps count up from 1', [shapes[i]['number'] for i in created[1:4]] == [1, 2, 3])
        d.check('pixelate style kept', shapes[created[7]]['style'] == 'pixelate')
        # The window's own controls.
        for tool in ('step', 'pen', 'ellipse', 'line'):
            view = d.ok('editor.view', {'id': op, 'action': 'tool', 'tool': tool})
            d.check('tool ' + tool, view['tool'] == tool)
        d.ok('editor.view', {'id': op, 'action': 'tool', 'tool': 'step'})
        view = d.ok('editor.view', {'id': op, 'action': 'inspect'})
        d.check('next number follows the highest step', view['nextStepNumber'] == 4, view['nextStepNumber'])
        d.ok('editor.view', {'id': op, 'action': 'stepNumber', 'number': 10})
        placed = d.ok('editor.view', {'id': op, 'action': 'place'})
        step = [s for s in d.ok('editor.inspect', {'id': op})['shapes'] if s['id'] == placed['selectedShapeId']][0]
        d.check('Next number sets the placed step', step['number'] == 10, step)
        count = len(d.ok('editor.inspect', {'id': op})['shapes'])
        d.ok('editor.apply', {'id': op, 'commands': [{'op': 'undo'}]})
        d.check('undo removes the placed step', len(d.ok('editor.inspect', {'id': op})['shapes']) == count - 1)
        d.ok('editor.apply', {'id': op, 'commands': [{'op': 'redo'}]})
        d.check('redo restores it', any(s['id'] == step['id'] and s['number'] == 10 for s in d.ok('editor.inspect', {'id': op})['shapes']))
        d.ok('editor.apply', {'id': op, 'commands': [{'op': 'remove', 'shapeId': step['id']}]})
        d.ok('editor.view', {'id': op, 'action': 'tool', 'tool': 'blur'})
        view = d.ok('editor.view', {'id': op, 'action': 'blurStyle', 'style': 'pixelate'})
        d.check('blur style pixelate', view['style']['blurStyle'] == 'pixelate')
        d.ok('editor.view', {'id': op, 'action': 'blurStyle', 'style': 'blur'})
        d.ok('editor.view', {'id': op, 'action': 'tool', 'tool': 'select'})
        preview = d.ok('editor.preview', {'id': op, 'maxEdge': 1200})
        (out / 'preview.png').write_bytes(base64.b64decode(preview['png']))
        # Copy image, then what the desktop clipboard holds.
        copied = d.ok('editor.copy', {'id': op})
        d.check('copied', copied.get('copied') is True)
        pasted, tool = clipboard_png()
        d.check('clipboard holds a PNG (' + tool + ')', pasted is not None, tool)
        (out / 'pasted.png').write_bytes(pasted)
        digest = hashlib.sha256(pasted).hexdigest()
        d.check('clipboard bytes are the copied PNG', digest == copied['sha256'], {'clipboard': digest, 'reply': copied['sha256']})
        width, height, channels, rows = decode(pasted)
        d.check('pasted size', (width, height) == (1200, 760), (width, height))
        leaks = [(x, y) for y in range(TOKEN[1], TOKEN[1] + TOKEN[3]) for x in range(TOKEN[0], TOKEN[0] + TOKEN[2])
                 if rows[y][x * channels:x * channels + 3] != b'\x00\x00\x00']
        d.check('redacted token is black in the paste', not leaks, leaks[:5])
        overlap = {rows[y][x * channels:x * channels + 3]
                   for y in range(OVERLAP_TOKEN[1], OVERLAP_TOKEN[1] + OVERLAP_TOKEN[3])
                   for x in range(OVERLAP_TOKEN[0], OVERLAP_TOKEN[0] + OVERLAP_TOKEN[2])}
        d.check('annotation over second redaction contains only black and its own red',
                overlap == {b'\x00\x00\x00', b'\xff\x00\x00'}, sorted(p.hex() for p in overlap))
        saved = d.ok('editor.saveCopy', {'id': op, 'path': str(out / 'saved-copy.png'), 'overwrite': True})
        d.check('Save a copy is the same PNG', saved['sha256'] == copied['sha256'])
        view = d.ok('editor.view', {'id': op, 'action': 'inspect'})
        d.check('window reports the copy', (view.get('lastCopy') or {}).get('status') == 'copied', view.get('lastCopy'))
        d.check('still editing, nothing handed off', d.ok('editor.inspect', {'id': op})['phase'] == 'editing')
        if args.submit:
            d.ok('editor.submit', {'id': op})
            for _ in range(600):
                operation = d.call('operation.inspect', {'id': op}, retry_starting=False)
                d.check('operation.inspect answers', operation.get('ok'), operation.get('error'))
                result = operation.get('result') or {}
                state = result.get('state') or (result.get('operation') or {}).get('state')
                if state in ('completed', 'failed', 'partial', 'cancelled'):
                    break
                time.sleep(0.2)
            d.log['operation'] = operation.get('result')
            d.check('upload completed', state == 'completed', state)
        elif not args.keep_open:
            d.ok('editor.cancel', {'id': op})
        if args.submit or not args.keep_open:
            # Step glyphs are frozen on creation using the helper's current font
            # environment. Compare geometry-only annotations across helpers so
            # font discovery cannot masquerade as a source-pixel difference.
            invariant_commands = [c for c in commands if c['shape']['kind'] != 'step']
            invariant_commands += [add({'kind': 'blur', 'rect': list(TOKEN), 'style': 'blur', 'strength': 12}),
                                   add({'kind': 'blur', 'rect': list(OVERLAP_TOKEN), 'style': 'pixelate', 'strength': 12})]
            variant = out / 'synthetic-variant.png'; synthetic(variant, variant=True)
            digests = []
            for label, path in (('baseline', source), ('variant', variant)):
                other = d.ok('editor.open', {'source': 'file', 'path': str(path)})['operationId']
                try:
                    d.ok('editor.apply', {'id': other, 'commands': invariant_commands})
                    result = d.ok('editor.saveCopy', {'id': other, 'path': str(out / (label + '-invariance.png')), 'overwrite': True})
                    digests.append(result['sha256'])
                finally:
                    d.ok('editor.cancel', {'id': other})
            d.check('sources differing only beneath redactions export identical bytes', digests[0] == digests[1], digests)
        d.log['ok'] = True
    finally:
        # A failed run leaves no editor open unless asked to.
        if not d.log['ok'] and not args.keep_open and d.log.get('operationId'):
            d.call('editor.cancel', {'id': d.log['operationId']}, retry_starting=False)
        d.save()
    print(json.dumps({'ok': d.log['ok'], 'operationId': d.log.get('operationId'), 'checks': len(d.log['checks'])}))


if __name__ == '__main__':
    sys.exit(main())
