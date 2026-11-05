#!/usr/bin/env python3
"""Re-derive the URLs, SHA-256s and commits that go with each pinned version (#420).

Renovate (.github/renovate.json) changes only version fields, then runs this as
its post-upgrade task; run it by hand after editing a version too. Commits come
from the upstream tags (git ls-remote, and tree-only fetches for the AWS SDK's
submodules); digests come from hashing the downloads, checked against upstream's
checksum file where one is published. Nothing downloaded is executed, so the
digest of the zsyncmake inside appimagetool (packaging/appimage/tools.lock.json)
stays a manual step; the AppImage job reports a stale one.
"""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
TOOL_LOCKS = ('packaging/appimage/tools.lock.json', 'packaging/sbom/tools.lock.json')
APPIMAGE_LOCK = 'packaging/appimage/tools.lock.json'
DEPENDENCY_LOCK = 'cmake/dependency-lock.json'


class Upstream:
    """Tag commits and downloads, cached for one run."""

    def __init__(self):
        self.tags = {}
        self.fetched = {}
        self.work = tempfile.TemporaryDirectory(prefix='putgd-pins-')

    def tag_commits(self, repository):
        url = repository if repository.startswith('https://') else 'https://github.com/' + repository
        if url not in self.tags:
            output = subprocess.check_output(['git', 'ls-remote', '--tags', url], text=True, timeout=300)
            found = {}
            for line in output.splitlines():
                commit, ref = line.split('\t')
                name = ref.removeprefix('refs/tags/')
                # An annotated tag's peeled entry (^{}) names its commit.
                if name.endswith('^{}'):
                    found[name[:-3]] = commit
                else:
                    found.setdefault(name, commit)
            self.tags[url] = found
        return self.tags[url]

    def release_commit(self, repository, release):
        tags = self.tag_commits(repository)
        for tag in (release, 'v' + release, 'n' + release, 'release-' + release.replace('.', '-')):
            if tag in tags:
                return tags[tag]
        raise ValueError(f'No tag for {release} in {repository}')

    def read(self, url):
        request = urllib.request.Request(url, headers={'User-Agent': 'putgd-refresh-pins'})
        with urllib.request.urlopen(request, timeout=300) as response:
            return response.read()

    def sha256(self, url):
        return hashlib.sha256(self.read(url)).hexdigest()

    def published_sha256(self, url, asset):
        """The digest upstream's checksum file lists for asset, if it publishes one."""
        name, version = asset.split('_')[:2] if asset.count('_') >= 2 else (None, None)
        if not name:
            return None
        try:
            listing = self.read(url.rsplit('/', 1)[0] + f'/{name}_{version}_checksums.txt').decode()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise
        for line in listing.splitlines():
            digest, _, listed = line.partition('  ')
            if listed.strip() == asset:
                return digest
        raise ValueError(f'{asset} is missing from its release checksum file')

    def checkout(self, repository, commit):
        """A shallow, blob-less, tree-less fetch of one commit; trees load as paths are read."""
        key = (repository, commit)
        if key not in self.fetched:
            work = tempfile.mkdtemp(prefix='repo-', dir=self.work.name)
            for args in (('init', '-q'), ('remote', 'add', 'origin', repository),
                         ('fetch', '-q', '--depth', '1', '--filter=tree:0', 'origin', commit)):
                subprocess.run(['git', '-C', work, *args], check=True, timeout=300)
            self.fetched[key] = work
        return self.fetched[key]

    def gitmodules(self, repository, commit):
        result = subprocess.run(['git', '-C', self.checkout(repository, commit), 'show', commit + ':.gitmodules'],
                                capture_output=True, text=True, timeout=300)
        return result.stdout if result.returncode == 0 else ''

    def gitlink(self, repository, commit, path):
        entry = subprocess.check_output(['git', '-C', self.checkout(repository, commit), 'ls-tree', commit, '--', path],
                                        text=True, timeout=300).split()
        if len(entry) < 3 or entry[1] != 'commit':
            raise ValueError(f'{path} is not a submodule in {repository} at {commit}')
        return entry[2]


