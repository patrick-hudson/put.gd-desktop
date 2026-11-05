#!/usr/bin/env python3
"""Create the stable release tag for the main branch HEAD.

Every push to main cuts the stable release `vYY.MDD.N` (#373): HEAD's UTC
committer date and full-history commit count. A rerun reuses the tag already
on HEAD. The tag stages a draft; it is published only after exact-asset
acceptance, and a commit whose acceptance fails is never released (the next
promotion is a new commit and so a new version). Existing tags are never moved.
"""
import argparse
import json
import os
from pathlib import Path
import sys

from github_api import GitHub, full_sha, remote_tag_commit, tag_exists
from nightly import ensure_tag, git
from release_identity import calendar_base

MAIN_REF = 'refs/heads/main'


def choose(root, head):
    """Return the stable tag of head, from its committer date and commit count."""
    full_sha(head)
    if git(root, 'rev-parse', '--is-shallow-repository') != 'false':
        raise ValueError('The release version needs the full Git history')
    count = int(git(root, 'rev-list', '--count', head))
    return 'v' + calendar_base(int(git(root, 'show', '-s', '--format=%ct', head)), count)


def cut(api, root, ref):
    if ref != MAIN_REF:
        raise ValueError('Stable releases are cut from main only, not ' + ref)
    if git(root, 'status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Release source must be clean')
    head = full_sha(git(root, 'rev-parse', 'HEAD'))
    tag = choose(root, head)
    create = not tag_exists(api, tag)
    if not create and remote_tag_commit(api, tag) != head:
        raise ValueError(tag + ' already identifies another commit')
    release = ensure_tag(api, root, tag, head)
    if release['channel'] != 'stable':
        raise ValueError('Created tag has the wrong channel: ' + tag)
    return {'schemaVersion': 2, 'tag': tag, 'channel': 'stable', 'created': create,
            'sourceSha': head, 'baseVersion': release['baseVersion'], 'release': release}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = cut(GitHub(os.environ['GITHUB_REPOSITORY']), args.root, os.environ['GITHUB_REF'])
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as stream:
            stream.write(f"tag={result['tag']}\nchannel={result['channel']}\n")
    print(json.dumps(result), file=sys.stdout)


if __name__ == '__main__':
    main()
