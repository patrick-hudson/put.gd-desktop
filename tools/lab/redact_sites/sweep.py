#!/usr/bin/env python3
"""Real-website sweep for the sensitive-text check (#1975): how often everyday public pages raise a prompt.

  sweep.py capture --sites sites.tsv --out DIR [--browsers chromium,firefox] [--scales 1,1.25,1.5] [--jobs 4]
  sweep.py run     --shots DIR --driver BIN --helper BIN --build-json FILE --data DIR --out RESULTS [--jobs 4] [--session ID]
  sweep.py replay  --shots DIR --words RESULTS --driver BIN --build-json FILE --out RESULTS
  sweep.py report  --shots DIR --results RESULTS [--shots DIR --results RESULTS ...] [--triage triage.tsv] --out DIR
  sweep.py compare REPORT_DIR REPORT_DIR

`capture` loads each page of sites.tsv in Playwright's Chromium and Firefox (a 1440x900 window at each
device scale, light, then dark when the page changes with prefers-color-scheme) and writes
DIR/shots/*.png and DIR/shots.jsonl. It needs the `playwright` package and its browsers
(`python -m playwright install chromium firefox`); run it with that interpreter.

`run` feeds every capture to tools/lab/redact_fuzz's driver: the package's own putgd-ocr, then
quickRedact::find() built from the tree under test, once with the upload check's default kinds and
once with every kind. `replay` reruns only the matcher on the words a previous `run` recognized
(no OCR: seconds instead of minutes), for iterating on a rule. Both stop launching work while
`desktop_lock.py guard` refuses (another session's quiet lease).

`report` lists every match with its page, browser, scale and theme, applies the triage rules
(triage.tsv: kind, regular expression on the matched text or "url=RX text=RX", verdict true/false, group, note; first
match wins). A capture the check couldn't finish (recognition failed or cut, matching unfinished)
counts as a false prompt of its own group, since the check asks then too. It writes matches.tsv, summary.json and table.md: pages with a false prompt per
profile and scale, and false matches per group. Untriaged matches count as false until a rule
says otherwise. `compare` prints two reports' tables side by side.

Captures and results stay out of git (they hold page text); run on builders, never on the
workstation.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAB = HERE.parent
ALL = ['email', 'phone', 'ip', 'card', 'key', 'name', 'list']
DEFAULT = ['card', 'key', 'list']  # the upload check's default kinds
PROFILES = {'default': DEFAULT, 'all': ALL}
VIEW = (1440, 900)
UA = {'chromium': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36'}


def read_sites(path, only=None):
    sites = []
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        category, url = line.split('\t')[:2]
        sites.append(dict(index=len(sites), category=category, url=url))
    if only:
        sites = [s for s in sites if re.search(only, s['category'] + ' ' + s['url'])]
    return sites


def guard_ok(session):
    """False while another session holds this host's quiet lease (checked on the builder itself)."""
    cmd = [sys.executable, str(LAB / 'desktop_lock.py'), 'guard'] + (['--session', session] if session else [])
    return subprocess.run(cmd, capture_output=True).returncode == 0


# --------------------------------------------------------------------------------- capture
def _differs(a, b):
    """Mean absolute difference of two same-size captures, 0-255 (a dark theme changes it a lot)."""
    from PIL import Image, ImageChops, ImageStat
    x, y = Image.open(a).convert('L'), Image.open(b).convert('L')
    if x.size != y.size:
        return 255.0
    return ImageStat.Stat(ImageChops.difference(x, y)).mean[0]


