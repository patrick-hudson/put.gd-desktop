#!/usr/bin/env python3
"""Sensitive-text fuzz harness (#1931): seeded scenes, the real helper and matcher, a scorer.

  fuzz.py build  --source TREE --out DIR                   compile the driver (g++, pkg-config Qt6)
  fuzz.py render --seed S --count N --out DIR [--start I] [--families a,b] [--jobs 4]
  fuzz.py run    --scenes DIR --driver BIN --helper BIN --build-json FILE --data DIR [--jobs 4] [--session ID]
  fuzz.py score  --scenes DIR [--results FILE] --out DIR

`render` writes DIR/images/NNNNN.png and DIR/labels.jsonl; the same seed, start and count on the
same host (same fonts) give the same images. `run` feeds every scene to the driver (the
package's own putgd-ocr, and quickRedact::find() and markBoxes() built from the package's
commit), sharded over at most --jobs processes, and stops launching work while
`desktop_lock.py guard` refuses (another session's quiet lease). `score` compares the marks
with the labels and writes findings.jsonl (every false positive and false negative with the
recognized text that caused it) and summary.json, grouped by kind, cause and tag.

Never commit rendered scenes or results: they hold secret-shaped strings. Run on builders,
never on the workstation.
"""
import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

ALL = ['email', 'phone', 'ip', 'card', 'key', 'name', 'list']
CHECK = ['card', 'key', 'list']  # the upload check's default kinds (all on warn: one pass)
TERMS = ['Falcon Ridge', 'ACME-4471']
DRIVER_SOURCES = ['editor/QuickRedact.cpp', 'editor/TextLayer.cpp', 'core/TextJob.cpp', 'core/TextJobProtocol.cpp', 'core/HelperFiles.cpp', 'ipc/UiControl.cpp']


# ----------------------------------------------------------------------------------- build
def build(args):
    src = Path(args.source).resolve() / 'src'
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    pc = lambda *a: subprocess.check_output(['pkg-config', *a, 'Qt6Core', 'Qt6Gui'], text=True).split()
    moc = Path(subprocess.check_output(['pkg-config', '--variable=libexecdir', 'Qt6Core'], text=True).strip()) / 'moc'
    if not moc.exists():
        moc = Path(subprocess.check_output(['pkg-config', '--variable=bindir', 'Qt6Core'], text=True).strip()) / 'moc'
    subprocess.check_call([str(moc), str(src / 'ipc/UiControl.h'), '-o', str(out / 'moc_UiControl.cpp')])
    units = [HERE / 'driver.cpp', out / 'moc_UiControl.cpp'] + [src / s for s in DRIVER_SOURCES]
    flags = ['-std=c++20', '-O1', '-fPIC', '-I' + str(src)] + pc('--cflags')
    objs = []
    procs = []
    for u in units:
        o = out / (u.stem + '.o')
        objs.append(o)
        procs.append(subprocess.Popen(['g++', *flags, '-c', str(u), '-o', str(o)]))
        while sum(p.poll() is None for p in procs) >= args.jobs:
            time.sleep(0.5)
    if any(p.wait() for p in procs):
        sys.exit('compile failed')
    subprocess.check_call(['g++', *map(str, objs), '-o', str(out / 'redact_fuzz_driver'), *pc('--libs')])
    print(out / 'redact_fuzz_driver')