def refresh_tools(lock, upstream):
    """Release-asset tools: each URL names its release, each digest its asset."""
    changes = []
    for tool in lock['tools']:
        repository, release = tool['repository'], tool['release']
        if 'commit' in tool:
            commit = upstream.release_commit(repository, release)
            if commit != tool['commit']:
                changes.append(f"{tool['name']}: commit {tool['commit'][:12]} -> {commit[:12]}")
                tool['commit'] = commit
        url = tool['url']
        if url.startswith('https://raw.githubusercontent.com/'):
            url = re.sub(r'/[0-9a-f]{40}/', '/' + tool['commit'] + '/', url)
        else:
            match = re.fullmatch(r'https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)/([^/]+)', url)
            if not match or match[1] != repository:
                raise ValueError('Unexpected tool URL: ' + url)
            asset = match[3].replace(match[2].removeprefix('v'), release.removeprefix('v'))
            url = f'https://github.com/{repository}/releases/download/{release}/{asset}'
        if url != tool['url']:
            digest = upstream.sha256(url)
            published = upstream.published_sha256(url, url.rsplit('/', 1)[1])
            if published is not None and published != digest:
                raise ValueError(f'{url} differs from its published checksum')
            changes.append(f"{tool['name']}: {tool['url']} -> {url}")
            tool['url'], tool['sha256'] = url, digest
    return changes


def refresh_build_inputs(lock, upstream):
    """Source pins built or read by the AppImage job: each commit is its release tag's."""
    inputs = lock['buildInputs']
    qt = {item['release'] for item in inputs['qtLicenseSources']}
    if qt != {inputs['qt']}:
        raise ValueError(f"Qt {inputs['qt']} differs from its licence sources {sorted(qt)}; update them together")
    changes = []
    for item in [*inputs['qtLicenseSources'], *(value for value in inputs.values() if isinstance(value, dict))]:
        if 'release' not in item or 'commit' not in item:
            continue
        commit = upstream.release_commit(item['repository'], item['release'])
        if commit != item['commit']:
            changes.append(f"{item['repository']} {item['release']}: commit {item['commit'][:12]} -> {commit[:12]}")
            item['commit'] = commit
    return changes


def submodules(upstream, repository, commit, prefix=''):
    """Every submodule path and commit below one commit, as `git submodule status --recursive` lists them."""
    found = []
    text = upstream.gitmodules(repository, commit)
    if not text:
        return found
    parser = configparser.ConfigParser()
    # .gitmodules indents its keys, which configparser would read as continuation lines.
    parser.read_string('\n'.join(line.strip() for line in text.splitlines()))
    for section in parser.sections():
        path, url = parser[section]['path'], parser[section]['url']
        # A relative URL is relative to the superproject's URL, as to a directory.
        url = urllib.parse.urljoin(repository.rstrip('/') + '/', url)
        pinned = upstream.gitlink(repository, commit, path)
        found.append({'path': prefix + path, 'commit': pinned})
        found += submodules(upstream, url, pinned, prefix + path + '/')
    return found


def refresh_aws(lock, upstream):
    sdk = lock['awsSdk']
    commit = upstream.release_commit(sdk['repository'], sdk['release'])
    if commit == sdk['commit']:
        return []
    pinned = sorted(submodules(upstream, sdk['repository'], commit), key=lambda item: item['path'])
    changes = [f"aws-sdk-cpp {sdk['release']}: commit {sdk['commit'][:12]} -> {commit[:12]}"]
    before = {item['path']: item['commit'] for item in sdk['submodules']}
    for item in pinned:
        if before.get(item['path']) != item['commit']:
            changes.append(f"  {item['path']}: {(before.get(item['path']) or 'new')[:12]} -> {item['commit'][:12]}")
    changes += [f'  {path}: removed' for path in sorted(set(before) - {item['path'] for item in pinned})]
    sdk['commit'], sdk['submodules'] = commit, pinned
    return changes


def refresh(root, upstream):
    changes = {}
    for name in TOOL_LOCKS:
        path = root / name
        lock = json.loads(path.read_text())
        found = refresh_tools(lock, upstream)
        if name == APPIMAGE_LOCK:
            found += refresh_build_inputs(lock, upstream)
        if found:
            path.write_text(json.dumps(lock, indent=2) + '\n')
            changes[name] = found
    path = root / DEPENDENCY_LOCK
    lock = json.loads(path.read_text())
    found = refresh_aws(lock, upstream)
    if found:
        path.write_text(json.dumps(lock, indent=2) + '\n')
        changes[DEPENDENCY_LOCK] = found
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    changes = refresh(args.root, Upstream())
    for name, lines in changes.items():
        print(name)
        for line in lines:
            print('  ' + line)
    if not changes:
        print('Every derived pin matches its version.')


if __name__ == '__main__':
    main()
