#!/usr/bin/env python3
"""Bypass the hosted runner's slow Azure Ubuntu mirror for AppImage (#500).

Keep Ubuntu's archive/security failover and APT's existing signing, suites and
components. No package snapshot: the shared Actions cache budget stays unchanged.
"""
import argparse
from pathlib import Path
import re

AZURE = re.compile(r'(?<!\S)https?://azure\.archive\.ubuntu\.com/ubuntu/?(?=\s|$)')
ARCHIVE = 'https://archive.ubuntu.com/ubuntu/'


def configure(root):
    """Handle both runner mirror+file sources and legacy direct source URIs."""
    updates = {}
    mirrors = root / 'apt-mirrors.txt'
    if mirrors.exists():
        before = mirrors.read_text()
        lines = before.splitlines(keepends=True)
        after = ''.join(line for line in lines
                        if line.lstrip().startswith('#') or not AZURE.search(line))
        if after != before:
            # Fail closed if the runner no longer supplies its official fallback.
            if not any(line.split() and line.split()[0] == ARCHIVE
                       for line in after.splitlines()):
                raise ValueError('Azure mirror has no official Ubuntu archive fallback')
            updates[mirrors] = after
    sources = [root / 'sources.list', *sorted((root / 'sources.list.d').glob('*.list')),
               *sorted((root / 'sources.list.d').glob('*.sources'))]
    for source in sources:
        if not source.exists():
            continue
        before = source.read_text()
        after = ''.join(AZURE.sub(ARCHIVE, line)
                        if line.lstrip().startswith(('deb ', 'deb-src ', 'URIs:')) else line
                        for line in before.splitlines(keepends=True))
        if after != before:
            updates[source] = after
    # Validate all inputs before writing any of them.
    for path, contents in updates.items():
        path.write_text(contents)
    return [str(path.relative_to(root)) for path in updates]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apt-root', type=Path, default=Path('/etc/apt'))
    args = parser.parse_args()
    changed = configure(args.apt_root)
    print('AppImage APT: excluded Azure; kept official Ubuntu archive/security fallback; '
          'signing settings unchanged; no additional Actions cache.')
    print('Updated: ' + (', '.join(changed) or 'none (already configured or no Azure source)'))


if __name__ == '__main__':
    main()