# ---------------------------------------------------------------------------------- render
def make_scene(seed, idx, families=None):
    import canvas as CV
    import content as C
    import families as F
    R = random.Random(f'{seed}:{idx}')
    names = families or list(F.FAMILIES)
    weights = [F.FAMILIES[n][1] for n in names]
    tiled = R.random() < 0.12
    picks = [R.choices(names, weights)[0] for _ in range(2 if tiled else 1)]
    scale = R.choices([1.0, 1.25, 1.5, 2.0], [60, 10, 12, 18])[0]
    W = R.randint(900, 1500) if not tiled else R.randint(1400, 1900)
    H = R.randint(560, 1000)
    login, real = R.choice(C.USERS)
    ctx = dict(login=login, realName=real, host=R.choice(F.HOSTS), ui=CV.pick(R, CV.UI_SANS), mono=CV.pick(R, CV.MONO),
               ui_size=R.choice([11, 12, 13, 13, 14, 14, 15, 16]), mono_size=R.choice([11, 12, 13, 13, 14, 15, 16]),
               features='zero' if R.random() < 0.25 else None)
    aa = R.choices(['grey', 'subpixel', 'none'], [70, 22, 8])[0]
    hint = R.choice(['none', 'slight', 'slight', 'full'])
    meta = dict(id=f'{seed}-{idx:05d}', seed=seed, index=idx, families=picks, scale=scale, antialias=aa, hint=hint,
                ui=ctx['ui'], mono=ctx['mono'], ui_size=ctx['ui_size'], mono_size=ctx['mono_size'], login=login, realName=real, tiled=tiled, features=ctx['features'])
    full = CV.Canvas(W, H, '#000000', scale, aa, hint)
    variants, themes = [], []
    pane_w = W // len(picks)
    for k, name in enumerate(picks):
        if name in F.TERMINAL:
            tname = R.choice(['vscode-dark', 'breeze-dark', 'adwaita-dark', 'ubuntu-term', 'green-term', 'solarized-light', 'light', 'nord', 'dracula', 'adwaita', 'lowcontrast'])
        elif name in ('ide', 'configs', 'csv', 'codelike', 'lockfiles') and R.random() < 0.2:
            tname = 'dim-dark'
        elif name == 'chat':
            tname = R.choice(['slack-aubergine', 'discord', 'light', 'adwaita-dark', 'breeze-dark', 'adwaita'])
        else:
            tname = R.choice([t for t in F.THEMES if t not in ('slack-aubergine', 'discord')])
        th = F.THEMES[tname]
        themes.append(tname)
        cv = CV.Canvas(pane_w if tiled else W, H, th['bg'], scale, aa, hint)
        info = F.FAMILIES[name][0](R, cv, th, ctx) or {}
        variants.append(info.get('variant', ''))
        _post_marks(R, cv, th)
        ox = k * pane_w
        full.cr.save()
        full.cr.identity_matrix()
        full.cr.set_source_surface(cv.surface, ox * scale, 0)
        full.cr.paint()
        full.cr.restore()
        sx = ox * scale
        for w in cv.words:
            full.words.append([w[0], round(w[1] + sx), w[2], w[3], w[4]])
        for s in cv.spans:
            s = dict(s)
            s['pieces'] = [[p[0] + sx, p[1], p[2] + sx, p[3]] for p in s['pieces']]
            s['chars'] = [[c[0] + sx, c[1], c[2] + sx, c[3]] for c in s['chars']]
            full.spans.append(s)
        if tiled and k == 0:
            full.cr.save()
            full.cr.identity_matrix()
            full.cr.set_source_rgb(0.3, 0.3, 0.3)
            full.cr.rectangle(pane_w * scale - 1, 0, 2, H * scale)
            full.cr.fill()
            full.cr.restore()
    meta.update(variant='+'.join(variants), theme='+'.join(themes))
    return full, meta, R


def _post_marks(R, cv, th):
    """Overlays a real screen has on top of text: a caret beside a secret, a spell-check squiggle."""
    secrets = [s for s in cv.spans if s['label'] == 'secret']
    if not secrets:
        return
    sc = cv.scale
    if R.random() < 0.10:
        s = R.choice(secrets)
        x0, y0, x1, y1 = s['pieces'][-1] if R.random() < 0.7 else s['pieces'][0]
        at = (x1 + R.choice([0, 1, 2])) if R.random() < 0.7 else (x0 - R.choice([1, 2, 3]))
        cv.rect(at / sc, (y0 - 2) / sc, max(1, R.choice([1, 1.5, 2])), (y1 - y0 + 4) / sc, th['fg'])
        s['tag'] = (s['tag'] + '+caret').lstrip('+')
    if R.random() < 0.08:
        s = R.choice(secrets)
        x0, y0, x1, y1 = s['pieces'][0]
        pts = []
        x = x0
        up = True
        while x < x1:
            pts.append((x / sc, (y1 + (1 if up else 3)) / sc))
            x += 2 * sc
            up = not up
        if len(pts) > 1:
            cv.poly(pts, '#e01b24', 1.0)
            s['tag'] = (s['tag'] + '+squiggle').lstrip('+')


