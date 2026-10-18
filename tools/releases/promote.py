#!/usr/bin/env python3
"""Draft, or open with --open, the promotion pull request from nightly into main.

Merging it cuts the next release candidate. The title names the version that
nightly carries; the body lists the changes main does not have yet.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def base_version(cmake_text):
    project = re.search(r'project\(\s*putgd\s+VERSION\s+([0-9.]+)', cmake_text)
    if not project:
        raise ValueError('Missing project base version')
    return project[1]


def body(version, changes):
    lines = [f'Promotes `nightly` into `main` for `v{version}`.', '',
             'Merging (with a merge commit) cuts the next `v' + version + '-rc.N` and stages its draft.', '',
             '## Changes', '']
    lines += [f'- {change}' for change in changes] or ['- No changes; main already contains nightly.']
    lines += ['', '## Before merging', '',
              '- [ ] `CI` passed on this pull request.',
              '- [ ] `project(putgd VERSION ...)` on nightly is the version to release.',
              '- [ ] Known blocked acceptance gates are listed in docs/verification.md.', '',
              '## After merging', '',
              '- Release workflow tags the candidate and stages a draft.',
              '- Run exact-asset acceptance on the draft (docs/release.md), then publish it.',
              '- Run Release with `channel=stable` on the accepted candidate commit.',
              '- Open the pull request into nightly that starts the next version.']
    return '\n'.join(lines) + '\n'


def plan(root, remote='origin'):
    git(root, 'fetch', '--quiet', remote, 'main', 'nightly')
    version = base_version(git(root, 'show', f'{remote}/nightly:CMakeLists.txt'))
    if git(root, 'ls-remote', '--tags', remote, f'refs/tags/v{version}'):
        raise ValueError(f'v{version} is already released; merge the next-version pull request into nightly first')
    changes = git(root, 'log', '--first-parent', '--format=%s (%h)', f'{remote}/main..{remote}/nightly')
    changes = [line for line in changes.splitlines() if line]
    if not changes:
        raise ValueError('main already contains nightly; nothing to promote')
    return f'release: v{version}', body(version, changes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--open', action='store_true', help='Create the pull request with gh')
    args = parser.parse_args()
    try:
        title, text = plan(args.root)
    except ValueError as error:
        parser.exit(1, f'promote: {error}\n')
    if not args.open:
        print(title + '\n\n' + text)
        return
    subprocess.run(['gh', 'pr', 'create', '--base', 'main', '--head', 'nightly',
                    '--title', title, '--body-file', '-'], input=text, text=True, check=True)


if __name__ == '__main__':
    sys.exit(main())
