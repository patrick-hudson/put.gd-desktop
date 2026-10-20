#!/usr/bin/env python3
"""Draft, or open with --open, the promotion pull request from nightly into main.

Merging it cuts the next release candidate. The title names the version that
nightly carries; the body lists the pull requests main does not have yet, as
the version's CHANGELOG.md section (Keep a Changelog style, from the same data
as the release notes, #188 and #217).

nightly takes changes only through pull requests, so --open first opens a
`changelog: vX.Y.Z` pull request into nightly that adds the section to
CHANGELOG.md, then the promotion pull request, which waits for it. Reviewers
curate the section in that pull request. Nothing in the working tree changes.
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import changelog  # noqa: E402

CHANGELOG = 'CHANGELOG.md'
HEADER = """# Changelog

All notable changes to put.gd, newest first. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and put.gd uses
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Each version's section is written by `tools/releases/promote.py --open` from the
pull requests merged since the previous stable release, and is curated in its
`changelog: vX.Y.Z` pull request. The date is the day the version was promoted
to `main`; the stable release follows its candidate's acceptance.
"""
_LINK = re.compile(r'\[[^\]]+\]: ')


def git(root, *args, input=None, env=None):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True, input=input,
                                   env=None if env is None else {**os.environ, **env}).strip()


def gh(root, *args, input):
    """Run gh in root and return its output (the new pull request's URL for `pr create`)."""
    return subprocess.run(['gh', *args], cwd=root, check=True, text=True, input=input,
                          stdout=subprocess.PIPE).stdout.strip()


def base_version(cmake_text):
    project = re.search(r'project\(\s*putgd\s+VERSION\s+([0-9.]+)', cmake_text)
    if not project:
        raise ValueError('Missing project base version')
    return project[1]


def repository_of(url):
    """owner/name from a GitHub remote URL (https or ssh)."""
    match = re.search(r'github\.com[:/]+([^/\s]+)/([^/\s]+?)(?:\.git)?/?$', url)
    if not match:
        raise ValueError(f'Cannot read a GitHub owner/name from the remote URL {url!r}; pass --repository')
    return f'{match[1]}/{match[2]}'


def with_section(existing, version, section, link):
    """Return CHANGELOG.md with the version's section first and its link reference first.

    None when the file already has a section for this version.
    """
    if not existing.strip():
        return HEADER + '\n' + section + '\n' + link + '\n'
    lines = existing.rstrip('\n').split('\n')
    if any(line.startswith(f'## [{version}]') for line in lines):
        return None
    sections = [i for i, line in enumerate(lines) if line.startswith('## [') and not line.startswith('## [Unreleased]')]
    links = [i for i, line in enumerate(lines) if _LINK.match(line) and not line.startswith('[Unreleased]: ')]
    if links:
        lines.insert(links[0], link)
    else:
        lines += ['', link]
    at = sections[0] if sections else (links[0] if links else len(lines) - 2)
    lines[at:at] = section.split('\n')
    return '\n'.join(lines) + '\n'


def body(version, section, changelog_pull=None):
    changes = section.split('\n', 2)[2] if section.count('\n') >= 2 else ''
    lines = [f'Promotes `nightly` into `main` for `v{version}`.', '',
             'Merging (with a merge commit) cuts the next `v' + version + '-rc.N` and stages its draft.', '']
    if changelog_pull:
        lines += [f'The changes below are `{CHANGELOG}`\'s new section, added by {changelog_pull}. '
                  'Curate them there; once it merges into nightly, this pull request carries it.', '']
    lines += ['## Changes', '', changes.rstrip('\n'), '',
              '## Before merging', '',
              '- [ ] `CI` passed on this pull request.',
              '- [ ] `project(putgd VERSION ...)` on nightly is the version to release.']
    if changelog_pull:
        lines.append(f'- [ ] {changelog_pull} is merged, so `{CHANGELOG}` has the `v{version}` section.')
    lines += ['- [ ] Known blocked acceptance gates are listed in docs/verification.md.', '',
              '## After merging', '',
              '- Release workflow tags the candidate and stages a draft.',
              '- Run exact-asset acceptance on the draft (docs/release.md), then publish it.',
              '- Run Release with `channel=stable` on the accepted candidate commit.',
              '- Open the pull request into nightly that starts the next version.']
    return '\n'.join(lines) + '\n'


def plan(root, remote='origin', repository=None, lookup=changelog.lookup_pulls, today=None):
    """Return the promotion: title, version, CHANGELOG.md section, new CHANGELOG.md text and body."""
    git(root, 'fetch', '--quiet', remote, 'main', 'nightly')
    nightly = f'{remote}/nightly'
    version = base_version(git(root, 'show', f'{nightly}:CMakeLists.txt'))
    if git(root, 'ls-remote', '--tags', remote, f'refs/tags/v{version}'):
        raise ValueError(f'v{version} is already released; merge the next-version pull request into nightly first')
    if not git(root, 'log', '--first-parent', '--format=%h', f'{remote}/main..{nightly}'):
        raise ValueError('main already contains nightly; nothing to promote')
    repository = repository or repository_of(git(root, 'remote', 'get-url', remote))
    previous = changelog.previous_stable(root, nightly)
    pulls = changelog.merged_pulls(root, previous, nightly)
    metadata = lookup(repository, [number for number, _ in pulls]) if lookup else {}
    date = (today or datetime.datetime.now(datetime.timezone.utc).date()).isoformat()
    section, link = changelog.version_section(version, date, previous, changelog.entries(pulls, metadata),
                                              'https://github.com', repository)
    existing = git(root, 'show', f'{nightly}:{CHANGELOG}') if git(root, 'ls-tree', '--name-only', nightly, CHANGELOG) else ''
    return {'title': f'release: v{version}', 'version': version, 'section': section,
            'changelog': with_section(existing, version, section, link), 'body': body(version, section)}


def commit_changelog(root, base, text, message):
    """Commit text as CHANGELOG.md on top of base through a private index; return the commit."""
    blob = git(root, 'hash-object', '-w', '--stdin', input=text)
    with tempfile.TemporaryDirectory(prefix='putgd-promote-') as directory:
        env = {'GIT_INDEX_FILE': str(Path(directory) / 'index')}
        git(root, 'read-tree', base, env=env)
        git(root, 'update-index', '--add', '--cacheinfo', f'100644,{blob},{CHANGELOG}', env=env)
        tree = git(root, 'write-tree', env=env)
    return git(root, 'commit-tree', tree, '-p', base, '-m', message)


def open_pulls(root, promotion, remote='origin'):
    """Open the changelog pull request into nightly (when needed), then the promotion."""
    version = promotion['version']
    changelog_pull = None
    if promotion['changelog'] is not None:
        branch = f'changelog/v{version}'
        commit = commit_changelog(root, f'{remote}/nightly', promotion['changelog'], f'changelog: v{version}')
        git(root, 'push', '--quiet', remote, f'{commit}:refs/heads/{branch}')
        changelog_pull = gh(root, 'pr', 'create', '--base', 'nightly', '--head', branch,
                            '--title', f'changelog: v{version}', '--body-file', '-',
                            input=f'Adds the `v{version}` section to `{CHANGELOG}` for the promotion to `main` '
                                  '(`tools/releases/promote.py --open`). Edit the entries here before merging; '
                                  'the promotion pull request carries this file once it is in nightly.\n')
    print(gh(root, 'pr', 'create', '--base', 'main', '--head', 'nightly', '--title', promotion['title'],
             '--body-file', '-', input=body(version, promotion['section'], changelog_pull)))
    return changelog_pull


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--repository', help='owner/name; read from the origin URL by default')
    parser.add_argument('--offline', action='store_true', help='Skip the pull request label lookup')
    parser.add_argument('--open', action='store_true', help='Open the changelog and promotion pull requests with gh')
    args = parser.parse_args()
    try:
        promotion = plan(args.root, repository=args.repository,
                         lookup=None if args.offline else changelog.lookup_pulls)
    except ValueError as error:
        parser.exit(1, f'promote: {error}\n')
    if not args.open:
        print(promotion['title'] + '\n\n' + promotion['body'])
        print(f'{CHANGELOG} ' + ('section:\n\n' + promotion['section'] if promotion['changelog'] is not None
                                 else f'already has v{promotion["version"]}.'))
        return
    open_pulls(args.root, promotion)


if __name__ == '__main__':
    sys.exit(main())