def render_one(job):
    seed, idx, out, families = job
    full, meta, R = make_scene(seed, idx, families)
    img = Path(out) / 'images' / f'{meta["id"]}.png'
    full.png(str(img))
    if R.random() < 0.08:  # shared as a JPEG
        from PIL import Image
        q = R.randint(55, 90)
        jpg = img.with_suffix('.jpg')
        Image.open(img).convert('RGB').save(jpg, quality=q)
        img.unlink()
        img = jpg
        meta['jpeg'] = q
    meta.update(image=str(img), width=full.surface.get_width(), height=full.surface.get_height(), spans=full.spans, words=full.words)
    return meta


def render(args):
    out = Path(args.out)
    (out / 'images').mkdir(parents=True, exist_ok=True)
    fams = args.families.split(',') if args.families else None
    jobs = [(args.seed, i, str(out), fams) for i in range(args.start, args.start + args.count)]
    with open(out / 'labels.jsonl', 'a') as fh, ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for meta in ex.map(render_one, jobs, chunksize=4):
            fh.write(json.dumps(meta, ensure_ascii=False) + '\n')
    import canvas as CV
    (out / 'fonts.json').write_text(json.dumps(sorted(CV.installed_families())))


# ------------------------------------------------------------------------------------- run
def guard_ok(session):
    lock = HERE.parent / 'desktop_lock.py'
    cmd = [sys.executable, str(lock), 'guard'] + (['--session', session] if session else [])
    return subprocess.run(cmd, capture_output=True).returncode == 0


def run(args):
    scenes = Path(args.scenes)
    labels = [json.loads(l) for l in open(scenes / 'labels.jsonl')]
    if args.families:
        keep = set(args.families.split(','))
        labels = [m for m in labels if keep & set(m['families'])]
    out = Path(args.out or scenes / 'results')
    out.mkdir(exist_ok=True)
    done = set()
    for f in out.glob('shard-*.jsonl'):
        for l in open(f):
            try:
                done.add(json.loads(l)['id'])
            except ValueError:
                pass
    todo = [m for m in labels if m['id'] not in done]
    print(f'{len(todo)} scenes to run, {len(done)} done', flush=True)
    batch = args.batch
    shard = len(list(out.glob('shard-*.jsonl')))
    while todo:
        if not guard_ok(args.session):
            print('guard refused: waiting for the quiet lease to clear', flush=True)
            time.sleep(60)
            continue
        procs = []
        for k in range(args.jobs):
            part = todo[k * batch:(k + 1) * batch]
            if not part:
                break
            lines = []
            for m in part:
                job = dict(id=m['id'], image=str(Path(m['image']).resolve()), login=m['login'], realName=m['realName'], terms=TERMS,
                           profiles={'all': ALL, 'check': CHECK}, words=m['words'], oracleOnly=args.oracle_only)
                lines.append(json.dumps(job, ensure_ascii=False))
            fh = open(out / f'shard-{shard:04d}.jsonl', 'w')
            shard += 1
            env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
            cmd = [args.driver, '--helper', args.helper, '--build-json', args.build_json] + sum((['--data', d] for d in args.data), [])
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=fh, stderr=subprocess.DEVNULL, env=env, text=True)
            p.stdin.write('\n'.join(lines) + '\n')
            p.stdin.close()
            procs.append((p, fh))
        for p, fh in procs:
            p.wait()
            fh.close()
        todo = todo[args.jobs * batch:]
        print(f'{len(todo)} left', flush=True)
    print('DONE', flush=True)


# ----------------------------------------------------------------------------------- score
def _covered(box, marks):
    x0, y0, x1, y1 = box
    if x1 <= x0 or y1 <= y0:
        return 1.0
    pts = [(x0 + (x1 - x0) * (i + 0.5) / 4, y0 + (y1 - y0) * (j + 0.5) / 4) for i in range(4) for j in range(4)]
    hit = sum(1 for (px, py) in pts if any(m[0] <= px < m[0] + m[2] and m[1] <= py < m[1] + m[3] for m in marks))
    return hit / len(pts)


def _overlap(a, b):
    """a: [x0,y0,x1,y1], b: [x,y,w,h]"""
    return a[0] < b[0] + b[2] and b[0] < a[2] and a[1] < b[1] + b[3] and b[1] < a[3]


