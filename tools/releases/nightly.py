#!/usr/bin/env python3
"""Name, tag and verify the scheduled nightly for the clean nightly branch HEAD.

A nightly is skipped only when the exact source is already a complete, verified
public nightly. Creating a tag never moves an existing one.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from github_api import GitHub, full_sha, matching_tags, remote_tag_commit, tag_exists
from release_bundle import expected_members
from proton_sources import PART_NAMES
from debug_symbols import asset_name as debug_asset_name
from release_identity import calendar_base, parse_tag
from verify_tag import verify
import support_assets

NIGHTLY_REF = 'refs/heads/nightly'


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def prepare(root):
    """Derive the immutable nightly identity from the clean full-history checkout.

    The tag is `vYY.MDD.N-nightly` (#373): HEAD's UTC committer date and commit count.
    """
    if (git(root, 'rev-parse', '--is-shallow-repository') != 'false'
            or git(root, 'status', '--porcelain', '--untracked-files=normal')):
        raise ValueError('Preparation requires clean source and full Git history')
    sha = full_sha(git(root, 'rev-parse', 'HEAD'))
    count = int(git(root, 'rev-list', '--count', 'HEAD'))
    base = calendar_base(int(git(root, 'show', '-s', '--format=%ct', 'HEAD')), count)
    tag = f'v{base}-nightly'
    parse_tag(tag)
    return {'schemaVersion': 4, 'tag': tag, 'sourceSha': sha, 'baseVersion': base,
            'gitTree': git(root, 'rev-parse', 'HEAD^{tree}'), 'commitCount': count,
            'committerDate': committer_date(root, 'HEAD')}


def committer_date(root, revision):
    """UTC calendar date of a source commit, for release titles and summaries."""
    return datetime.datetime.fromtimestamp(int(git(root, 'show', '-s', '--format=%ct', revision)),
                                           datetime.timezone.utc).strftime('%Y-%m-%d')


def ensure_tag(api, root, tag, source_sha):
    # A missing tag is the sole create case. Every other API failure stays fatal.
    try:
        remote = remote_tag_commit(api, tag)
    except subprocess.CalledProcessError:
        if tag_exists(api, tag):
            raise ValueError('Existing remote tag could not be verified')
        api.json('git/refs', method='POST', data={'ref': 'refs/tags/' + tag, 'sha': full_sha(source_sha)})
        remote = remote_tag_commit(api, tag)
    if remote != source_sha:
        raise ValueError('Immutable release tag already identifies another source: ' + tag)
    subprocess.run(['git', '-C', str(root), 'fetch', '--no-tags', 'origin',
                    'refs/tags/' + tag + ':refs/tags/' + tag], check=True, timeout=120)
    return verify(root, tag)


def public_inventory(tag, layout=None):
    identity = parse_tag(tag)
    if layout == support_assets.LAYOUT:
        return support_assets.inventory(identity.version)
    if layout is not None:
        raise ValueError('Unknown release asset layout')
    return set(expected_members(identity.version) + ['putgd-' + identity.version + '-release.zip', 'SHA256SUMS', *PART_NAMES,
                                                      debug_asset_name(identity.version)])


def published_nightly(api, tag, release):
    if parse_tag(tag).channel != 'nightly' or release.get('channel') != 'nightly':
        raise ValueError('Only the nightly publication class can establish a skip')
    pages = api.json('releases?per_page=100', paginate=True)
    matches = [value for page in pages for value in page if value.get('tag_name') == tag]
    if not matches:
        return None
    if len(matches) != 1:
        raise ValueError('Ambiguous matching nightly releases')
    value = matches[0]
    if value.get('draft') is True:
        return None
    if value.get('draft') is not False or value.get('prerelease') is not True:
        raise ValueError('Matching public release is not a nightly prerelease')
    if remote_tag_commit(api, tag) != release['gitCommit']:
        raise ValueError('Public tag differs from pinned source')
    assets = value['assets']
    manifests = [a for a in assets if a['name'] == 'release-builds.json']
    if (len(manifests) != 1 or manifests[0].get('state') != 'uploaded'
            or type(manifests[0].get('size')) is not int or manifests[0]['size'] <= 0):
        raise ValueError('Public nightly has no unique source manifest')
    manifest_bytes = api.asset_bytes(manifests[0]['id'])
    manifest = json.loads(manifest_bytes)
    inventory = public_inventory(tag, manifest.get('assetLayout'))
    if (len(assets) != len(inventory) or {asset['name'] for asset in assets} != inventory
            or any(asset.get('state') != 'uploaded' or type(asset.get('size')) is not int
                   or asset['size'] <= 0 or type(asset.get('id')) is not int or asset['id'] <= 0 for asset in assets)):
        raise ValueError('Public nightly asset inventory is incomplete or ambiguous')
    by_name = {asset['name']: asset for asset in assets}
    def download(name):
        data = api.asset_bytes(by_name[name]['id'])
        if len(data) != by_name[name]['size']:
            raise ValueError('Public nightly asset size mismatch')
        return data
    checksums = download('SHA256SUMS')
    expected = {}
    for line in checksums.decode('ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not match or match[2] in expected or match[2] not in inventory - {'SHA256SUMS'}:
            raise ValueError('Invalid public nightly checksum inventory')
        expected[match[2]] = match[1]
    if set(expected) != inventory - {'SHA256SUMS'}:
        raise ValueError('Incomplete public nightly checksum inventory')
    if len(manifest_bytes) != by_name['release-builds.json']['size']:
        raise ValueError('Public nightly manifest size mismatch')
    downloaded = {'SHA256SUMS': checksums, 'release-builds.json': manifest_bytes}
    expected['SHA256SUMS'] = hashlib.sha256(checksums).hexdigest()
    for name, asset in by_name.items():
        digest = asset.get('digest')
        if digest is not None and digest != 'sha256:' + expected[name]:
            raise ValueError('Public nightly server digest differs from checksum')
        if name in downloaded or digest is None:
            data = downloaded.get(name) if name in downloaded else download(name)
            if hashlib.sha256(data).hexdigest() != expected[name]:
                raise ValueError('Public nightly bytes differ from checksum')
    source = json.loads(downloaded['release-builds.json'])['release']
    if any(source.get(key) != val for key, val in release.items()):
        raise ValueError('Public nightly channel/source receipt differs from pinned source')
    if manifest.get('assetLayout') == support_assets.LAYOUT:
        linked = {name: download(name) for name in ('source-delivery.json', 'proton-source-catalog.json')}
        if any(hashlib.sha256(data).hexdigest() != expected[name] for name, data in linked.items()):
            raise ValueError('Linked source metadata differs from checksum')
        delivery = json.loads(linked['source-delivery.json'])
        if delivery != manifest.get('sources'):
            raise ValueError('Linked source receipt differs from release manifest')
        support_assets.verify_public(api, delivery, release)
        support_assets.verify_proton(api, json.loads(linked['proton-source-catalog.json']))
    def snapshot(metadata):
        return {**{key: metadata.get(key) for key in ('id', 'tag_name', 'draft', 'prerelease')},
                'assets': sorted(({key: asset.get(key) for key in
                     ('id', 'name', 'size', 'state', 'updated_at', 'digest')}
                     for asset in metadata['assets']), key=lambda asset: asset['name'])}
    after = api.json('releases/' + str(value['id']))
    if (snapshot(after) != snapshot(value) or remote_tag_commit(api, tag) != release['gitCommit']):
        raise ValueError('Public nightly release changed during verification')
    return {'releaseId': value['id'], 'tag': tag, 'sourceSha': release['gitCommit'], 'assetSha256': expected}


def newest_nightly(api):
    """The newest existing nightly tag, by version (pre-#373 ones included), or None."""
    found = []
    for name in matching_tags(api, 'v'):
        try:
            identity = parse_tag(name)
        except ValueError:
            continue
        if identity.channel == 'nightly':
            found.append((identity.key, name))
    return max(found)[1] if found else None


def plan(api, root, ref):
    """Skip a published source; otherwise tag HEAD, which must sort after every earlier nightly."""
    if ref != NIGHTLY_REF:
        raise ValueError('Nightlies are cut from the nightly branch only, not ' + ref)
    prepared = prepare(root)
    tag = prepared['tag']
    if tag_exists(api, tag):
        public = published_nightly(api, tag, ensure_tag(api, root, tag, prepared['sourceSha']))
        if public:
            return {**prepared, 'skip': True, 'publishedNightly': public}
    else:
        # A rewritten committer date could name a version older than a published nightly.
        newest = newest_nightly(api)
        if newest and parse_tag(newest).key >= parse_tag(tag).key:
            raise ValueError(f'{tag} would not sort after the existing nightly {newest}; '
                             'check the committer date of nightly HEAD')
    ensure_tag(api, root, tag, prepared['sourceSha'])
    return {**prepared, 'skip': False, 'publishedNightly': None}


def write_outputs(result):
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as stream:
            stream.write(f"tag={result['tag']}\nsource_sha={result['sourceSha']}\n")
            stream.write(f"skip={str(result['skip']).lower()}\n")
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as stream:
            if result['skip']:
                stream.write(f"Nightly unchanged: `{result['tag']}` is already published. Nothing to build.\n")
            else:
                stream.write(f"Building nightly `{result['tag']}` from `{result['sourceSha'][:12]}`, "
                             f"committed {result['committerDate']}.\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('plan',))
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = plan(GitHub(os.environ['GITHUB_REPOSITORY']), args.root, os.environ['GITHUB_REF'])
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    write_outputs(result)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
