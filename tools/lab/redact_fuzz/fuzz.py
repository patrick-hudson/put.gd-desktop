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
    meta = dict(id=f'{safe_name(seed)}-{idx:05d}', seed=seed, index=idx, families=picks, scale=scale, antialias=aa, hint=hint,
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


def safe_name(seed):
    """The seed as it may appear in a file name: letters, digits, '.', '_' and '-' only, never a path. A seed that
    needed changing gets a short hash of itself, so two seeds never share a name."""
    seed = str(seed)
    safe = re.sub(r'[^A-Za-z0-9_.-]', '_', seed).lstrip('.') or '_'
    if safe != seed:
        import hashlib
        safe += '-' + hashlib.sha1(seed.encode()).hexdigest()[:8]
    return safe


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
    out = Path(out)
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
    meta.update(image=str(img.relative_to(out)), width=full.surface.get_width(), height=full.surface.get_height(), spans=full.spans, words=full.words)
    return meta


def render(args):
    out = Path(args.out)
    if (out / 'labels.jsonl').exists():  # checked before anything is written
        have = {json.loads(l)['id'] for l in open(out / 'labels.jsonl') if l.strip()}
        clash = [f'{safe_name(args.seed)}-{i:05d}' for i in range(args.start, args.start + args.count) if f'{safe_name(args.seed)}-{i:05d}' in have]
        if clash:
            sys.exit(f'{out} already holds scene {clash[0]} ({len(clash)} in all): use another seed, --start or --out')
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


def scene_image(scenes, meta):
    """A scene's image: recorded relative to the corpus; older corpora recorded an absolute path, which may have moved."""
    image = Path(meta['image'])
    for candidate in ((scenes / image) if not image.is_absolute() else image, Path.cwd() / image):
        if candidate.exists():
            return candidate.resolve()
    return (scenes / 'images' / image.name).resolve()


def new_shard(out):
    """Open the next unused shard file; never truncate one another run (or a gap in the numbering) already holds."""
    k = len(list(out.glob('shard-*.jsonl')))
    while True:
        try:
            return open(out / f'shard-{k:04d}.jsonl', 'x')
        except FileExistsError:
            k += 1


def whole(r, oracle_only):
    """A row that holds what its run is for: both profiles of the oracle (an --oracle-only run, whatever recognition
    did), or both profiles of the recognized and true words with no error. `run` and `score` share this rule."""
    profiles = ('all', 'check')
    if oracle_only:
        return all(p in r.get('oracle', {}) for p in profiles)
    return not r.get('error') and all(p in r.get('ocr', {}) and p in r.get('oracle', {}) for p in profiles)


def completed(out, oracle_only, retries):
    """(ok, exhausted): scene ids with a result holding the run's own output (recognized words, or the oracle alone),
    and those that failed more than `retries` times without one. Any other failed or partial row is run again."""
    ok, failures = set(), {}
    for f in out.glob('shard-*.jsonl'):
        with open(f) as fh:
            rows = fh.readlines()
        for l in rows:
            try:
                r = json.loads(l)
            except ValueError:
                continue
            if whole(r, oracle_only):
                ok.add(r['id'])
            else:
                failures[r.get('id')] = failures.get(r.get('id'), 0) + 1
    return ok, {i for i, n in failures.items() if n > retries and i not in ok}


def run(args):
    scenes = Path(args.scenes)
    labels = [json.loads(l) for l in open(scenes / 'labels.jsonl')]
    if args.families:
        keep = set(args.families.split(','))
        labels = [m for m in labels if keep & set(m['families'])]
    out = Path(args.out or scenes / 'results')
    out.mkdir(exist_ok=True)
    ok, gave_up = completed(out, args.oracle_only, args.retries)
    todo = [m for m in labels if m['id'] not in ok | gave_up]
    print(f'{len(todo)} scenes to run, {len(labels) - len(todo)} done or given up', flush=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    cmd = [args.driver, '--helper', args.helper, '--build-json', args.build_json] + sum((['--data', d] for d in args.data), [])
    for attempt in range(args.retries + 1):
        while todo:
            procs = []
            for _ in range(args.jobs):
                if not todo:
                    break
                # Checked before every driver: one still busy with its batch may have let another session take the lease.
                if not guard_ok(args.session):
                    break
                part, todo = todo[:args.batch], todo[args.batch:]
                lines = []
                for m in part:
                    job = dict(id=m['id'], image=str(scene_image(scenes, m)), login=m['login'], realName=m['realName'], terms=TERMS,
                               profiles={'all': ALL, 'check': CHECK}, words=m['words'], oracleOnly=args.oracle_only)
                    lines.append(json.dumps(job, ensure_ascii=False))
                fh = new_shard(out)
                p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=fh, stderr=subprocess.DEVNULL, env=env, text=True)
                try:
                    p.stdin.write('\n'.join(lines) + '\n')
                    p.stdin.close()
                except BrokenPipeError:
                    pass  # the driver died; its scenes are missing and are run again below
                procs.append((p, fh))
            if not procs:
                print('guard refused: waiting for the quiet lease to clear', flush=True)
                time.sleep(60)
                continue
            for p, fh in procs:
                p.wait()
                fh.close()
            print(f'{len(todo)} left', flush=True)
        ok, gave_up = completed(out, args.oracle_only, args.retries)
        todo = [m for m in labels if m['id'] not in ok | gave_up]
        if not todo:
            break
        print(f'{len(todo)} scenes have no result yet: running them again', flush=True)
    missing = [m['id'] for m in labels if m['id'] not in ok]
    if missing:
        print(f'INCOMPLETE: {len(missing)} scenes have no result (first: {missing[0]})', flush=True)
        sys.exit(1)
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
def _ip_literal(word):
    """True when the word holds a valid IPv4 or IPv6 address (with a port, a prefix or a zone allowed)."""
    import ipaddress
    for candidate in IPV4.findall(word):
        try:
            ipaddress.IPv4Address(candidate)  # rejects leading zeros such as 01.2.3.4
            return True
        except ValueError:
            pass
    w = word.strip('[](),;"\'')
    w = re.sub(r'\]:\d+$', '', w)
    w = w.split('%')[0].split('/')[0]
    if w.count(':') >= 2:
        try:
            ipaddress.IPv6Address(w)
            return True
        except ValueError:
            return False
    return False


def _by_design(meta, category, under, decoys):
    """A cover the rules intend whatever its look: any IP address, and any form of the user's own name. Judged on
    the true words under the cover, never on what recognition read."""
    text = ' '.join(under)
    if category == 'ip' and not decoys:
        return any(_ip_literal(w) for w in under)
    if category == 'name':
        flat = re.sub(r'[^a-z0-9]', '', text.lower())
        parts = [p for p in re.split(r'[^a-z0-9]+', (meta['realName'] + ' ' + meta['login']).lower()) if len(p) >= 3]
        return any(p in flat for p in parts)
    return False


def _box(w):
    return [w[1], w[2], w[1] + w[3], w[2] + w[4]]


def scoreable(meta, span):
    """A secret wholly in the picture: not partly hidden, and no character past the image's edge."""
    W, H = meta.get('width', 1 << 30), meta.get('height', 1 << 30)
    return not span.get('clipped') and all(c[2] <= W and c[3] <= H for c in span['chars'])


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
    for s in meta['spans']:
        # partly hidden, or cut by the image's edge: not a whole secret on screen, and recognition rarely reads it
        if s['label'] != 'secret' or not scoreable(meta, s):
            continue
        kind = s['kind']
        if kind not in want and kind != 'gap':
            continue
        shown = {i for i, c in enumerate(s['chars']) if _covered(c, ocr_rects) < 0.6}
        if not shown:
            continue
        oshown = {i for i, c in enumerate(s['chars']) if _covered(c, orc_rects) < 0.6}
        # The rules are at fault for the characters the true words leave visible too; recognition for the rest.
        by_rule = len(shown & oshown)
        read = [w[0] for w in words if any(_overlap(p, w[1:5]) for p in s['pieces'])]
        near = [m['category'] + ':' + m['text'] for m in ocr if any(_overlap(p, m['rect']) for p in s['pieces'])]
        findings.append(dict(base, type='FN' if len(shown) == len(s['chars']) else 'PARTIAL', kind=kind, tag=s['tag'], text=s['text'],
                             visible=len(shown), chars=len(s['chars']), cause='rule' if 2 * by_rule >= len(shown) else 'ocr',
                             by_rule=by_rule, oracle_visible=len(oshown), read=' '.join(read), marks_near=near))
    # a match that covers a secret anywhere is a true finding; its other rows are over-cover
    good = {m['match'] for m in ocr if any(s['label'] in ('secret', 'neutral') and any(_overlap(c, m['rect']) and _covered(c, [m['rect']]) >= 0.3
                                                                                     for c in s['chars']) for s in meta['spans'])}
    seen = {}
    for m in ocr:
        r = m['rect']
        box = [r[0], r[1], r[0] + r[2], r[1] + r[3]]
        hits = [s for s in meta['spans'] if any(_overlap(c, r) and _covered(c, [r]) >= 0.3 for c in s['chars'])]
        if any(s['label'] in ('secret', 'neutral') for s in hits):
            continue
        decoys = [s for s in hits if s['label'] == 'decoy']
        under_words = [w for w in meta['words'] if _overlap(box, w[1:5]) and _covered(_box(w), [r]) >= 0.5]
        under = [w[0] for w in under_words]
        design = _by_design(meta, m['category'], under, decoys)
        # The same mistake on the true words: the oracle's marks of this category cover the same true words (or, with
        # no word under the cover, most of the cover itself).
        same = [o['rect'] for o in orc if o['category'] == m['category']]
        if under_words:
            rule = 2 * sum(1 for w in under_words if _covered(_box(w), same) >= 0.5) >= len(under_words)
        else:
            rule = _covered(box, same) >= 0.5
        kind = 'DESIGN' if design else 'OVER' if m['match'] in good else 'FP'
        key = (kind, m['match'])
        if key in seen:  # one finding per match; count its rows, and the rows the oracle shares
            f = seen[key]
            f['rows'] += 1
            f['rule_rows'] += int(rule)
            f['cause'] = 'rule' if 2 * f['rule_rows'] >= f['rows'] else 'ocr'
            f['under'] = (f['under'] + ' / ' + ' '.join(under))[:240]
            continue
        f = dict(base, type=kind, category=m['category'], match=m['text'][:240], rows=1, rule_rows=int(rule),
                 decoy=('ip_address' if m['category'] == 'ip' else 'name_part') if design else ','.join(sorted({s['kind'] or '?' for s in decoys})) or 'ordinary',
                 decoy_text=' | '.join(s['text'] for s in decoys)[:200], under=' '.join(under)[:200], cause='rule' if rule else 'ocr')
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
            complete = whole(r, args.oracle)
            if args.oracle:  # score the true words' run as if it were the recognized one (an --oracle-only run)
                r = dict(r, ocr=r.get('oracle', {}), error=None)
            r['_whole'] = complete
            if complete or r['id'] not in results:  # a retry's complete row is never hidden by a failed duplicate
                results[r['id']] = r
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    findings = []
    errors = {}
    totals = {'scenes': 0, 'missing': 0, 'incomplete': 0, 'secrets': {}, 'marks': {}}
    for sid, meta in labels.items():
        r = results.get(sid)
        if not r:
            totals['missing'] += 1  # no result at all: counted, never scored as zero matches
            continue
        if r.get('error'):
            errors[r['error']] = errors.get(r['error'], 0) + 1
            continue
        if not r['_whole']:
            totals['incomplete'] += 1  # a profile is missing (an oracle-only row scored without --oracle, or a cut row)
            continue
        totals['scenes'] += 1
        for s in meta['spans']:
            if s['label'] == 'secret' and scoreable(meta, s):
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
    if totals['missing'] or totals['incomplete'] or errors:
        print(f"NOT SCORED: {totals['missing']} scenes with no result, {totals['incomplete']} incomplete, errors {errors}")
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


def blank_png(width, height):
    """A white RGB PNG, from the standard library alone (a toolbox may have no Cairo for Python)."""
    import struct
    import zlib
    chunk = lambda kind, data: struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = (b'\x00' + b'\xff' * (3 * width)) * height
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def probe(args):
    """Run text lines through find() as perfectly recognized words (8 px a character, 20 px a line) and print the matches:
    the quickest way to see what a rule does with a string."""
    lines = args.lines or [l.rstrip('\n') for l in sys.stdin]
    words = []
    for i, line in enumerate(lines):
        x = 0
        for part in re.split(r'(\s+)', line):  # any whitespace ends a word, as on the canvas
            if part and not part.isspace():
                words.append([part, 10 + x * 8, 10 + i * 20, len(part) * 8, 14])
            x += len(part)
    import tempfile
    # tmpfs-ok: one small PNG, removed on exit; the standalone copy of redact_fuzz/ has no scratch.py
    with tempfile.TemporaryDirectory(prefix='redact-probe-') as private:
        image = Path(private) / 'blank.png'
        image.write_bytes(blank_png(max([20 + 8 * len(line) for line in lines] + [200]), 30 + 20 * len(lines)))
        job = dict(id='probe', image=str(image.resolve()), login=args.login, realName=args.real_name, terms=TERMS, oracleOnly=True,
                   profiles={'all': ALL, 'check': CHECK}, words=words)
        out = subprocess.run([args.driver, '--helper', '/bin/false', '--build-json', args.build_json], input=json.dumps(job) + '\n',
                             capture_output=True, text=True, env=dict(os.environ, QT_QPA_PLATFORM='offscreen')).stdout
    res = json.loads(out)
    for prof in ('all', 'check'):
        for m in res['oracle'][prof]['matches']:
            print(f"{prof:5} {m['category']:6} {m['text']}")


def positive(text):
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError('must be 1 or more')
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--source', required=True)
    b.add_argument('--out', required=True)
    b.add_argument('--jobs', type=positive, default=4)
    r = sub.add_parser('render')
    r.add_argument('--seed', required=True)
    r.add_argument('--count', type=int, required=True)
    r.add_argument('--start', type=int, default=0)
    r.add_argument('--out', required=True)
    r.add_argument('--families')
    r.add_argument('--jobs', type=positive, default=4)
    u = sub.add_parser('run')
    u.add_argument('--scenes', required=True)
    u.add_argument('--driver', required=True)
    u.add_argument('--helper', required=True)
    u.add_argument('--build-json', required=True)
    u.add_argument('--data', action='append', required=True)
    u.add_argument('--jobs', type=positive, default=4)
    u.add_argument('--batch', type=positive, default=10)
    u.add_argument('--retries', type=int, default=1, help='times a scene with no result is run again (default 1)')
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