def _marks(profile):
    out = []
    for i, m in enumerate(profile.get('matches', [])):
        for mark, words in m['marks']:
            out.append(dict(rect=mark, words=words, category=m['category'], text=m['text'], match=i))
    return out


IPV4 = re.compile(r'(?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)(?![\d.]*\d)')


def _by_design(meta, m, decoys):
    """A cover the rules intend whatever the content: any IP literal, and any form of the user's own name."""
    if m['category'] == 'ip' and not decoys and (IPV4.search(m['text']) or '::' in m['text'] or m['text'].count(':') >= 2):
        return True
    if m['category'] == 'name':
        text = re.sub(r'[^a-z0-9]', '', m['text'].lower())
        parts = [p for p in re.split(r'[^a-z0-9]+', (meta['realName'] + ' ' + meta['login']).lower()) if len(p) >= 3]
        return any(p in text for p in parts)
    return False


def score_scene(meta, res, profile):
    """Findings for one scene and one profile ('all' or 'check')."""
    findings = []
    want = set(ALL if profile == 'all' else CHECK)
    ocr = _marks(res.get('ocr', {}).get(profile, {}))
    orc = _marks(res.get('oracle', {}).get(profile, {}))
    ocr_rects = [m['rect'] for m in ocr]
    orc_rects = [m['rect'] for m in orc]
    words = res.get('words', [])
    base = dict(scene=meta['id'], family='+'.join(meta['families']), variant=meta.get('variant', ''), theme=meta.get('theme', ''), scale=meta['scale'],
                font=meta['mono'] + '|' + meta['ui'], size=f"{meta['mono_size']}|{meta['ui_size']}", profile=profile)
    W, H = meta.get('width', 1 << 30), meta.get('height', 1 << 30)
    for s in meta['spans']:
        if s['label'] != 'secret':
            continue
        # characters past the image's edge are not in the picture (scenes rendered before the canvas dropped them)
        kept = [c for c in s['chars'] if c[2] <= W and c[3] <= H]
        if len(kept) < len(s['chars']):
            continue  # cut by the image's edge: what is left is not a whole secret, and recognition rarely reads it
        s = dict(s, chars=kept)
        kind = s['kind']
        if kind not in want and kind != 'gap':
            continue
        cov = [_covered(c, ocr_rects) for c in s['chars']]
        vis = sum(1 for v in cov if v < 0.6)
        if vis == 0:
            continue
        ocov = [_covered(c, orc_rects) for c in s['chars']]
        ovis = sum(1 for v in ocov if v < 0.6)
        read = [w[0] for w in words if any(_overlap(p, w[1:5]) for p in s['pieces'])]
        near = [m['category'] + ':' + m['text'] for m in ocr if any(_overlap(p, m['rect']) for p in s['pieces'])]
        findings.append(dict(base, type='FN' if vis == len(cov) else 'PARTIAL', kind=kind, tag=s['tag'], text=s['text'], visible=vis, chars=len(cov),
                             cause='rule' if ovis else 'ocr', oracle_visible=ovis, read=' '.join(read), marks_near=near))
    # a match that covers a secret anywhere is a true finding; its other rows are over-cover
    def secret_rows(marks):
        good = set()
        for m in marks:
            r = m['rect']
            if any(s['label'] in ('secret', 'neutral') and any(_overlap(c, r) and _covered(c, [r]) >= 0.3 for c in s['chars']) for s in meta['spans']):
                good.add(m['match'])
        return good
    good = secret_rows(ocr)
    seen = {}
    for m in ocr:
        r = m['rect']
        box = [r[0], r[1], r[0] + r[2], r[1] + r[3]]
        hits = [s for s in meta['spans'] if any(_overlap(c, r) and _covered(c, [r]) >= 0.3 for c in s['chars'])]
        if any(s['label'] in ('secret', 'neutral') for s in hits):
            continue
        decoys = [s for s in hits if s['label'] == 'decoy']
        if _by_design(meta, m, decoys):
            if m['category'] == 'name':
                findings.append(dict(base, type='DESIGN', category='name', match=m['text'][:120], rows=1, decoy='name_part', decoy_text='', under='', cause='rule'))
            continue
        under = [w[0] for w in meta['words'] if _overlap(box, w[1:5]) and _covered([w[1], w[2], w[1] + w[3], w[2] + w[4]], [r]) >= 0.5]
        in_oracle = any(o['category'] == m['category'] and _overlap(box, o['rect']) for o in orc)
        kind = 'OVER' if m['match'] in good else 'FP'
        key = (kind, m['match'])
        if key in seen:  # one finding per match; count its rows
            seen[key]['rows'] += 1
            seen[key]['under'] = (seen[key]['under'] + ' / ' + ' '.join(under))[:240]
            continue
        f = dict(base, type=kind, category=m['category'], match=m['text'][:240], rows=1, decoy=','.join(sorted({s['kind'] or '?' for s in decoys})) or 'ordinary',
                 decoy_text=' | '.join(s['text'] for s in decoys)[:200], under=' '.join(under)[:200], cause='rule' if in_oracle else 'ocr')
        seen[key] = f
        findings.append(f)
    return findings