def _capture_worker(job):
    from playwright.sync_api import sync_playwright
    browser_name, sites, out, scales, dark, settle = job
    shots = Path(out) / 'shots'
    rows = []
    log = open(Path(out) / f'shots-{browser_name}-{os.getpid()}.part', 'a')
    with sync_playwright() as p:
        # Chromium: the full browser in its new headless mode, not the stripped headless shell.
        browser = getattr(p, browser_name).launch(headless=True, **({'channel': 'chromium'} if browser_name == 'chromium' else {}))
        for site in sites:
            for scale in scales:
                light = None
                for theme in (['light', 'dark'] if dark else ['light']):
                    sid = f'{site["index"]:03d}-{browser_name[:2]}-{int(scale * 100)}-{theme[0]}'
                    png = shots / f'{sid}.png'
                    row = dict(id=sid, url=site['url'], category=site['category'], browser=browser_name, scale=scale, theme=theme)
                    if png.exists():
                        row.update(png=str(png.relative_to(out)), status='cached')
                        rows.append(row)
                        if theme == 'light':
                            light = png
                        continue
                    ctx = None
                    try:
                        opts = dict(viewport=dict(width=VIEW[0], height=VIEW[1]), device_scale_factor=scale, color_scheme=theme,
                                    locale='en-US', timezone_id='America/New_York')
                        if browser_name in UA:
                            opts['user_agent'] = UA[browser_name]
                        ctx = browser.new_context(**opts)
                        page = ctx.new_page()
                        resp = page.goto(site['url'], wait_until='domcontentloaded', timeout=30000)
                        try:
                            page.wait_for_load_state('load', timeout=15000)
                        except Exception:
                            pass
                        page.wait_for_timeout(settle)
                        page.screenshot(path=str(png))
                        row.update(png=str(png.relative_to(out)), status=resp.status if resp else None, title=page.title()[:120], final=page.url)
                        if theme == 'dark' and light is not None:
                            diff = _differs(light, png)
                            row['darkDiff'] = round(diff, 2)
                            if diff < 3.0:  # the page ignores prefers-color-scheme: the same capture again
                                png.unlink()
                                row.pop('png')
                                row['status'] = 'same_as_light'
                        if theme == 'light':
                            light = png
                    except Exception as e:
                        row['error'] = str(e).splitlines()[0][:200]
                    finally:
                        if ctx:
                            try:
                                ctx.close()
                            except Exception:
                                pass
                    rows.append(row)
                    log.write(json.dumps(row, ensure_ascii=False) + '\n')
                    log.flush()
        browser.close()
    log.close()
    return rows


def capture(args):
    out = Path(args.out).resolve()
    (out / 'shots').mkdir(parents=True, exist_ok=True)
    sites = read_sites(args.sites, args.only)
    if args.part:  # K/N: every Nth page from the Kth, to split one capture over hosts
        k, n = map(int, args.part.split('/'))
        sites = [s for s in sites if s['index'] % n == k]
    scales = [float(s) for s in args.scales.split(',')]
    jobs = []
    for b in args.browsers.split(','):
        bs = scales if b == 'chromium' else [float(s) for s in (args.firefox_scales or args.scales).split(',')]
        dark = args.dark and (b == 'chromium' or args.firefox_dark)
        for k in range(args.jobs):
            part = sites[k::args.jobs]
            if part:
                jobs.append((b, part, str(out), bs, dark, args.settle))
    rows = []
    with ProcessPoolExecutor(args.jobs) as pool:
        for got in pool.map(_capture_worker, jobs):
            rows.extend(got)
    old = out / 'shots.jsonl'
    if old.exists():  # an earlier capture into the same folder (another browser): keep its rows
        seen = {r['id'] for r in rows}
        rows += [r for r in map(json.loads, old.read_text().splitlines()) if r['id'] not in seen]
    rows.sort(key=lambda r: r['id'])
    for part in out.glob('shots-*.part'):
        part.unlink()
    with open(out / 'shots.jsonl', 'w') as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + '\n')
    ok = sum(1 for r in rows if 'png' in r)
    print(f'{ok} captures, {sum(1 for r in rows if r.get("status") == "same_as_light")} dark pages same as light, '
          f'{sum(1 for r in rows if "error" in r)} errors', flush=True)


# ------------------------------------------------------------------------------ run/replay
def _shots(shots_dir):
    d = Path(shots_dir).resolve()
    return d, [json.loads(l) for l in open(d / 'shots.jsonl') if l.strip()]


def _drive(cmd, jobs_lines, out_fh, env):
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=out_fh, stderr=subprocess.DEVNULL, env=env, text=True)
    try:
        p.stdin.write('\n'.join(jobs_lines) + '\n')
        p.stdin.close()
    except BrokenPipeError:
        pass
    return p


