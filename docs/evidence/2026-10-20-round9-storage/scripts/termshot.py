#!/usr/bin/env python3
"""Render a transcript as an annotated terminal image with ImageMagick:
termshot.py IN.txt OUT.png "TITLE" "SUBTITLE" HIGHLIGHT_SUBSTRING... (lines containing one get a red box and red text)"""
import subprocess, sys, textwrap
src, out, title, subtitle, *marks = sys.argv[1:]
mono = '/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf'
bold = '/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf'
sans = '/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf'
lines = []
for raw in open(src).read().splitlines():
    wrapped = textwrap.wrap(raw, 118, subsequent_indent=' ' * 12) or ['']
    lines += [(w, any(m in raw for m in marks)) for w in wrapped]
lh, top, width = 24, 96, 1500
height = top + lh * len(lines) + 30
args = ['magick', '-size', f'{width}x{height}', 'xc:#14181d',
        '-fill', '#f0f3f6', '-font', bold, '-pointsize', '24', '-annotate', '+24+40', title,
        '-fill', '#9aa5b1', '-font', sans, '-pointsize', '17', '-annotate', '+24+70', subtitle,
        '-font', mono, '-pointsize', '17']
groups, start = [], None
for i, (_, hot) in enumerate(lines + [('', False)]):
    if hot and start is None: start = i
    if not hot and start is not None: groups.append((start, i - 1)); start = None
for a, b in groups:
    args += ['-fill', 'none', '-stroke', '#ff5a3c', '-strokewidth', '3', '-draw',
             f'roundrectangle 14,{top + a * lh - 18} {width - 14},{top + b * lh + 8} 6,6', '-stroke', 'none']
for i, (text, hot) in enumerate(lines):
    args += ['-fill', '#ff8a70' if hot else '#d7dde3', '-annotate', f'+24+{top + i * lh}', text]
args.append(out)
subprocess.run(args, check=True)