def score(args):
    scenes = Path(args.scenes)
    labels = {}
    for l in open(scenes / 'labels.jsonl'):
        m = json.loads(l)
        labels[m['id']] = m
    results = {}
    where = Path(args.results) if args.results else scenes / 'results'
    files = sorted(where.glob('shard-*.jsonl')) if where.is_dir() else [where]
    for f in files:
        for l in open(f):
            try:
                r = json.loads(l)
            except ValueError:
                continue
            if args.oracle:  # score the true words' run as if it were the recognized one (an --oracle-only run)
                r['ocr'] = r.get('oracle', {})
            results[r['id']] = r
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    findings = []
    errors = {}
    totals = {'scenes': 0, 'secrets': {}, 'marks': {}}
    for sid, meta in labels.items():
        r = results.get(sid)
        if not r:
            continue
        if r.get('error'):
            errors[r['error']] = errors.get(r['error'], 0) + 1
            continue
        totals['scenes'] += 1
        for s in meta['spans']:
            if s['label'] == 'secret':
                totals['secrets'][s['kind']] = totals['secrets'].get(s['kind'], 0) + 1
        for profile in ('all', 'check'):
            totals['marks'][profile] = totals['marks'].get(profile, 0) + len(_marks(r.get('ocr', {}).get(profile, {})))
            findings += score_scene(meta, r, profile)
    with open(out / 'findings.jsonl', 'w') as fh:
        for f in findings:
            fh.write(json.dumps(f, ensure_ascii=False) + '\n')
    groups = {}
    for f in findings:
        if f['type'] in ('FP', 'OVER', 'DESIGN'):
            key = (f['profile'], f['type'], f['category'], f['decoy'], f['cause'])
        else:
            key = (f['profile'], f['type'], f['kind'], f['tag'].split('+')[0] or '-', f['cause'])
        g = groups.setdefault(key, {'count': 0, 'scenes': set(), 'families': {}, 'examples': []})
        g['count'] += 1
        g['scenes'].add(f['scene'])
        g['families'][f['family']] = g['families'].get(f['family'], 0) + 1
        if len(g['examples']) < 6:
            g['examples'].append({k: f[k] for k in f if k in ('scene', 'text', 'read', 'match', 'under', 'decoy_text', 'visible', 'chars', 'marks_near', 'rows', 'font', 'size', 'scale')})
    summary = {'totals': totals, 'errors': errors, 'groups': [
        dict(profile=k[0], type=k[1], what=k[2], sub=k[3], cause=k[4], count=g['count'], scenes=len(g['scenes']), families=g['families'], examples=g['examples'])
        for k, g in sorted(groups.items(), key=lambda kv: -kv[1]['count'])]}
    (out / 'summary.json').write_text(json.dumps(summary, indent=1, ensure_ascii=False))
    print(json.dumps(totals))
    for g in summary['groups'][:args.top]:
        print(f"{g['profile']:5} {g['type']:7} {g['what']:6} {g['sub'][:28]:28} {g['cause']:4} {g['count']:5} in {g['scenes']:4} scenes  e.g. {json.dumps(g['examples'][0], ensure_ascii=False)[:160]}")


