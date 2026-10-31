#!/usr/bin/env python3
"""Write release changes and the downloads/sources boundary from merged pull requests.

The changes are the pull request merge commits between the previous release and
this tag. A nightly is compared with the previous nightly; a stable release
(or a pre-#373 candidate) with the previous stable, and its documentation, lab
and test changes are folded into one line. Only published releases count as
the previous one when their list is available: a stable tag whose acceptance
failed was never released. Grouping uses the pull request's labels, falling back
to its title prefix (`feat:`, `fix(s3):`, `docs:` ...).

Everything but the optional pull request lookup (labels and the issues each pull
request closes) is read from the local repository, so the output is
deterministic for a given history and lookup.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

from release_identity import parse_tag

# Section order. Folded groups are listed in one line for candidate/stable notes.
GROUPS = (('features', 'Features'), ('fixes', 'Fixes'), ('recording', 'Recording'),
          ('uploads', 'Uploads'), ('packaging', 'Packaging and releases'),
          ('other', 'Other changes'), ('docs', 'Documentation'), ('lab', 'Lab'),
          ('test', 'Tests and CI'))
FOLDED = {'docs': 'documentation', 'lab': 'lab', 'test': 'test'}

# Title types and scopes, lower case, that name a group directly.
_NOISE = {'docs': 'docs', 'doc': 'docs', 'lab': 'lab', 'test': 'test', 'tests': 'test', 'ci': 'test'}
_AREAS = {'recording': 'recording', 'record': 'recording', 'uploads': 'uploads', 'upload': 'uploads',
          's3': 'uploads', 'proton': 'uploads', 'storage': 'uploads', 'packaging': 'packaging',
          'releases': 'packaging', 'release': 'packaging', 'appimage': 'packaging', 'flatpak': 'packaging',
          'deb': 'packaging', 'rpm': 'packaging', 'build': 'packaging'}
_KINDS = {'feat': 'features', 'feature': 'features', 'fix': 'fixes', 'bugfix': 'fixes'}
_LABELS = {'documentation': 'docs', 'bug': 'fixes', 'enhancement': 'features',
           **{name: name for name, _ in GROUPS}}

_MERGE = re.compile(r'Merge pull request #([1-9][0-9]*) from ([^/\s]+)/(\S+)')
_PREFIX = re.compile(r'([A-Za-z][A-Za-z0-9 -]*?)(?:\(([^)]*)\))?!?:\s+(.*)')
_TRAILING_REFS = re.compile(r'\s*\(#[1-9][0-9]*(?:[\s,]+#[1-9][0-9]*)*\)\s*$')
_REF = re.compile(r'#([1-9][0-9]*)')
# Promotion and back-merge pull requests carry other pull requests' merges.
_PLUMBING_BRANCHES = {'main', 'nightly'}


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def previous_tag(root, tag, released=None):
    """Return the release tag this tag's notes start from, or None for the whole history.

    Only tags reachable from this tag count, so a hotfix on main never borrows
    nightly's history and the result doesn't depend on tags pushed later. With
    released (the published release tags), unpublished tags don't count either.
    """
    identity = parse_tag(tag)
    return _latest(root, tag, 'nightly' if identity.channel == 'nightly' else 'stable', released)


def previous_stable(root, ref, released=None):
    """Return the newest stable tag reachable from ref (a promotion's base), or None."""
    return _latest(root, ref, 'stable', released)


def _latest(root, ref, wanted, released=None):
    commit = git(root, 'rev-parse', ref + '^{commit}')
    found = []
    for name in git(root, 'tag', '--merged', commit, '--list', 'v*').splitlines():
        try:
            other = parse_tag(name)
        except ValueError:
            continue
        if name == ref or other.channel != wanted or (released is not None and name not in released):
            continue
        if git(root, 'rev-parse', name + '^{commit}') == commit:
            continue
        found.append((other.key, name))
    return max(found)[1] if found else None


def released_tags(repository):
    """The tags of the repository's published (non-draft) releases, or None if they can't be read."""
    try:
        output = subprocess.check_output(['gh', 'api', '--paginate', f'repos/{repository}/releases?per_page=100',
                                          '--jq', '.[] | select(.draft == false) | .tag_name'], text=True, timeout=120)
    except (subprocess.SubprocessError, OSError) as error:
        print(f'Warning: could not read the published releases for the changelog: {error}', file=sys.stderr, flush=True)
        return None
    return set(output.split())


def merged_pulls(root, start, tag):
    """List (number, title) for every pull request merged in start..tag, oldest first."""
    span = f'{start}..{tag}' if start else tag
    log = git(root, 'log', '--merges', '--reverse', '--topo-order', '--format=%H%x1f%s%x1f%b%x1e', span)
    pulls, seen = [], set()
    for record in log.split('\x1e'):
        if not record.strip():
            continue
        _, subject, body = (record.strip('\n').split('\x1f') + ['', ''])[:3]
        match = _MERGE.fullmatch(subject.strip())
        if not match or match[3] in _PLUMBING_BRANCHES:
            continue
        number = int(match[1])
        if number in seen:
            continue
        seen.add(number)
        title = next((line.strip() for line in body.splitlines() if line.strip()), '') or subject
        pulls.append((number, title))
    return pulls


def classify(title, labels=()):
    """Pick a group: labels first, then the title's type/scope prefix."""
    for label in labels:
        group = _LABELS.get(label.lower())
        if group:
            return group
    match = _PREFIX.fullmatch(title)
    if not match:
        return 'other'
    kind = match[1].strip().lower()
    scopes = [part.strip().lower() for part in (match[2] or '').split(',') if part.strip()]
    if kind in _NOISE:
        return _NOISE[kind]
    for word in [kind, *scopes]:
        if word in _AREAS:
            return _AREAS[word]
    return _KINDS.get(kind, 'other')


def change_type(title, labels=()):
    """Keep a Changelog type: Added for features, Fixed for fixes, else Changed.

    Labels win over the title, as in classify(); an area prefix such as
    `feat(s3):` still counts as an addition here.
    """
    for label in labels:
        if label.lower() == 'bug':
            return 'Fixed'
        if label.lower() == 'enhancement':
            return 'Added'
    match = _PREFIX.fullmatch(title)
    kind = _KINDS.get(match[1].strip().lower()) if match else None
    return {'features': 'Added', 'fixes': 'Fixed'}.get(kind, 'Changed')


def lookup_pulls(repository, numbers, chunk=50):
    """Fetch labels and closing issues for the pull requests, chunk per GraphQL query.

    Returns {} if any lookup fails: the notes then fall back to title prefixes
    and the issue numbers in each title, and publication continues.
    """
    result = {}
    for start in range(0, len(numbers), chunk):
        part = _lookup(repository, numbers[start:start + chunk])
        if part is None:
            return {}
        result.update(part)
    return result


def _lookup(repository, numbers):
    owner, name = repository.split('/', 1)
    fields = ' '.join(f'p{n}: pullRequest(number: {n}) {{ labels(first: 20) {{ nodes {{ name }} }} '
                      f'closingIssuesReferences(first: 20) {{ nodes {{ number }} }} }}' for n in numbers)
    query = f'query($owner: String!, $name: String!) {{ repository(owner: $owner, name: $name) {{ {fields} }} }}'
    try:
        output = subprocess.check_output(['gh', 'api', 'graphql', '-f', 'query=' + query,
                                          '-F', 'owner=' + owner, '-F', 'name=' + name],
                                         text=True, timeout=60)
        data = json.loads(output)['data']['repository']
    except (subprocess.SubprocessError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'Warning: could not read pull request labels for the changelog: {error}', file=sys.stderr, flush=True)
        return None
    result = {}
    for number in numbers:
        value = data.get(f'p{number}') or {}
        result[number] = {
            'labels': sorted(node['name'] for node in value.get('labels', {}).get('nodes', [])),
            'closes': sorted(node['number'] for node in value.get('closingIssuesReferences', {}).get('nodes', []))}
    return result


def display_title(title):
    """Drop a prefix that only names the group the entry is listed under."""
    match = _PREFIX.fullmatch(title)
    if not match:
        return title
    kind = match[1].strip().lower()
    if kind not in _NOISE and kind not in _AREAS and kind not in _KINDS:
        return title
    text = match[3]
    return text[:1].upper() + text[1:]


def entries(pulls, metadata):
    """Combine titles with looked-up labels and closing issues."""
    result = []
    for number, title in pulls:
        known = metadata.get(number)
        trimmed = _TRAILING_REFS.sub('', title) or title
        if known is not None:
            closes = known['closes']
            labels = known['labels']
        else:
            # Without the lookup, the issue numbers a title ends with stand in.
            tail = title[len(trimmed):]
            closes = sorted({int(ref) for ref in _REF.findall(tail)})
            labels = []
        result.append({'number': number, 'title': display_title(trimmed), 'group': classify(title, labels),
                       'type': change_type(title, labels), 'closes': closes})
    return result


def render(tag, previous, items, server, repository, *, fold):
    base = f'{server}/{repository}'

    def link(number, kind):
        return f'[#{number}]({base}/{kind}/{number})'

    if previous:
        compare = f'Full comparison: {base}/compare/{previous}...{tag}'
    else:
        compare = 'This is the first release of its kind, so every merged pull request is listed.'
    lines = ["## What's changed", '']
    delivery = ['## Downloads and sources', '', compare]
    if not items:
        return '\n'.join(lines + ['No pull requests were merged since then.', '', *delivery]) + '\n'
    folded = [item for item in items if fold and item['group'] in FOLDED]
    for key, name in GROUPS:
        listed = sorted((item for item in items if item['group'] == key and item not in folded),
                        key=lambda item: item['number'])
        if not listed:
            continue
        lines += [f'### {name}', '']
        for item in listed:
            line = f"- {item['title']} ({link(item['number'], 'pull')})"
            if item['closes']:
                line += ', closes ' + ', '.join(link(number, 'issues') for number in item['closes'])
            lines.append(line)
        lines.append('')
    if folded:
        kinds = [word for key, word in FOLDED.items() if any(item['group'] == key for item in folded)]
        refs = ', '.join(link(item['number'], 'pull') for item in sorted(folded, key=lambda item: item['number']))
        lines += [f'Also {len(folded)} {_series(kinds)} changes: {refs}.', '']
    return '\n'.join(lines + delivery) + '\n'


KEEP_A_CHANGELOG_TYPES = ('Added', 'Fixed', 'Changed')


def version_section(version, date, previous, items, server, repository):
    """Return (section, link) for CHANGELOG.md in Keep a Changelog 1.1 style.

    The same entries as the release notes, by change type instead of area;
    documentation, lab and test changes are folded into one line. With version
    None the section is `[Unreleased]`: the changes nightly is about to promote.
    """
    base = f'{server}/{repository}'

    def link(number, kind):
        return f'[#{number}]({base}/{kind}/{number})'

    lines = [f'## [{version}] - {date}' if version else '## [Unreleased]', '']
    folded = [item for item in items if item['group'] in FOLDED]
    for name in KEEP_A_CHANGELOG_TYPES:
        listed = sorted((item for item in items if item['type'] == name and item not in folded),
                        key=lambda item: item['number'])
        if not listed:
            continue
        lines += [f'### {name}', '']
        for item in listed:
            line = f"- {item['title']} ({link(item['number'], 'pull')})"
            if item['closes']:
                line += ', closes ' + ', '.join(link(number, 'issues') for number in item['closes'])
            lines.append(line)
        lines.append('')
    if folded:
        kinds = [word for key, word in FOLDED.items() if any(item['group'] == key for item in folded)]
        refs = ', '.join(link(item['number'], 'pull') for item in sorted(folded, key=lambda item: item['number']))
        lines += [f'Also {len(folded)} {_series(kinds)} changes: {refs}.', '']
    if not items:
        lines += ['No pull requests were merged.', '']
    return '\n'.join(lines), version_link(version, previous, server, repository)


def version_link(version, previous, server, repository):
    """The link reference of a CHANGELOG.md section: its comparison with the previous stable release."""
    base = f'{server}/{repository}'
    if not version:
        return '[Unreleased]: ' + (f'{base}/compare/{previous}...nightly' if previous else f'{base}/commits/nightly')
    return f'[{version}]: ' + (f'{base}/compare/{previous}...v{version}' if previous else f'{base}/releases/tag/v{version}')


def _series(words):
    return words[0] if len(words) == 1 else ', '.join(words[:-1]) + ' and ' + words[-1]


def section(root, tag, repository, server='https://github.com', lookup=lookup_pulls, released=None):
    """Return the Markdown changes section for tag, read from root's history.

    released, when given, reads the published release tags (released_tags).
    """
    previous = previous_tag(root, tag, released(repository) if released else None)
    pulls = merged_pulls(root, previous, tag)
    metadata = lookup(repository, [number for number, _ in pulls]) if lookup else {}
    fold = parse_tag(tag).channel != 'nightly'
    return render(tag, previous, entries(pulls, metadata), server, repository, fold=fold)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--repository', required=True, help='owner/name, for links and the label lookup')
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--offline', action='store_true', help='Skip the pull request label lookup')
    args = parser.parse_args()
    sys.stdout.write(section(args.root, args.tag, args.repository, lookup=None if args.offline else lookup_pulls,
                             released=None if args.offline else released_tags))


if __name__ == '__main__':
    main()
