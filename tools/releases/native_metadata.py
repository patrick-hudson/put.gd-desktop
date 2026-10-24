#!/usr/bin/env python3
"""Generate retained DEP-5 notices and a reproducible native package changelog."""
import argparse
from datetime import datetime, timezone
from email.utils import format_datetime
import gzip
import json
from pathlib import Path


def continuation(text):
    return '\n'.join(' ' + line if line else ' .' for line in text.splitlines())


def generate(docs, identity, source_url, timestamp):
    # Read the installed notice inputs, including the complete upstream Bun
    # corpus. Debian's path-exclude exception retains only this copyright file.
    inputs = sorted(p for p in docs.rglob('*') if p.is_file() and p.name not in
                    ('copyright', 'changelog.gz', 'changelog.Debian.gz'))
    groups = [('*', 'MIT', [docs / 'LICENSE']),
              ('usr/lib*/putgd/libaws* usr/lib*/putgd/libs2n*',
               'Apache-2.0 and MIT and Zlib and BSD-2-clause',
               [p for p in inputs if p.name.startswith('aws-')]),
              ('usr/bin/putgd-record', 'LGPL-2.1+',
               [p for p in inputs if p.name.startswith('wayland-')])]
    if (docs / 'proton').is_dir():
        groups.append(('usr/libexec/putgd/proton/*', 'Proton-bundled-notices',
                       [p for p in inputs if p.parent == docs / 'proton']))
    text = ('Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/\n'
            'Upstream-Name: put.gd\nSource: ' + source_url + '\n\n')
    for files, license_name, paths in groups:
        text += ('Files: ' + files + '\nCopyright: See the original copyright statements reproduced below.\n'
                 'License: ' + license_name + '\n')
        for path in paths:
            data = gzip.decompress(path.read_bytes()) if path.suffix == '.gz' else path.read_bytes()
            text += continuation(path.relative_to(docs).as_posix() + ':\n' + data.decode('utf-8', errors='strict')) + '\n'
        text += '\n'
    # Preserve source pointers and every additional installed notice as well.
    included = {p for _, _, paths in groups for p in paths}
    extra = '\n\n'.join(p.relative_to(docs).as_posix() + ':\n' + p.read_text()
                        for p in inputs if p not in included)
    if extra:
        text += 'Comment: Additional redistribution notices and source directions.\n' + continuation(extra) + '\n'
    (docs / 'copyright').write_text(text)
    date = format_datetime(datetime.fromtimestamp(timestamp, timezone.utc))
    changelog = ('putgd (' + identity['packageVersion'] + ') unstable; urgency=medium\n\n'
                 '  * Build from source commit ' + identity['gitCommit'] + '.\n'
                 '  * Release notes and corresponding source: ' + source_url + '\n\n'
                 ' -- put.gd maintainers <packages@put.gd>  ' + date + '\n')
    (docs / 'changelog.gz').write_bytes(gzip.compress(changelog.encode(), mtime=0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--docs', type=Path, required=True)
    parser.add_argument('--identity', type=Path, required=True)
    parser.add_argument('--source-url', required=True)
    parser.add_argument('--timestamp', type=int, required=True)
    args = parser.parse_args()
    generate(args.docs, json.loads(args.identity.read_text()), args.source_url, args.timestamp)


if __name__ == '__main__':
    main()
