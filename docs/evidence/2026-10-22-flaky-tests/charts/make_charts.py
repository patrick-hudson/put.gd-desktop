#!/usr/bin/env python3
"""Static SVG/HTML charts for the PR #351 docs; rendered to PNG with headless Chrome.

Run from anywhere: python3 make_charts.py (needs google-chrome); PNGs land beside
this folder, next to the data they show."""
import csv, html, json, pathlib, subprocess, sys

OUT = pathlib.Path(__file__).resolve().parent
DATA = json.loads((OUT / 'data.json').read_text())
SURFACE, INK, INK2, MUTED, GRID, BASE = '#fcfcfb', '#0b0b0b', '#52514e', '#898781', '#e1e0d9', '#c3c2b7'
BLUE, ORANGE = '#2a78d6', '#eb6834'
FONT = "font-family:-apple-system,'Segoe UI',Roboto,Helvetica,Arial,sans-serif"


def page(title, body, width):
    return f'''<!doctype html><html><head><meta charset="utf-8"><style>
body{{margin:0;background:{SURFACE};color:{INK};{FONT}}}
.wrap{{width:{width}px;padding:24px 28px 20px;box-sizing:border-box}}
h1{{font-size:20px;margin:0 0 4px;font-weight:650}}
.sub{{font-size:13px;color:{INK2};margin:0 0 14px;line-height:1.45}}
.foot{{font-size:11.5px;color:{MUTED};margin-top:10px;line-height:1.5}}
table{{border-collapse:collapse;font-size:12.5px;width:100%}}
th{{text-align:left;color:{INK2};font-weight:600;border-bottom:1px solid {BASE};padding:5px 6px}}
td{{border-bottom:1px solid {GRID};padding:4px 6px;vertical-align:top}}
.n{{text-align:right;font-variant-numeric:tabular-nums}}
.tag{{display:inline-block;padding:1px 6px;border-radius:4px;font-size:11px;font-weight:600}}
.legend{{display:flex;gap:18px;font-size:12.5px;color:{INK2};margin:0 0 6px}}
.sw{{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:6px;vertical-align:-1px}}
</style></head><body><div class="wrap">{body}</div></body></html>'''


def stress_chart():
    rows = DATA['stress']
    left, barh, gap, groupgap, width = 330, 16, 2, 22, 980
    plot = width - left - 170
    y, parts = 10, []
    for row in rows:
        parts.append(f'<text x="0" y="{y + 12}" font-size="13" fill="{INK}" font-weight="600">{html.escape(row["host"])}</text>')
        parts.append(f'<text x="0" y="{y + 29}" font-size="12" fill="{INK2}">{html.escape(row["contention"])}</text>')
        for i, (key, color) in enumerate((('before', BLUE), ('after', ORANGE))):
            failed, runs = row[key]
            by = y + i * (barh + gap)
            w = max(plot * failed / runs, 0)
            parts.append(f'<rect x="{left}" y="{by}" width="{plot}" height="{barh}" fill="{GRID}" opacity="0.45" rx="3"/>')
            if w:
                parts.append(f'<rect x="{left}" y="{by}" width="{w:.1f}" height="{barh}" fill="{color}" rx="3"/>')
            label = f'{failed} of {runs} runs'
            parts.append(f'<text x="{left + plot + 8}" y="{by + 12}" font-size="12" fill="{INK}">{label}</text>')
        notes = row.get('note', '').split('; ') if row.get('note') else []
        for k, note in enumerate(notes):
            parts.append(f'<text x="{left}" y="{y + 2 * (barh + gap) + 13 + 15 * k}" font-size="11.5" fill="{INK2}">{html.escape(note)}</text>')
        y += 2 * (barh + gap) + groupgap + 15 * len(notes)
    height = y
    svg = f'<svg width="{width - 56}" height="{height}" viewBox="0 0 {width - 56} {height}" xmlns="http://www.w3.org/2000/svg" style="{FONT}">{"".join(parts)}</svg>'
    legend = (f'<div class="legend"><span><span class="sw" style="background:{BLUE}"></span>{html.escape(DATA["before"])}</span>'
              f'<span><span class="sw" style="background:{ORANGE}"></span>{html.escape(DATA["after"])}</span></div>')
    body = (f'<h1>Runs with a failed test, under CPU contention</h1>'
            f'<p class="sub">The twelve tests that flaked in CI, run repeatedly by <code>tools/ci/stress_suite.py</code> on the builders. '
            f'A run counts once however many of its tests failed. CTest runs eight at once with the CI resource spec.</p>'
            f'{legend}{svg}<div class="foot">{DATA["stressFoot"]}</div>')
    return page('stress', body, width)


