#!/usr/bin/env python3
"""#418 checks on a started afa.py run, beside afa.py. One JSON document per command.

  qt418.py clipfile NAME SAMPLE [SAMPLE ...] [--label L]
      Each sample copied as a file (text/uri-list) and uploaded with Upload clipboard, as a user would upload a
      file they didn't make; then history.preview (the resident's image worker decodes it) and a real click on
      its History card (putgd-ui reads the original). Records the PIDs before and after and a frame per sample.

  qt418.py badfile NAME SAMPLE [SAMPLE ...] [--label L]
      The newest image entries of the run's history get the samples' bytes in place of their own
      (the saved local copy and every artifact file with the same content), the newest saved PNG first. Then:
      cache.clear and history.preview per entry (the resident's image worker decodes it), and
      History opened with the first entry selected; Down moves through the rest (putgd-ui reads
      each original with QImageReader). Records whether the resident, putgd-ui and the image
      worker survived, their PIDs before and after, and a full-desktop frame per row when allowed.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import afa  # noqa: E402
from afa import Run, out, iso, tools, park, fullshot  # noqa: E402


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pids(run):
    tree = afa.tree(run)
    return {name: sorted(p['pid'] for p in tree if p['comm'] == name) for name in ('putgd', 'putgd-ui')}


def cards(frame, raised):
    """History's capture cards, top to bottom, found by their thumbnail squares: blocks of 40 to 90 rows whose
    longest span of the theme's surfaceRaised colour is 50 to 100 px. Returns each thumbnail's (x0, y0, x1, y1)."""
    w, h, px = frame
    target = bytes((raised[2], raised[1], raised[0]))
    rows = []
    for y in range(h):
        line = px[y * w * 4:(y + 1) * w * 4]
        # The thumbnail's icon interrupts the colour, so a span may have gaps of up to 40 px.
        best, start, last = (0, 0, 0), None, None
        for x in range(w):
            if line[x * 4:x * 4 + 3] == target:
                if start is None or x - last > 40: start = x
                last = x
                if x + 1 - start > best[0]: best = (x + 1 - start, start, x + 1)
        rows.append(best if 50 <= best[0] <= 100 else None)
    blocks, y = [], 0
    while y < h:
        if rows[y]:
            top = y
            while y < h and rows[y] and abs(rows[y][1] - rows[top][1]) <= 2: y += 1
            if 40 <= y - top <= 90:
                blocks.append((rows[top][1], top, rows[top][2], y))
        else:
            y += 1
    return blocks


def badfile(args):
    run = Run(args.name)
    samples = [Path(s).resolve() for s in args.samples]
    listed = run.ok('history.list', {'limit': 50, 'offset': 0})['records']
    # The newest saved PNG captures, one per sample, with their row in History (newest first, as listed).
    images = [(row, r) for row, r in enumerate(listed) if r['mime'] == 'image/png' and (r.get('local') or {}).get('status') == 'saved'
              and Path((r.get('local') or {}).get('path') or '/nonexistent').is_file()][:len(samples)]
    if len(images) < len(samples):
        raise SystemExit('fewer saved PNG captures in history than samples')
    roots = [run.dir, afa.DATA_HOME / 'putgd']
    rep = {'at': iso(), 'before': pids(run), 'entries': []}
    for sample, (row, record) in zip(samples, images):
        local = Path(record['local']['path'])
        original = digest(local)
        # Every file of this entry with the capture's own bytes: the saved copy and any artifact or spool copy.
        copies = sorted({str(p) for root in roots if root.exists() for p in root.rglob('*')
                         if p.is_file() and not p.is_symlink() and p.stat().st_size == local.stat().st_size and digest(p) == original})
        for path in copies:
            # Artifacts are stored read-only; the swap stands in for a file the user didn't make.
            mode = os.stat(path).st_mode & 0o7777
            os.chmod(path, mode | 0o200)
            shutil.copyfile(sample, path)
            os.chmod(path, mode)
        rep['entries'].append({'id': record['id'], 'row': row, 'name': record['name'], 'sample': sample.name, 'sampleSha256': digest(sample),
                               'replaced': copies})
    rep['cacheClear'] = run.cli('cache.clear', {})
    for entry in rep['entries']:
        reply = run.cli('history.preview', {'id': entry['id']}, timeout=90)
        result = reply.get('result') or {}
        entry['preview'] = {'ok': reply.get('ok'), 'state': result.get('state'), 'error': reply.get('error'),
                            'previewPath': result.get('previewPath')}
        entry['afterPreview'] = pids(run)
    frames, inp, size = tools(run)
    park(run, inp, size)
    run.ok('ui.show', {'view': 'history'}); time.sleep(3); park(run, inp, size)
    # Select the newest card with a real click; the list then has focus and Down moves the selection.
    tokens = run.ok('ui.viewState')['appearance']['tokens']
    raised = tuple(int(tokens['surfaceRaised'][i:i + 2], 16) for i in (1, 3, 5))
    found = cards(frames.grab(), raised)
    rep['cards'] = found
    if not found: raise SystemExit('no History card found on screen')
    x0, y0, x1, y1 = found[0]
    inp.click(x1 + 60, (y0 + y1) // 2)
    # History opens on the newest row; Down moves the selection one row at a time.
    current = 0
    for index, entry in enumerate(rep['entries']):
        time.sleep(3)
        for _ in range(entry['row'] - current):
            inp.key('Down'); time.sleep(.4)
        current = entry['row']
        time.sleep(4); park(run, inp, size)
        entry['frame'] = fullshot(run, frames.grab(), f'{args.label}-{index}')
        entry['afterUi'] = pids(run)
    time.sleep(2)
    rep['after'] = pids(run)
    rep['diagnostics'] = {k: v for k, v in run.ok('diagnostics').items() if k in ('build', 'qtVersion', 'journalReady')}
    rep['crashLogLines'] = [l for l in run.logs() if any(word in json.dumps(l).lower() for word in ('crash', 'signal', 'image_job', 'imagejob', 'worker'))][-20:]
    rep['survived'] = rep['after'] == rep['before'] and all(p for p in rep['after'].values())
    out(run.record('badfile', rep))


def set_clipboard(run, path):
    """Own the clipboard with path's file URI: xclip on X11, wl-copy, else setclip.py's GTK 4 window. Returns the owner."""
    env = run.env(private=False)
    if env.get('XDG_SESSION_TYPE') == 'x11':
        uri = Path(path).resolve().as_uri() + '\r\n'
        owner = subprocess.Popen(['xclip', '-quiet', '-selection', 'clipboard', '-t', 'text/uri-list'], env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        owner.stdin.write(uri.encode()); owner.stdin.close(); time.sleep(1)
        return owner
    if shutil.which('wl-copy'):
        uri = Path(path).resolve().as_uri() + '\r\n'
        owner = subprocess.Popen(['wl-copy', '--foreground', '--type', 'text/uri-list'], env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        owner.stdin.write(uri.encode()); owner.stdin.close(); time.sleep(1)
        return owner
    owner = subprocess.Popen([sys.executable, str(HERE / 'setclip.py'), str(path)], env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, start_new_session=True)
    ready = owner.stdout.readline().strip()
    if ready != 'ready': owner.kill(); raise SystemExit('clipboard helper did not take the clipboard')
    return owner


def clipfile(args):
    """Each sample as a file the user copies and uploads with Upload clipboard, then previewed and opened in History."""
    run = Run(args.name)
    frames, inp, size = tools(run)
    # The theme tokens come from putgd-ui's checkpoint, so start it first.
    run.ok('ui.show', {'view': 'history'}); time.sleep(3)
    tokens = run.ok('ui.viewState')['appearance']['tokens']
    raised = tuple(int(tokens['surfaceRaised'][i:i + 2], 16) for i in (1, 3, 5))
    rep = {'at': iso(), 'before': pids(run), 'entries': []}
    for index, sample in enumerate(Path(s).resolve() for s in args.samples):
        copy = run.dir / 'working' / sample.name
        shutil.copyfile(sample, copy)
        entry = {'sample': sample.name, 'sha256': digest(sample), 'bytes': sample.stat().st_size}
        owner = set_clipboard(run, copy)
        entry['clipboardOwnerPid'] = owner.pid
        try:
            reply = run.ok('action.invoke', {'action': 'clipboard'})
            op = afa.operation_done(run, reply['operationId'])
        finally:
            owner.kill(); owner.wait(10)
        entry['operation'] = afa.summarize_op(op or {'id': reply['operationId'], 'state': 'timeout'})
        listed = run.ok('history.list', {'limit': 20, 'offset': 0})['records']
        record = next((r for r in listed if r.get('operationId') == reply['operationId']), None)
        entry['history'] = record and {'id': record['id'], 'name': record['name'], 'mime': record['mime'], 'uploadStatus': record.get('uploadStatus'),
                                       'local': (record.get('local') or {}).get('status'), 'newest': listed[0]['id'] == record['id']}
        if record:
            preview = run.cli('history.preview', {'id': record['id']}, timeout=90)
            result = preview.get('result') or {}
            entry['preview'] = {'ok': preview.get('ok'), 'state': result.get('state'), 'error': preview.get('error'),
                                'previewExists': bool(result.get('previewPath')) and Path(result['previewPath']).exists()}
        entry['afterPreview'] = pids(run)
        # The newest card is this upload. A real click on any card gives the list focus; Home then selects the
        # newest and scrolls to it, which opens its detail (putgd-ui reads the original's size).
        park(run, inp, size)
        run.ok('ui.show', {'view': 'history'}); time.sleep(3); park(run, inp, size)
        found = cards(frames.grab(), raised)
        entry['cards'] = len(found)
        if found:
            x0, y0, x1, y1 = found[0]
            inp.click(x1 + 60, (y0 + y1) // 2); time.sleep(1.5)
            inp.key('Home')
            time.sleep(4); park(run, inp, size)
            entry['frame'] = fullshot(run, frames.grab(), f'{args.label}-{index}')
        entry['afterUi'] = pids(run)
        rep['entries'].append(entry)
    time.sleep(2)
    rep['after'] = pids(run)
    rep['diagnostics'] = {k: v for k, v in run.ok('diagnostics').items() if k in ('build', 'qtVersion', 'journalReady')}
    rep['survived'] = rep['after'] == rep['before'] and all(p for p in rep['after'].values())
    out(run.record('clipfile', rep))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('badfile'); a.add_argument('name'); a.add_argument('samples', nargs='+'); a.add_argument('--label', default='badfile')
    a = sub.add_parser('clipfile'); a.add_argument('name'); a.add_argument('samples', nargs='+'); a.add_argument('--label', default='clipfile')
    args = p.parse_args()
    {'badfile': badfile, 'clipfile': clipfile}[args.cmd](args)


if __name__ == '__main__':
    main()
