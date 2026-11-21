#!/usr/bin/env python3
"""Research #1805: replay the text corpus through one OCR command, one process per capture.

Records wall time, CPU time (user+system of the child) and peak RSS (the child's
ru_maxrss) for every capture, and scores the text as character and word error
rate against the hand transcript, and against a reference run (today's helper)
when --reference names one. Run with python3 -I.

  python3 -I bench.py MANIFEST --label NAME --out DIR [--reference DIR] [--repeat N]
      [--only CLASS,...] [--ids ID,...] -- COMMAND ... {image} ...

COMMAND prints either one JSON line with a "text" field (bench_ocr_method,
test_ocr_run --text) or, with --raw, the recognized text itself. An "rssKiB"
or "peakKiB" field in the JSON is kept as the engine's own peak figure.
"""
import argparse, json, os, statistics, subprocess, sys, time, unicodedata
from pathlib import Path


def normalize(text):
    return ' '.join(unicodedata.normalize('NFC', text).split())


def levenshtein(a, b):
    """Unit-cost edit distance over two sequences (characters or words)."""
    if len(a) < len(b):
        a, b = b, a
    if not b:
        return len(a)
    previous = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        current = [i]
        for j, y in enumerate(b, 1):
            current.append(min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (x != y)))
        previous = current
    return previous[-1]


def char_distance(reference, actual):
    """Bit-parallel Levenshtein over code points (Myers), as score.py does."""
    if not reference:
        return len(actual)
    masks = {}
    for i, char in enumerate(reference):
        masks[char] = masks.get(char, 0) | (1 << i)
    positive, negative, score = ~0, 0, len(reference)
    top = 1 << (len(reference) - 1)
    for char in actual:
        equal = masks.get(char, 0)
        xv = equal | negative
        xh = (((equal & positive) + positive) ^ positive) | equal
        ph = negative | ~(xh | positive)
        mh = positive & xh
        score += bool(ph & top) - bool(mh & top)
        ph = (ph << 1) | 1
        mh <<= 1
        positive = mh | ~(xv | ph)
        negative = ph & xv
    return score


def rates(reference, actual):
    ref, out = normalize(reference), normalize(actual)
    cer = char_distance(ref, out) / max(1, len(ref))
    rw, ow = ref.split(), out.split()
    # Word DP is quadratic; the largest corpus capture has about 2,000 words.
    wer = levenshtein(rw, ow) / max(1, len(rw))
    return cer, wer


def read_text(stdout, raw):
    if raw:
        return stdout, {}
    lines = [l for l in stdout.splitlines() if l.strip().startswith('{')]
    if not lines:
        raise ValueError('no JSON line')
    v = json.loads(lines[-1])
    if v.get('error'):
        raise ValueError('engine error ' + str(v['error']))
    return v['text'], v


