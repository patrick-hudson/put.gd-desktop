#!/usr/bin/env python3
"""Validate a release label against the actual clean Git checkout and project base."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def verify(root, tag):
    pattern = r'v((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))(?:-rc\.([1-9][0-9]*))?'
    match = re.fullmatch(pattern, tag)
    if not match:
        raise ValueError('Expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH-rc.N')
    project = re.search(r'project\(\s*putgd\s+VERSION\s+([0-9.]+)', (root / 'CMakeLists.txt').read_text())
    if not project or match[1] != project[1]:
        raise ValueError('Release tag must match the project base version')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()
    if git('status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Release source must be clean')
    commit = git('rev-parse', 'HEAD')
    if git('rev-parse', '--verify', 'refs/tags/' + tag + '^{commit}') != commit:
        raise ValueError('Release tag must identify the current checkout')
    return {'schemaVersion': 1, 'tag': tag, 'version': tag[1:], 'baseVersion': match[1],
            'packageVersion': match[1] + ('~rc.' + match[2] if match[2] else ''), 'prerelease': bool(match[2]),
            'gitCommit': commit, 'gitTree': git('rev-parse', 'HEAD^{tree}'),
            'commitCount': int(git('rev-list', '--count', 'HEAD'))}


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