def _execute(args, specs):
    """specs: one driver job per capture; sharded over --jobs drivers, appended to --out."""
    out = Path(args.out)
    done = set()
    if out.exists():
        for l in open(out):
            try:
                r = json.loads(l)
            except ValueError:
                continue
            if 'error' not in r:
                done.add(r['id'])
    todo = [s for s in specs if s['id'] not in done]
    print(f'{len(todo)} captures to run, {len(specs) - len(todo)} done', flush=True)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    cmd = [args.driver, '--helper', args.helper, '--build-json', args.build_json] + sum((['--data', d] for d in args.data), [])
    tmp = out.parent / (out.name + '.parts')
    tmp.mkdir(exist_ok=True)
    n = 0
    while todo:
        procs = []
        for _ in range(args.jobs):
            if not todo:
                break
            if not guard_ok(args.session):
                break
            part, todo = todo[:args.batch], todo[args.batch:]
            fh = open(tmp / f'part-{os.getpid()}-{n:05d}.jsonl', 'w')
            n += 1
            procs.append((_drive(cmd, [json.dumps(s, ensure_ascii=False) for s in part], fh, env), fh))
        if not procs:
            print('guard refused: waiting for the quiet lease to clear', flush=True)
            time.sleep(60)
            continue
        for p, fh in procs:
            p.wait()
            fh.close()
        print(f'{len(todo)} left', flush=True)
    with open(out, 'a') as dst:
        for part in sorted(tmp.glob('part-*.jsonl')):
            dst.write(part.read_text())
            part.unlink()
    tmp.rmdir()
    # A driver that died leaves its captures without a result: say so, as fuzz.py run does (Sol S11 on #1976).
    got = set()
    for l in open(out):
        try:
            got.add(json.loads(l)['id'])
        except (ValueError, KeyError):
            pass
    missing = [s['id'] for s in specs if s['id'] not in got]
    if missing:
        print(f'INCOMPLETE: {len(missing)} captures have no result (first: {missing[0]}); run again to retry them', flush=True)
        sys.exit(1)
    print('DONE', flush=True)


def run(args):
    d, rows = _shots(args.shots)
    specs = [dict(id=r['id'], image=str(d / r['png']), login='lab', realName='Lab Tester', terms=[], profiles=PROFILES)
             for r in rows if 'png' in r]
    _execute(args, specs)


def replay(args):
    d, rows = _shots(args.shots)
    words = {}
    for l in open(args.words):
        r = json.loads(l)
        if 'words' in r:
            words[r['id']] = r['words']  # with confidence and second-read flag, as recognized
    specs = [dict(id=r['id'], image=str(d / r['png']), login='lab', realName='Lab Tester', terms=[], profiles=PROFILES,
                  words=words[r['id']], oracleOnly=True) for r in rows if r['id'] in words]
    args.helper = '/bin/false'
    # The words' own run knew whether recognition failed or was cut; the replay's driver can't, so the
    # flags are carried over and the report still counts those captures as unfinished.
    flags = {}
    for l in open(args.words):
        r = json.loads(l)
        kept = {k: r[k] for k in ('truncated', 'error') if r.get(k)}
        if kept:
            flags[r['id']] = kept
    try:
        _execute(args, specs)
    finally:
        out = Path(args.out)
        if flags and out.exists():
            rows = [json.loads(l) for l in open(out) if l.strip()]
            seen = {r['id'] for r in rows}
            # A capture recognition failed on has no words to replay: its failure stands as it was.
            rows += [dict(id=i, **f) for i, f in flags.items() if i not in seen and 'error' in f]
            out.write_text(''.join(json.dumps({**r, **flags.get(r['id'], {})}, ensure_ascii=False) + '\n' for r in rows))


# ---------------------------------------------------------------------------------- report
def read_triage(path):
    rules = []
    if not path:
        return rules
    for line in Path(path).read_text().splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        kind, pattern, verdict, group = f[:4]
        # A pattern "url=RX text=RX" limits a rule to pages whose address matches too.
        u = re.match(r'url=(\S+) text=(.*)', pattern)
        rules.append((kind, re.compile(u.group(1)) if u else None, re.compile(u.group(2) if u else pattern), verdict, group))
    return rules


