#!/usr/bin/env python3
"""Draft, or open with --open, the promotion pull request from nightly into main.

Merging it tags the stable release `vYY.MDD.N` on the merge commit and stages
its draft (#373). That version is the merge commit's UTC date and commit count,
so it is known only after the merge. The title names the nightly version being
promoted; the body lists the pull requests main does not have yet, as
CHANGELOG.md's `[Unreleased]` section (Keep a Changelog style, from the same
data as the release notes, #188 and #217).

nightly takes changes only through pull requests, so --open first opens a
`changelog: next stable release` pull request into nightly that adds that
section, then the promotion pull request, which waits for it. Reviewers curate
the section there. Once the stable release is published, --finalize vYY.MDD.N
opens `changelog: vYY.MDD.N`, which names the section after the version.
Nothing in the working tree changes.
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
from release_identity import calendar_base, parse_tag  # noqa: E402

CHANGELOG = 'CHANGELOG.md'
HEADER = """# Changelog

All notable changes to put.gd, newest first. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). put.gd uses calendar
versions `YY.MDD.N`: the UTC date of the released commit (month times 100 plus
day) and its commit count.

`tools/releases/promote.py --open` writes the `[Unreleased]` section from the
pull requests merged since the previous stable release, curated in its
`changelog: next stable release` pull request. Once the stable release is
published, `promote.py --finalize vYY.MDD.N` names the section after it; the
date is the day it was published.
"""
_LINK = re.compile(r'\[[^\]]+\]: ')


def git(root, *args, input=None, env=None):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True, input=input,
                                   env=None if env is None else {**os.environ, **env}).strip()


def gh(root, *args, input):
    """Run gh in root and return its output (the new pull request's URL for `pr create`)."""
    return subprocess.run(['gh', *args], cwd=root, check=True, text=True, input=input,
                          stdout=subprocess.PIPE).stdout.strip()


def repository_of(url):
    """owner/name from a GitHub remote URL (https or ssh)."""
    match = re.search(r'github\.com[:/]+([^/\s]+)/([^/\s]+?)(?:\.git)?/?$', url)
    if not match:
        raise ValueError(f'Cannot read a GitHub owner/name from the remote URL {url!r}; pass --repository')
    return f'{match[1]}/{match[2]}'


def with_section(existing, version, section, link):
    """Return CHANGELOG.md with the section first and its link reference first.

    version None adds the `[Unreleased]` section. None when the file already has
    a section for this version, or an `[Unreleased]` one: it is being curated.
    """
    if not existing.strip():
        return HEADER + '\n' + section + '\n' + link + '\n'
    lines = existing.rstrip('\n').split('\n')
    if any(line.startswith(f'## [{version or "Unreleased"}]') for line in lines):
        return None
    sections = [i for i, line in enumerate(lines) if line.startswith('## [')
                and (version is None or not line.startswith('## [Unreleased]'))]
    links = [i for i, line in enumerate(lines) if _LINK.match(line)
             and (version is None or not line.startswith('[Unreleased]: '))]
    if links:
        lines.insert(links[0], link)
    else:
        lines += ['', link]
    at = sections[0] if sections else (links[0] if links else len(lines) - 2)
    lines[at:at] = section.split('\n')
    return '\n'.join(lines) + '\n'


def named_section(existing, version, date, link):
    """Return CHANGELOG.md with its `[Unreleased]` section named after version, or None without one."""
    lines = existing.rstrip('\n').split('\n')
    headings = [i for i, line in enumerate(lines) if line.strip() == '## [Unreleased]']
    references = [i for i, line in enumerate(lines) if line.startswith('[Unreleased]: ')]
    if len(headings) != 1 or len(references) > 1:
        return None
    lines[headings[0]] = f'## [{version}] - {date}'
    if references:
        lines[references[0]] = link
    else:
        lines += ['', link]
    return '\n'.join(lines) + '\n'


def body(source, section, changelog_pull=None):
    changes = section.split('\n', 2)[2] if section.count('\n') >= 2 else ''
    lines = [f'Promotes `nightly` (`{source}-nightly`) into `main`.', '',
             'Merging (with a merge commit) tags the stable release `vYY.MDD.N`, the merge commit\'s '
             'UTC date and commit count, and stages its draft.', '']
    if changelog_pull:
        lines += [f'The changes below are `{CHANGELOG}`\'s `[Unreleased]` section, added by {changelog_pull}. '
                  'Curate them there; once it merges into nightly, this pull request carries it.', '']
    lines += ['## Changes', '', changes.rstrip('\n'), '',
              '## Before merging', '',
              '- [ ] `CI` passed on this pull request.']
    if changelog_pull:
        lines.append(f'- [ ] {changelog_pull} is merged, so `{CHANGELOG}` has the `[Unreleased]` section.')
    lines += ['- [ ] Known blocked acceptance gates are listed in docs/verification.md.', '',
              '## After merging', '',
              '- The Release workflow tags `vYY.MDD.N` and stages its draft.',
              '- Run exact-asset acceptance on the draft (docs/release.md), then publish it.',
              '- Run `tools/releases/promote.py --finalize vYY.MDD.N` to name the CHANGELOG.md section.']
    return '\n'.join(lines) + '\n'


def nightly_version(root, ref):
    """The calendar version YY.MDD.N of a commit: its UTC committer date and commit count."""
    return calendar_base(int(git(root, 'show', '-s', '--format=%ct', ref)), int(git(root, 'rev-list', '--count', ref)))


def current_changelog(root, ref):
    return git(root, 'show', f'{ref}:{CHANGELOG}') if git(root, 'ls-tree', '--name-only', ref, CHANGELOG) else ''


def plan(root, remote='origin', repository=None, lookup=changelog.lookup_pulls, released=None):
    """Return the promotion: title, nightly version, `[Unreleased]` section, new CHANGELOG.md text and body."""
    git(root, 'fetch', '--quiet', remote, 'main', 'nightly')
    nightly = f'{remote}/nightly'
    if not git(root, 'log', '--first-parent', '--format=%h', f'{remote}/main..{nightly}'):
        raise ValueError('main already contains nightly; nothing to promote')
    source = nightly_version(root, nightly)
    repository = repository or repository_of(git(root, 'remote', 'get-url', remote))
    previous = changelog.previous_stable(root, nightly, released(repository) if released else None)
    pulls = changelog.merged_pulls(root, previous, nightly)
    metadata = lookup(repository, [number for number, _ in pulls]) if lookup else {}
    section, link = changelog.version_section(None, None, previous, changelog.entries(pulls, metadata),
                                              'https://github.com', repository)
    return {'title': f'release: promote nightly {source}', 'source': source, 'section': section,
            'changelog': with_section(current_changelog(root, nightly), None, section, link),
            'body': body(source, section)}


def finalize(root, tag, remote='origin', repository=None, lookup=changelog.lookup_pulls, released=None, today=None):
    """Return the `changelog: vYY.MDD.N` change for a published stable release: title and new CHANGELOG.md text.

    The `[Unreleased]` section takes the version's name; without one, a section
    is written from the pull requests merged since the previous stable release.
    The text is None when CHANGELOG.md already has the version.
    """
    identity = parse_tag(tag)
    if identity.channel != 'stable' or identity.legacy:
        raise ValueError(f'{tag} is not a calendar stable release')
    git(root, 'fetch', '--quiet', remote, 'nightly', f'refs/tags/{tag}:refs/tags/{tag}')
    repository = repository or repository_of(git(root, 'remote', 'get-url', remote))
    published = released(repository) if released else None
    if published is not None and tag not in published:
        raise ValueError(f'{tag} is not published; publish it before naming its CHANGELOG.md section')
    version = identity.version
    date = (today or datetime.datetime.now(datetime.timezone.utc).date()).isoformat()
    previous = changelog.previous_stable(root, tag, published)
    existing = current_changelog(root, f'{remote}/nightly')
    link = changelog.version_link(version, previous, 'https://github.com', repository)
    if any(line.startswith(f'## [{version}]') for line in existing.split('\n')):
        text = None
    else:
        text = named_section(existing, version, date, link) if existing.strip() else None
        if text is None:
            pulls = changelog.merged_pulls(root, previous, tag)
            metadata = lookup(repository, [number for number, _ in pulls]) if lookup else {}
            section, link = changelog.version_section(version, date, previous, changelog.entries(pulls, metadata),
                                                      'https://github.com', repository)
            text = with_section(existing, version, section, link)
    return {'title': f'changelog: {tag}', 'version': version, 'changelog': text}


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
    source = promotion['source']
    changelog_pull = None
    if promotion['changelog'] is not None:
        branch = f'changelog/promote-{source}'
        commit = commit_changelog(root, f'{remote}/nightly', promotion['changelog'], 'changelog: next stable release')
        git(root, 'push', '--quiet', remote, f'{commit}:refs/heads/{branch}')
        changelog_pull = gh(root, 'pr', 'create', '--base', 'nightly', '--head', branch,
                            '--title', 'changelog: next stable release', '--body-file', '-',
                            input=f'Adds the `[Unreleased]` section to `{CHANGELOG}` for the promotion of nightly '
                                  f'`{source}` to `main` (`tools/releases/promote.py --open`). Edit the entries here '
                                  'before merging; the promotion pull request carries this file once it is in nightly.\n')
    print(gh(root, 'pr', 'create', '--base', 'main', '--head', 'nightly', '--title', promotion['title'],
             '--body-file', '-', input=body(source, promotion['section'], changelog_pull)))
    return changelog_pull


def open_finalize(root, change, remote='origin'):
    """Open the pull request into nightly that names the CHANGELOG.md section after the release."""
    version = change['version']
    branch = f'changelog/v{version}'
    commit = commit_changelog(root, f'{remote}/nightly', change['changelog'], change['title'])
    git(root, 'push', '--quiet', remote, f'{commit}:refs/heads/{branch}')
    url = gh(root, 'pr', 'create', '--base', 'nightly', '--head', branch, '--title', change['title'], '--body-file', '-',
             input=f'Names `{CHANGELOG}`\'s `[Unreleased]` section after the published stable release `v{version}` '
                   '(`tools/releases/promote.py --finalize`).\n')
    print(url)
    return url


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--repository', help='owner/name; read from the origin URL by default')
    parser.add_argument('--offline', action='store_true', help='Skip the pull request label and release lookups')
    parser.add_argument('--open', action='store_true', help='Open the pull requests with gh')
    parser.add_argument('--finalize', metavar='TAG', help='Name the [Unreleased] section after this published stable release')
    args = parser.parse_args()
    lookup, released = (None, None) if args.offline else (changelog.lookup_pulls, changelog.released_tags)
    try:
        if args.finalize:
            change = finalize(args.root, args.finalize, repository=args.repository, lookup=lookup, released=released)
        else:
            promotion = plan(args.root, repository=args.repository, lookup=lookup, released=released)
    except ValueError as error:
        parser.exit(1, f'promote: {error}\n')
    if args.finalize:
        if change['changelog'] is None:
            print(f'{CHANGELOG} already has v{change["version"]}.')
        elif args.open:
            open_finalize(args.root, change)
        else:
            print(change['title'] + '\n\n' + change['changelog'])
        return
    if not args.open:
        print(promotion['title'] + '\n\n' + promotion['body'])
        print(f'{CHANGELOG} ' + ('section:\n\n' + promotion['section'] if promotion['changelog'] is not None
                                 else 'already has an [Unreleased] section.'))
        return
    open_pulls(args.root, promotion)


if __name__ == '__main__':
    sys.exit(main())
