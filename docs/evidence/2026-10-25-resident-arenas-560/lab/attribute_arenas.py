#!/usr/bin/env python3
"""Per-arena malloc_info and 64 MiB-aligned anonymous heap RSS from retained #560 d2 files.

usage: python3 attribute_arenas.py DIR   (DIR holds before-workload-malloc.xml, after-complete-settle-malloc.xml
and the matching *-smaps.txt files from evidence-b1-d2.tar.gz, member p560d2/)
glibc malloc_info: <total type="rest"> includes the top chunk (malloc.c 2.39 lines 5857-5860), so an
arena with no bin chunks and rest ~= system holds an entirely free top that mtrim() never trims."""
import re, sys, xml.etree.ElementTree as ET
from pathlib import Path
root = Path(sys.argv[1]); MIB = 1024**2
for name in ('before-workload', 'after-complete-settle'):
    tree = ET.parse(root / f'{name}-malloc.xml').getroot()
    print(name)
    for heap in tree.findall('heap'):
        total = {t.get('type'): int(t.get('size')) for t in heap.findall('total')}
        system = {s.get('type'): int(s.get('size')) for s in heap.findall('system')}
        bins = heap.find('sizes').findall('size')
        print(f"  arena {heap.get('nr'):>2} system {system.get('current'):>9} fast {total.get('fast'):>7} rest {total.get('rest'):>9} binClasses {len(bins)}")
for name in ('before-workload-smaps.txt', 'after-complete-settle-smaps.txt', 'after-complete-settle-after-trim-smaps.txt'):
    rows, cur = [], None
    for line in (root / name).read_text().splitlines():
        m = re.match(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ (\d+)\s*(.*)$', line)
        if m: cur = dict(s=int(m[1], 16), e=int(m[2], 16), name=m[5].strip(), rss=0); rows.append(cur); continue
        if cur and line.startswith('Rss:'): cur['rss'] = int(line.split()[1]) * 1024
    groups = {}
    for r in rows:
        if r['name'] == '': groups.setdefault(r['s'] & ~(64 * MIB - 1), []).append(r)
    heaps = [g for b, g in groups.items() if g[0]['s'] == b and max(x['e'] for x in g) - b >= 60 * MIB]
    sizes = sorted((sum(x['rss'] for x in g) for g in heaps), reverse=True)
    print(name, 'nonMainArenaHeaps', len(heaps), 'rss', sum(sizes), 'largest', sizes[:6])
