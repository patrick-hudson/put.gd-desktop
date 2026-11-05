#!/usr/bin/env python3
"""Validate a release label against the actual clean Git checkout: its UTC commit date and count (#373)."""
import argparse
import json
from pathlib import Path
import subprocess

from release_identity import calendar_base, parse_tag


def verify(root, tag):
    identity = parse_tag(tag)
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    if git('status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Release source must be clean')
    commit = git('rev-parse', 'HEAD')
    if git('rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != commit:
        raise ValueError('Release tag must identify the current checkout')
    if git('rev-parse', '--is-shallow-repository') != 'false':
        raise ValueError('Release identity requires full Git history')
    count = int(git('rev-list', '--count', 'HEAD'))
    base = calendar_base(int(git('show', '-s', '--format=%ct', 'HEAD')), count)
    if identity.legacy or identity.base_version != base:
        raise ValueError(f'Release tag must be v{base} or v{base}-nightly: the source commit\'s UTC date and commit count')
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