def parallel_chart():
    pts = DATA['parallel']
    width, height, left, top, bottom, right = 920, 330, 56, 18, 46, 220
    pw, ph = width - 56 - left - right, height - top - bottom
    xs = [p['parallel'] for p in pts]
    xmin, xmax, ymin, ymax = 5, 13, 60, 150
    X = lambda v: left + (v - xmin) / (xmax - xmin) * pw
    Y = lambda v: top + (1 - (v - ymin) / (ymax - ymin)) * ph
    parts = []
    for v in range(60, 151, 15):
        parts.append(f'<line x1="{left}" x2="{left + pw}" y1="{Y(v):.1f}" y2="{Y(v):.1f}" stroke="{GRID}" stroke-width="1"/>')
        parts.append(f'<text x="{left - 8}" y="{Y(v) + 4:.1f}" font-size="11.5" fill="{MUTED}" text-anchor="end">{v} s</text>')
    for v in (6, 8, 10, 12):
        parts.append(f'<text x="{X(v):.1f}" y="{top + ph + 18}" font-size="11.5" fill="{MUTED}" text-anchor="middle">{v}</text>')
    parts.append(f'<text x="{left + pw / 2:.1f}" y="{height - 6}" font-size="12" fill="{INK2}" text-anchor="middle">tests run at once (ctest --parallel)</text>')
    floor = DATA['floor']
    parts.append(f'<line x1="{left}" x2="{left + pw}" y1="{Y(floor):.1f}" y2="{Y(floor):.1f}" stroke="{MUTED}" stroke-dasharray="4 4" stroke-width="1"/>')
    parts.append(f'<text x="{left + pw + 8}" y="{Y(floor) + 4:.1f}" font-size="11.5" fill="{INK2}">longest test, {floor} s</text>')
    line = ' '.join(f'{X(p["parallel"]):.1f},{Y(p["median"]):.1f}' for p in pts)
    parts.append(f'<polyline points="{line}" fill="none" stroke="{BLUE}" stroke-width="2"/>')
    for p in pts:
        lo, hi = p['min'], p['max']
        parts.append(f'<line x1="{X(p["parallel"]):.1f}" x2="{X(p["parallel"]):.1f}" y1="{Y(lo):.1f}" y2="{Y(hi):.1f}" stroke="{BLUE}" stroke-width="2" opacity="0.5"/>')
        parts.append(f'<circle cx="{X(p["parallel"]):.1f}" cy="{Y(p["median"]):.1f}" r="5" fill="{BLUE}" stroke="{SURFACE}" stroke-width="2"/>')
        parts.append(f'<text x="{X(p["parallel"]) + 9:.1f}" y="{Y(p["median"]) - 8:.1f}" font-size="12" fill="{INK}">{p["median"]:.0f} s</text>')
    old = DATA['oldConfig']
    parts.append(f'<circle cx="{X(old["parallel"]):.1f}" cy="{Y(old["median"]):.1f}" r="5" fill="{ORANGE}" stroke="{SURFACE}" stroke-width="2"/>')
    parts.append(f'<text x="{X(old["parallel"]) - 9:.1f}" y="{Y(old["median"]) + 18:.1f}" font-size="12" fill="{INK}" text-anchor="end">{old["median"]:.0f} s before</text>')
    chosen = [p for p in pts if p['parallel'] == DATA['chosen']][0]
    parts.append(f'<circle cx="{X(chosen["parallel"]):.1f}" cy="{Y(chosen["median"]):.1f}" r="10" fill="none" stroke="{INK}" stroke-width="1.5"/>')
    parts.append(f'<text x="{left + pw + 8}" y="{Y(chosen["median"]) - 14:.1f}" font-size="12" fill="{INK}" font-weight="600">chosen: 12</text>')
    svg = f'<svg width="{width - 56}" height="{height}" xmlns="http://www.w3.org/2000/svg" style="{FONT}">{"".join(parts)}</svg>'
    legend = (f'<div class="legend"><span><span class="sw" style="background:{BLUE}"></span>profiled CPU pool (14 tests, 3 slots), median and range</span>'
              f'<span><span class="sw" style="background:{ORANGE}"></span>before: 8 at once, 8-test pool</span></div>')
    body = (f'<h1>Full CTest wall time on a four-core builder</h1>'
            f'<p class="sub">k72 (Kubuntu 26.04, 4 cores, like the hosted runner), all 102 tests, PUTGD_TEST_TIME_SCALE=3, no other load.</p>'
            f'{legend}{svg}<div class="foot">{DATA["parallelFoot"]}</div>')
    return page('parallel', body, width)


def history_table():
    rows = list(csv.DictReader(open(OUT.parent / 'ci-failures-2026-10-16-22.csv')))
    colors = {'flake': ('#fde7dc', '#9a3a12'), 'real bug': ('#e3eefb', '#1d5aa3'), 'open': ('#fdf1d6', '#7a5200')}
    trs = []
    for r in rows:
        bg, fg = colors[r['class']]
        trs.append(f'<tr><td style="white-space:nowrap">{r["date"][5:]}</td><td>{html.escape(r["job"])}</td><td><code>{html.escape(r["test"])}</code><br>'
                   f'<span style="color:{INK2}">{html.escape(r["function"])}</span></td>'
                   f'<td><span class="tag" style="background:{bg};color:{fg}">{r["class"]}</span></td>'
                   f'<td>{html.escape(r["what happened"])}</td><td>{html.escape(r["addressed by"])}</td></tr>')
    flakes = sum(1 for r in rows if r['class'] == 'flake')
    body = (f'<h1>Linux and sanitizer test failures, 2026-10-16 to 2026-10-22</h1>'
            f'<p class="sub">{DATA["historySub"]}</p>'
            f'<table><tr><th>Date</th><th>Job</th><th>Test</th><th>Class</th><th>What happened</th><th>Addressed by</th></tr>{"".join(trs)}</table>'
            f'<div class="foot">{DATA["historyFoot"]}</div>')
    return page('history', body, 1180)


def render(name, text, width):
    path = OUT / f'{name}.html'  # scratch page, not kept
    path.write_text(text)
    png = OUT.parent / f'{name}.png'
    subprocess.run(['google-chrome', '--headless=new', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=2',
                    f'--window-size={width},{DATA["heights"][name]}', f'--screenshot={png}', path.as_uri()],
                   check=True, capture_output=True)
    print(png)


render('stress-before-after', stress_chart(), 980)
render('parallelism', parallel_chart(), 920)
render('ci-failure-history', history_table(), 1180)
