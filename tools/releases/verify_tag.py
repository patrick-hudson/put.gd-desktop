#!/usr/bin/env python3
"""Validate a release label against the actual clean Git checkout and project base."""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess

from release_identity import parse_tag


def verify(root, tag):
    identity = parse_tag(tag)
    project = re.search(r'project\(\s*putgd\s+VERSION\s+([0-9.]+)', (root / 'CMakeLists.txt').read_text())
    if not project or identity.base_version != project[1]:
        raise ValueError('Release tag must match the project base version')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    if git('status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Release source must be clean')
    commit = git('rev-parse', 'HEAD')
    if git('rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != commit:
        raise ValueError('Release tag must identify the current checkout')
    count = int(git('rev-list', '--count', 'HEAD'))
    if identity.channel == 'nightly':
        if git('rev-parse', '--is-shallow-repository') != 'false':
            raise ValueError('Nightly identity requires full Git history')
        utc = datetime.datetime.fromtimestamp(int(git('show', '-s', '--format=%ct', 'HEAD')),
                                              datetime.timezone.utc).strftime('%Y%m%d%H%M%S')
        if (identity.count != count or identity.short_commit != commit[:12]
                or identity.utc != utc):
            raise ValueError('Nightly tag count, UTC timestamp and short hash must match source HEAD')
    return {'schemaVersion': 1, 'tag': tag, 'version': identity.version,
            'baseVersion': identity.base_version, 'packageVersion': identity.package_version,
            'channel': identity.channel, 'prerelease': identity.prerelease,
            'gitCommit': commit, 'gitTree': git('rev-parse', 'HEAD^{tree}'), 'commitCount': count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--output', type=Path, default=Path('build/release-tag.json'))
    args = parser.parse_args()
    result = verify(args.root, args.tag)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