def show(args):
    """Print the findings that match every FIELD=VALUE filter (a value may be a prefix ending in *)."""
    filters = [f.split('=', 1) for f in args.filters]
    n = 0
    for line in open(args.findings):
        f = json.loads(line)
        if all(str(f.get(k, '')) == v or (v.endswith('*') and str(f.get(k, '')).startswith(v[:-1])) for k, v in filters):
            print(json.dumps(f, ensure_ascii=False))
            n += 1
            if n >= args.n:
                break


def probe(args):
    """Run text lines through find() as perfectly recognized words (8 px a character, 20 px a line) and print the matches:
    the quickest way to see what a rule does with a string."""
    lines = args.lines or [l.rstrip('\n') for l in sys.stdin]
    words = []
    for i, line in enumerate(lines):
        x = 0
        for part in re.split(r'( +)', line):
            if part and not part.isspace():
                words.append([part, 10 + x * 8, 10 + i * 20, len(part) * 8, 14])
            x += len(part)
    image = Path(args.image)
    if not image.exists():
        import cairo
        surface = cairo.ImageSurface(cairo.FORMAT_RGB24, 1400, 30 + 20 * len(lines))
        ctx = cairo.Context(surface)
        ctx.set_source_rgb(1, 1, 1)
        ctx.paint()
        surface.write_to_png(str(image))
    job = dict(id='probe', image=str(image.resolve()), login=args.login, realName=args.real_name, terms=TERMS, oracleOnly=True,
               profiles={'all': ALL, 'check': CHECK}, words=words)
    out = subprocess.run([args.driver, '--helper', '/bin/false', '--build-json', args.build_json], input=json.dumps(job) + '\n',
                         capture_output=True, text=True, env=dict(os.environ, QT_QPA_PLATFORM='offscreen')).stdout
    res = json.loads(out)
    for prof in ('all', 'check'):
        for m in res['oracle'][prof]['matches']:
            print(f"{prof:5} {m['category']:6} {m['text']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--source', required=True)
    b.add_argument('--out', required=True)
    b.add_argument('--jobs', type=int, default=4)
    r = sub.add_parser('render')
    r.add_argument('--seed', required=True)
    r.add_argument('--count', type=int, required=True)
    r.add_argument('--start', type=int, default=0)
    r.add_argument('--out', required=True)
    r.add_argument('--families')
    r.add_argument('--jobs', type=int, default=4)
    u = sub.add_parser('run')
    u.add_argument('--scenes', required=True)
    u.add_argument('--driver', required=True)
    u.add_argument('--helper', required=True)
    u.add_argument('--build-json', required=True)
    u.add_argument('--data', action='append', required=True)
    u.add_argument('--jobs', type=int, default=4)
    u.add_argument('--batch', type=int, default=10)
    u.add_argument('--session')
    u.add_argument('--out', help='results directory (default SCENES/results); give your own to reuse scenes read-only')
    u.add_argument('--families', help='run only scenes holding one of these families (comma-separated)')
    u.add_argument('--oracle-only', action='store_true', help='match the true words only: no recognition, seconds per thousand scenes')
    s = sub.add_parser('score')
    s.add_argument('--scenes', required=True)
    s.add_argument('--results', help='a results file or directory of shard-*.jsonl (default SCENES/results)')
    s.add_argument('--oracle', action='store_true', help='score the true-word run (after run --oracle-only)')
    s.add_argument('--out', required=True)
    s.add_argument('--top', type=int, default=40)
    q = sub.add_parser('probe')
    q.add_argument('lines', nargs='*')
    q.add_argument('--driver', required=True)
    q.add_argument('--build-json', required=True)
    q.add_argument('--image', default='/tmp/redact-probe-blank.png')
    q.add_argument('--login', default='jdoe')
    q.add_argument('--real-name', default='Jane Doe')
    w = sub.add_parser('show')
    w.add_argument('findings')
    w.add_argument('filters', nargs='*')
    w.add_argument('--n', type=int, default=5)
    a = ap.parse_args()
    if a.jobs if hasattr(a, 'jobs') else 0:
        a.jobs = min(a.jobs, 4)  # at most 4 jobs on a shared builder
    {'build': build, 'render': render, 'run': run, 'score': score, 'show': show, 'probe': probe}[a.cmd](a)


if __name__ == '__main__':
    main()