def triage(rules, kind, text, url):
    for k, urx, rx, verdict, group in rules:
        if k in ('*', kind) and (urx is None or urx.search(url)) and rx.search(text):
            return verdict, group
    return 'untriaged', ''


def report(args):
    if len(args.shots) != len(args.results):
        sys.exit('give one --results for each --shots')
    meta, results = {}, {}
    rules = read_triage(args.triage)
    # Several capture folders (one per host) report together; capture IDs name the browser, so they don't collide.
    for shots, results_file in zip(args.shots, args.results):
        meta.update({r['id']: r for r in _shots(shots)[1]})
        res = Path(results_file)
        # A run still going keeps its finished shards beside the results file.
        for f in ([res] if res.exists() else []) + sorted(res.parent.glob(res.name + '.parts/*.jsonl')):
            for l in open(f):
                if l.strip():
                    r = json.loads(l)
                    results[r['id']] = r
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    lines = []
    # per (profile, scale): pages captured, pages with any false prompt, pages with any prompt
    table, groups, kinds, pages, flagged = {}, {}, {}, {}, {}
    for sid, m in sorted(meta.items()):
        if 'png' not in m:
            continue
        r = results.get(sid)
        if not r:
            continue
        side = r.get('ocr') or r.get('oracle') or {}
        for prof in PROFILES:
            matches = side.get(prof, {}).get('matches', [])
            key = (prof, m['scale'])
            t = table.setdefault(key, dict(captures=0, prompted=0, false=0, true_only=0, incomplete=0))
            t['captures'] += 1
            pages.setdefault(prof, set()).add(m['url'])
            # The check asks when it couldn't finish (recognition failed or was cut, matching unfinished):
            # a prompt with nothing found to show, counted as false (Sol S11 on #1976).
            if 'error' in r or r.get('truncated') or side.get(prof, {}).get('unfinished'):
                t['incomplete'] += 1
                t['prompted'] += 1
                t['false'] += 1
                flagged.setdefault(prof, set()).add(m['url'])
                why = r.get('error') or ('truncated' if r.get('truncated') else 'matching_unfinished')
                groups[(prof, 'incomplete: ' + why)] = groups.get((prof, 'incomplete: ' + why), 0) + 1
                continue
            verdicts = []
            for x in matches:
                v, g = triage(rules, x['category'], x['text'], m['url'])
                verdicts.append(v)
                words = r.get('words') or []
                lo, hi = min(x['words'], default=0), max(x['words'], default=0)
                context = ' '.join(w[0] for w in words[max(0, lo - 4):lo]) + ' [' + x['text'] + '] ' + ' '.join(w[0] for w in words[hi + 1:hi + 5])
                lines.append([prof, sid, m['category'], m['browser'], str(m['scale']), m['theme'], x['category'], x['text'], v, g, m['url'], context])
                if v != 'true':
                    gk = (prof, g or 'untriaged:' + x['category'])
                    groups[gk] = groups.get(gk, 0) + 1
                    kinds[(prof, x['category'])] = kinds.get((prof, x['category']), 0) + 1
            if matches:
                t['prompted'] += 1
                if any(v != 'true' for v in verdicts):
                    t['false'] += 1
                    flagged.setdefault(prof, set()).add(m['url'])
                else:
                    t['true_only'] += 1
    with open(out / 'matches.tsv', 'w') as fh:
        fh.write('profile\tshot\tsite\tbrowser\tscale\ttheme\tkind\ttext\tverdict\tgroup\turl\tcontext\n')
        for l in lines:
            fh.write('\t'.join(x.replace('\t', ' ') for x in l) + '\n')
    summary = dict(table={f'{p}@{s}': v for (p, s), v in sorted(table.items())},
                   groups={f'{p}:{g}': n for (p, g), n in sorted(groups.items(), key=lambda kv: -kv[1])},
                   kinds={f'{p}:{k}': n for (p, k), n in sorted(kinds.items())},
                   pages={p: dict(pages=len(pages[p]), false=len(flagged.get(p, ())), urls=sorted(flagged.get(p, ()))) for p in pages})
    (out / 'summary.json').write_text(json.dumps(summary, indent=1))
    md = ['| Profile | Scale | Captures | Any prompt | False prompt | Of which unfinished | % false |', '|---|---|---|---|---|---|---|']
    for (p, s), v in sorted(table.items()):
        md.append(f'| {p} | {int(s * 100)} % | {v["captures"]} | {v["prompted"]} | {v["false"]} | {v["incomplete"]} | {100 * v["false"] / max(1, v["captures"]):.1f} |')
    md += ['', '| Profile | Pages | Pages with a false prompt in any capture |', '|---|---|---|']
    for p in sorted(pages):
        md.append(f'| {p} | {len(pages[p])} | {len(flagged.get(p, ()))} |')
    md += ['', '| Profile | Group | False matches |', '|---|---|---|']
    for (p, g), n in sorted(groups.items(), key=lambda kv: (kv[0][0], -kv[1])):
        md.append(f'| {p} | {g} | {n} |')
    (out / 'table.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))


def compare(args):
    a, b = (json.loads((Path(x) / 'summary.json').read_text()) for x in (args.before, args.after))
    print('| Profile @ scale | Captures | False prompt before | False prompt after |')
    print('|---|---|---|---|')
    for k in sorted(set(a['table']) | set(b['table'])):
        x, y = a['table'].get(k, {}), b['table'].get(k, {})
        print(f'| {k} | {x.get("captures", y.get("captures"))} | {x.get("false", 0)} | {y.get("false", 0)} |')
    print()
    for p in sorted(set(a.get('pages', {})) | set(b.get('pages', {}))):
        print(f'| {p}: pages with a false prompt | {a["pages"].get(p, {}).get("pages")} | {a["pages"].get(p, {}).get("false", 0)} | {b["pages"].get(p, {}).get("false", 0)} |')
    print()
    print('| Group | Before | After |')
    print('|---|---|---|')
    for k in sorted(set(a['groups']) | set(b['groups']), key=lambda k: -a['groups'].get(k, 0)):
        print(f'| {k} | {a["groups"].get(k, 0)} | {b["groups"].get(k, 0)} |')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture')
    c.add_argument('--sites', default=str(HERE / 'sites.tsv'))
    c.add_argument('--out', required=True)
    c.add_argument('--browsers', default='chromium,firefox')
    c.add_argument('--scales', default='1,1.25,1.5')
    c.add_argument('--firefox-scales', default='1,1.25')
    c.add_argument('--no-dark', dest='dark', action='store_false')
    c.add_argument('--firefox-dark', action='store_true')
    c.add_argument('--only', help='regular expression on "category url"')
    c.add_argument('--part', help='K/N: capture only pages whose index is K modulo N')
    c.add_argument('--settle', type=int, default=3000, help='ms to wait after load')
    c.add_argument('--jobs', type=int, default=4)
    for name in ('run', 'replay'):
        r = sub.add_parser(name)
        r.add_argument('--shots', required=True)
        r.add_argument('--driver', required=True)
        r.add_argument('--build-json', required=True)
        r.add_argument('--data', action='append', default=[])
        r.add_argument('--out', required=True)
        r.add_argument('--jobs', type=int, default=4)
        r.add_argument('--batch', type=int, default=25)
        r.add_argument('--session', default=os.environ.get('PUTGD_LAB_SESSION', ''))
        if name == 'run':
            r.add_argument('--helper', required=True)
        else:
            r.add_argument('--words', required=True, help="a previous run's results")
    p = sub.add_parser('report')
    p.add_argument('--shots', action='append', required=True, help='a capture folder; repeat with --results for each host')
    p.add_argument('--results', action='append', required=True)
    p.add_argument('--triage', default=str(HERE / 'triage.tsv'))
    p.add_argument('--out', required=True)
    m = sub.add_parser('compare')
    m.add_argument('before')
    m.add_argument('after')
    args = ap.parse_args()
    if args.cmd == 'replay':
        args.batch = max(args.batch, 500)
    dict(capture=capture, run=run, replay=replay, report=report, compare=compare)[args.cmd](args)


if __name__ == '__main__':
    main()
