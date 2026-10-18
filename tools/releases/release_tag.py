#!/usr/bin/env python3
"""Choose and create the candidate or stable tag for the main branch HEAD.

Every push to main cuts the next candidate `vBASE-rc.N`, or reuses the
candidate already on HEAD so reruns are idempotent. A stable `vBASE` is cut
only on a commit that already carries a candidate for the same base.
Existing tags are never moved.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

from github_api import GitHub, full_sha, matching_tags, remote_tag_commit
from nightly import ensure_tag, git, project_base
from release_identity import parse_tag

MAIN_REF = 'refs/heads/main'
_CANDIDATE = re.compile(r'-rc\.([1-9][0-9]*)$')


def candidate_number(tag):
    return int(_CANDIDATE.search(tag)[1])


def release_tags(api, base):
    """Map each existing candidate/stable tag for base to its commit."""
    tags = {}
    for name in matching_tags(api, 'v' + base):
        try:
            identity = parse_tag(name)
        except ValueError:
            continue
        if identity.base_version == base and identity.channel in ('candidate', 'stable'):
            tags[name] = remote_tag_commit(api, name)
    return tags


def choose(channel, base, head, existing):
    """Return (tag, create) for the requested channel without touching the remote."""
    full_sha(head)
    stable = 'v' + base
    candidates = {tag: commit for tag, commit in existing.items() if parse_tag(tag).channel == 'candidate'}
    if channel == 'candidate':
        if stable in existing:
            raise ValueError(stable + ' is already released. Bump project(putgd VERSION ...) in this '
                             'change before cutting another candidate')
        on_head = sorted((candidate_number(tag), tag) for tag, commit in candidates.items() if commit == head)
        if on_head:
            return on_head[-1][1], False
        number = max((candidate_number(tag) for tag in candidates), default=0) + 1
        return f'{stable}-rc.{number}', True
    if channel != 'stable':
        raise ValueError('Unknown release channel: ' + str(channel))
    if stable in existing:
        if existing[stable] != head:
            raise ValueError(stable + ' already identifies ' + existing[stable])
        return stable, False
    if not any(commit == head for commit in candidates.values()):
        raise ValueError('Stable releases are cut only on a candidate commit. Run the candidate channel '
                         'on this main commit, run acceptance, then cut stable')
    return stable, True


def cut(api, root, ref, channel):
    if ref != MAIN_REF:
        raise ValueError('Candidate and stable releases are cut from main only, not ' + ref)
    if git(root, 'status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Release source must be clean')
    head = full_sha(git(root, 'rev-parse', 'HEAD'))
    base = project_base(root)
    tag, create = choose(channel, base, head, release_tags(api, base))
    release = ensure_tag(api, root, tag, head)
    if release['channel'] != channel:
        raise ValueError('Created tag has the wrong channel: ' + tag)
    return {'schemaVersion': 1, 'tag': tag, 'channel': channel, 'created': create,
            'sourceSha': head, 'baseVersion': base, 'release': release}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--channel', choices=('candidate', 'stable'), required=True)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = cut(GitHub(os.environ['GITHUB_REPOSITORY']), args.root, os.environ['GITHUB_REF'], args.channel)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as stream:
            stream.write(f"tag={result['tag']}\nchannel={result['channel']}\n")
    print(json.dumps(result), file=sys.stdout)


if __name__ == '__main__':
    main()
