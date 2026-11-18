#!/usr/bin/env python3
"""Group ThreadSanitizer reports by their racing accesses.

TSan with log_path writes one file per process (tsan.<pid>). This lists each distinct report once with how often it
occurred, in two parts: reports with a put.gd frame within the top five frames of either racing access (the ones to
read first), and reports that stay inside Qt, GLib, GStreamer or the C library (usually a suppression candidate, after
the report has been read). Reports of the same shape that differ only in addresses or thread numbers are one group.

  python3 -I tools/lab/tsan_summary.py --source ~/work/races-1/src tsan-reports/
"""
import argparse
import collections
from pathlib import Path
import re
import sys


def function(line):
    line = re.sub(r'^\s+#\d+ ', '', line)
    return re.sub(r' (/|\.\./|<null>).*', '', line)[:110]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--source', required=True, help='the repository root; a frame under its src/ or tests/ is put.gd code')
    options = parser.parse_args()
    own_prefixes = (str(Path(options.source).resolve() / 'src') + '/', str(Path(options.source).resolve() / 'tests') + '/')
    groups = {'put.gd': collections.defaultdict(list), 'external': collections.defaultdict(list)}
    for report in sorted(options.folder.glob('tsan.*')):
        for block in report.read_text(errors='replace').split('==================\n'):
            if 'WARNING: ThreadSanitizer' not in block:
                continue
            kind = re.search(r'WARNING: ThreadSanitizer: (.*?) \(', block)
            accesses = [s for s in block.split('\n\n') if re.match(r'\s*(WARNING.*\n)?\s*(Write|Read|Previous|Atomic)', s)][:2]
            stacks = [[l for l in s.split('\n') if re.match(r'\s+#\d+ ', l)] for s in accesses]
            own = [function(l) for frames in stacks for l in frames[:5] if any(p in l for p in own_prefixes) and 'sanitizer-diagnosis' not in l]
            key = (kind.group(1) if kind else '?', ' || '.join(function(frames[0]) if frames else '?' for frames in stacks), tuple(sorted(set(own)))[:2])
            groups['put.gd' if own else 'external'][key].append(report.name)
    for name in ('put.gd', 'external'):
        print('=====', name, sum(len(v) for v in groups[name].values()), 'reports')
        for (kind, key, own), files in sorted(groups[name].items(), key=lambda item: -len(item[1])):
            print('%d x %s | %s' % (len(files), kind, key[:200]))
            if own:
                print('      put.gd: %s' % ' ; '.join(own)[:240])
            print('      e.g. %s' % files[0])
    return 0


if __name__ == '__main__':
    sys.exit(main())