def run_one(cmd, timeout):
    """Runs one child; returns its exit code, output, wall time and its own rusage (wait4)."""
    import tempfile
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        start = time.monotonic()
        p = subprocess.Popen(cmd, stdout=out, stderr=err, stdin=subprocess.DEVNULL, env=os.environ | {'QT_QPA_PLATFORM': 'offscreen'})
        while True:
            pid, status, usage = os.wait4(p.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() - start > timeout:
                p.kill()
                os.wait4(p.pid, 0)
                p.returncode = -9
                raise ValueError('timeout')
            time.sleep(0.005)
        wall = time.monotonic() - start
        p.returncode = os.waitstatus_to_exitcode(status)
        out.seek(0); err.seek(0)
        return p.returncode, out.read().decode('utf-8', 'replace'), err.read().decode('utf-8', 'replace')[-2000:], wall, usage


def main():
    argv = sys.argv[1:]
    if '--' not in argv:
        sys.exit('usage: bench.py MANIFEST [options] -- COMMAND {image}')
    split = argv.index('--')
    p = argparse.ArgumentParser()
    p.add_argument('manifest', type=Path)
    p.add_argument('--label', required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--reference', type=Path)
    p.add_argument('--raw', action='store_true')
    p.add_argument('--repeat', type=int, default=1)
    p.add_argument('--only')
    p.add_argument('--ids')
    p.add_argument('--timeout', type=int, default=120)
    a = p.parse_args(argv[:split])
    command = argv[split + 1:]
    a.out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(a.manifest.read_text())
    root = a.manifest.parent
    captures = manifest['captures']
    if a.only:
        keep = set(a.only.split(','))
        captures = [c for c in captures if c['class'] in keep]
    if a.ids:
        keep = set(a.ids.split(','))
        captures = [c for c in captures if c['id'] in keep]
    reference = {}
    if a.reference:
        for row in json.loads((a.reference / 'rows.json').read_text()):
            if 'text' in row:
                reference[row['id']] = row['text']
    rows = []
    for c in captures:
        image = (root / c['image']).resolve()
        truth = (root / c['transcript']).read_text(encoding='utf-8')
        cmd = [x.replace('{image}', str(image)) for x in command]
        for r in range(a.repeat):
            row = dict(id=c['id'], cls=c['class'], dims=c.get('dimensions'), chars=len(normalize(truth)), run=r)
            try:
                code, out, err, wall, ru = run_one(cmd, a.timeout)
                if code != 0:
                    raise ValueError(f'exit {code}: {err[-300:]}')
                text, extra = read_text(out, a.raw)
                cer, wer = rates(truth, text)
                row.update(status='ok', wall=round(wall, 4),
                           cpu=round(ru.ru_utime + ru.ru_stime, 4),
                           # The whole child's peak (wait4); the engine's own figure is kept too.
                           maxrssKiB=ru.ru_maxrss, cer=round(cer, 5), wer=round(wer, 5), text=text)
                for k in ('peakKiB', 'rssKiB', 'peakRssKiB', 'engineStartMs', 'recognitionMs', 'totalMs', 'threads', 'scale', 'truncated', 'engineMs', 'loadMs'):
                    if k in extra:
                        row[k] = extra[k]
                if c['id'] in reference:
                    row['cer_vs_ref'], row['wer_vs_ref'] = [round(x, 5) for x in rates(reference[c['id']], text)]
            except (ValueError, OSError) as e:
                row.update(status='failed', reason=str(e)[:400])
            rows.append(row)
            print(json.dumps({k: v for k, v in row.items() if k != 'text'}), flush=True)
    (a.out / 'rows.json').write_text(json.dumps(rows, ensure_ascii=False, indent=0))
    summary = summarize(rows, a.label, command)
    (a.out / 'summary.json').write_text(json.dumps(summary, indent=1))
    print(json.dumps(summary, indent=1))


def summarize(rows, label, command):
    ok = [r for r in rows if r['status'] == 'ok']
    def med(xs):
        return round(statistics.median(xs), 5) if xs else None
    def group(pred):
        g = [r for r in ok if pred(r)]
        if not g:
            return None
        return dict(n=len(g), wall_sum=round(sum(r['wall'] for r in g), 2), cpu_sum=round(sum(r['cpu'] for r in g), 2),
                    wall_p50=med([r['wall'] for r in g]), wall_max=round(max(r['wall'] for r in g), 3),
                    peak_max_KiB=max(r.get('peakKiB') or r.get('peakRssKiB') or r.get('rssKiB') or r['maxrssKiB'] for r in g),
                    cer_median=med([r['cer'] for r in g]), cer_mean=round(sum(r['cer'] for r in g) / len(g), 5),
                    cer_pooled=round(sum(r['cer'] * r['chars'] for r in g) / max(1, sum(r['chars'] for r in g)), 5),
                    wer_median=med([r['wer'] for r in g]), wer_mean=round(sum(r['wer'] for r in g) / len(g), 5),
                    cer_vs_ref_median=med([r['cer_vs_ref'] for r in g if 'cer_vs_ref' in r]))
    classes = sorted({r['cls'] for r in ok})
    big = lambda r: r.get('dims') and r['dims'][0] * r['dims'][1] >= 4_000_000
    screen = lambda r: r['cls'] == 'monitor' and not big(r)
    return dict(label=label, command=command, captures=len({r['id'] for r in rows}), failed=len(rows) - len(ok),
                all=group(lambda r: True), screens_1080p=group(screen), screens_4k=group(big),
                regions=group(lambda r: r['cls'] != 'monitor'),
                classes={c: group(lambda r, c=c: r['cls'] == c) for c in classes})


if __name__ == '__main__':
    main()
