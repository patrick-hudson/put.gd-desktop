#!/usr/bin/env python3
"""Bypass the hosted runner's slow Azure Ubuntu mirror for AppImage (#500).

Keep Ubuntu's archive/security failover and APT's existing signing, suites and
components. No package snapshot of its own: the shared Actions cache budget stays unchanged.

Also switch off the runner's third-party APT sources (packages.microsoft.com). The job
installs nothing from them, yet `apt-get update --error-on=any` fails the whole install
when one answers 429, and the offline snapshot fallback has no metadata for them.
"""
import argparse
from pathlib import Path
import re

AZURE = re.compile(r'(?<!\S)https?://azure\.archive\.ubuntu\.com/ubuntu/?(?=\s|$)')
ARCHIVE = 'https://archive.ubuntu.com/ubuntu/'
# The values apt reads as false for a deb822 boolean.
DISABLED = frozenset({'no', 'false', 'without', 'off', 'disable', '0'})


def configure(root):
    """Handle both runner mirror+file sources and legacy direct source URIs."""
    updates = {}
    mirrors = root / 'apt-mirrors.txt'
    if mirrors.exists():
        before = mirrors.read_text()
        lines = before.splitlines(keepends=True)
        after = ''.join(line for line in lines
                        if not line.split() or not AZURE.fullmatch(line.split()[0]))
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
        lines = []
        uris = False
        for line in before.splitlines(keepends=True):
            if not line.strip() or line.lstrip().startswith('#'):
                lines.append(line)
                continue
            if source.suffix == '.sources':
                if not line[0].isspace():
                    uris = line.startswith('URIs:')
                replace = uris
            else:
                replace = re.match(r'^\s*deb(?:-src)?\s', line)
            lines.append(AZURE.sub(ARCHIVE, line) if replace else line)
        after = ''.join(lines)
        if after != before:
            updates[source] = after
    # Validate all inputs before writing any of them.
    for path, contents in updates.items():
        path.write_text(contents)
    return [str(path.relative_to(root)) for path in updates]


def official(uri):
    """Ubuntu's own hosts, and mirror lists or files on this machine."""
    uri = uri.strip()
    if uri.startswith(('mirror+file:', 'file:')):
        return True
    host = re.match(r'^[a-z][a-z0-9+.-]*://(?:[^@/]*@)?([^/:]+)', uri, re.I)
    return bool(host) and (host[1].lower() == 'ubuntu.com' or host[1].lower().endswith('.ubuntu.com'))


def disable_third_party(root):
    """Comment out `.list` lines and add `Enabled: no` to `.sources` stanzas of non-Ubuntu hosts."""
    updates = {}
    sources = [root / 'sources.list', *sorted((root / 'sources.list.d').glob('*.list')),
               *sorted((root / 'sources.list.d').glob('*.sources'))]
    for source in sources:
        if not source.exists():
            continue
        before = source.read_text()
        if source.suffix == '.sources':
            stanzas = re.split(r'(\n\s*\n)', before)
            for index in range(0, len(stanzas), 2):
                lines = stanzas[index].splitlines(keepends=True)
                uris = []
                collecting = False
                enabled = None
                for number, line in enumerate(lines):
                    if line.lstrip().startswith('#'):
                        continue
                    if not line[:1].isspace():
                        # deb822 field names are case-insensitive.
                        name, _, value = line.partition(':')
                        collecting = name.strip().lower() == 'uris'
                        if name.strip().lower() == 'enabled':
                            enabled = number
                        line = value
                    if collecting:
                        uris += line.split()
                if not uris or all(official(uri) for uri in uris):
                    continue
                text = stanzas[index]
                if enabled is None:
                    stanzas[index] = text + ('' if text.endswith('\n') else '\n') + 'Enabled: no\n'
                elif lines[enabled].partition(':')[2].strip().lower() not in DISABLED:
                    # An explicit `Enabled: yes` would keep the source active.
                    name = lines[enabled].partition(':')[0]
                    lines[enabled] = f'{name}: no\n'
                    stanzas[index] = ''.join(lines)
            after = ''.join(stanzas)
        else:
            lines = []
            for line in before.splitlines(keepends=True):
                match = re.match(r'^\s*deb(?:-src)?\s+(?:\[[^\]]*\]\s*)?(\S+)', line)
                if match and not official(match[1]):
                    line = '# disabled for the AppImage job: ' + line
                lines.append(line)
            after = ''.join(lines)
        if after != before:
            updates[source] = after
    for path, contents in updates.items():
        path.write_text(contents)
    return [str(path.relative_to(root)) for path in updates]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apt-root', type=Path, default=Path('/etc/apt'))
    args = parser.parse_args()
    changed = configure(args.apt_root)
    third_party = disable_third_party(args.apt_root)
    print('AppImage APT: excluded Azure; kept official Ubuntu archive/security fallback; '
          'signing settings unchanged; no additional Actions cache.')
    print('Updated: ' + (', '.join(changed) or 'none (already configured or no Azure source)'))
    print('Third-party sources disabled: ' + (', '.join(third_party) or 'none'))


if __name__ == '__main__':
    main()
