#!/usr/bin/env python3
"""Research #1805: one Markdown table over every run of a host. python3 -I compare.py RUNS_DIR [LABEL...]"""
import json, sys
from pathlib import Path

CLASSES = ['terminal', 'editor-code', 'browser-local', 'browser-github', 'browser-wikipedia', 'small-ui', 'monitor']
SHORT = dict(zip(CLASSES, ['term', 'code', 'local', 'gh', 'wiki', 'small', 'mon']))


def load(d):
    s = json.loads((d / 'summary.json').read_text())
    return s


def pct(x):
    return '–' if x is None else f'{100 * x:.2f}'


def main():
    root = Path(sys.argv[1])
    labels = sys.argv[2:] or sorted(p.name for p in root.iterdir() if (p / 'summary.json').exists())
    if 'baseline' in labels:
        labels.remove('baseline'); labels.insert(0, 'baseline')
    base = load(root / 'baseline') if (root / 'baseline' / 'summary.json').exists() else None
    print('| Run | Corpus wall s | vs base | CPU s | 1080p p50 s | 4K s (2) | Peak MiB | Pooled CER % | WER mean % | ' + ' | '.join(SHORT[c] + ' CER' for c in CLASSES) + ' | CER vs today |')
    print('|' + '---|' * (10 + len(CLASSES)))
    for l in labels:
        s = load(root / l)
        a, sc, k = s['all'], s.get('screens_1080p') or {}, s.get('screens_4k') or {}
        if not a:
            print(f"| {l} (all {s['failed']} failed) |"); continue
        rel = f"{a['wall_sum'] / base['all']['wall_sum']:.2f}x" if base else ''
        four = f"{k['wall_sum'] / max(1, k['n']):.2f}" if k else '–'
        cls = [pct((s['classes'].get(c) or {}).get('cer_median')) for c in CLASSES]
        fail = f" ({s['failed']} failed)" if s['failed'] else ''
        print(f"| {l}{fail} | {a['wall_sum']:.1f} | {rel} | {a['cpu_sum']:.1f} | {sc.get('wall_p50', 0):.2f} | {four} | {a['peak_max_KiB'] / 1024:.0f} | {pct(a['cer_pooled'])} | {pct(a['wer_mean'])} | " + ' | '.join(cls) + f" | {pct(a.get('cer_vs_ref_median'))} |")


if __name__ == '__main__':
    main()
