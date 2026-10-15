#!/usr/bin/env python3
"""Pin one nightly source, retain its preparation, and coordinate exact-tag CI."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import quote

from github_evidence import (GitHub, BUILD_JOBS, BUILD_WORKFLOW,
                             coordinate_build, coordinate_packages, full_sha, remote_tag_commit, verify_ci)
from release_bundle import expected_members
from proton_sources import PART_NAMES
from release_identity import parse_tag
from verify_tag import verify


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def branch_commit(api, branch):
    value = api.json('git/ref/heads/' + branch)['object']
    if value.get('type') != 'commit':
        raise ValueError('Branch must resolve to a source commit: ' + branch)
    return full_sha(value['sha'])


def sync_main(api):
    # Resolve current main so delayed push events cannot rewind nightly.
    main = branch_commit(api, 'main')
    current = branch_commit(api, 'nightly')
    if current == main:
        return current
    status = api.json('compare/' + current + '...' + main + '?per_page=1')['status']
    if status == 'behind':
        return current  # Nightly already contains main and has additional work.
    if status != 'ahead':
        raise ValueError('Cannot fast-forward nightly to main; reconcile divergent history through review')
    # GitHub rechecks ancestry at mutation time, including concurrent pushes.
    updated = api.json('git/refs/heads/nightly', method='PATCH', data={'sha': main, 'force': False})
    value = updated['object']
    if value.get('type') != 'commit' or full_sha(value['sha']) != main:
        raise ValueError('Cannot verify updated nightly source')
    return main


def pin(api, run_id, attempt, receipt_path):
    if type(run_id) is not int or run_id <= 0 or type(attempt) is not int or attempt <= 0:
        raise ValueError('Valid orchestration run ID and attempt required')
    if attempt > 1:
        if not receipt_path.is_file() or receipt_path.is_symlink():
            raise ValueError('Original nightly preparation receipt is missing; retry cannot retarget')
        receipt = json.loads(receipt_path.read_text())
        if (receipt.get('schemaVersion') != 1 or receipt.get('runId') != run_id
                or receipt.get('repository') != api.repository or not receipt.get('tag')):
            raise ValueError('Original nightly preparation receipt contradicts this run')
        full_sha(receipt.get('sourceSha'))
        if parse_tag(receipt['tag']).channel != 'nightly':
            raise ValueError('Original nightly preparation receipt has wrong channel')
        return receipt
    # Actions may replace a pending main run with another trigger. Every new
    # run catches up main, then pins once; retries above never retarget.
    source = sync_main(api)
    return {'schemaVersion': 1, 'repository': api.repository, 'runId': run_id, 'sourceSha': source}


def prepare(root, receipt):
    if (git(root, 'rev-parse', 'HEAD') != full_sha(receipt['sourceSha'])
            or git(root, 'rev-parse', '--is-shallow-repository') != 'false'
            or git(root, 'status', '--porcelain', '--untracked-files=normal')):
        raise ValueError('Preparation requires exact clean source and full Git history')
    project = re.search(r'project\(\s*putgd\s+VERSION\s+([0-9.]+)', (root / 'CMakeLists.txt').read_text())
    if not project:
        raise ValueError('Missing project base version')
    count = int(git(root, 'rev-list', '--count', 'HEAD'))
    utc = datetime.datetime.fromtimestamp(int(git(root, 'show', '-s', '--format=%ct', 'HEAD')),
                                         datetime.timezone.utc).strftime('%Y%m%d%H%M%S')
    tag = f'v{project[1]}-dev.{count}.{utc}+g{receipt["sourceSha"][:12]}'
    parse_tag(tag)
    prepared = {**receipt, 'tag': tag, 'gitTree': git(root, 'rev-parse', 'HEAD^{tree}'),
                'commitCount': count, 'committerUtc': utc}
    if 'tag' in receipt and prepared != receipt:
        raise ValueError('Original preparation contradicts pinned source identity')
    return prepared


def ensure_tag(api, root, tag, source_sha):
    # A missing tag is the sole create case. Every other API failure stays fatal.
    try:
        remote = remote_tag_commit(api, tag)
    except subprocess.CalledProcessError:
        refs = api.json('git/matching-refs/tags/' + quote(tag, safe=''))
        if any(ref.get('ref') == 'refs/tags/' + tag for ref in refs):
            raise ValueError('Existing remote tag could not be verified')
        api.json('git/refs', method='POST', data={'ref': 'refs/tags/' + tag, 'sha': full_sha(source_sha)})
        remote = remote_tag_commit(api, tag)
    if remote != source_sha:
        raise ValueError('Immutable nightly tag already identifies another source')
    subprocess.run(['git', '-C', str(root), 'fetch', '--no-tags', 'origin',
                    'refs/tags/' + tag + ':refs/tags/' + tag], check=True, timeout=120)
    return verify(root, tag)


def public_inventory(tag):
    identity = parse_tag(tag)
    return set(expected_members(identity.version) + ['putgd-' + identity.version + '-release.zip', 'SHA256SUMS', *PART_NAMES])


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
    inventory = public_inventory(tag)
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
    downloaded = {'SHA256SUMS': checksums, 'release-builds.json': download('release-builds.json')}
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
    def snapshot(metadata):
        return {**{key: metadata.get(key) for key in ('id', 'tag_name', 'draft', 'prerelease')},
                'assets': sorted(({key: asset.get(key) for key in
                     ('id', 'name', 'size', 'state', 'updated_at', 'digest')}
                     for asset in metadata['assets']), key=lambda asset: asset['name'])}
    after = api.json('releases/' + str(value['id']))
    if (snapshot(after) != snapshot(value) or remote_tag_commit(api, tag) != release['gitCommit']):
        raise ValueError('Public nightly release changed during verification')
    return {'releaseId': value['id'], 'tag': tag, 'sourceSha': release['gitCommit'], 'assetSha256': expected}


def build_mode(root, event_name, ref, package_tag):
    if event_name == 'push' and ref == 'refs/heads/nightly':
        return True
    if package_tag:
        if event_name != 'workflow_dispatch' or ref != 'refs/tags/' + package_tag:
            raise ValueError('Coordinated Build must execute the exact tag workflow recipe')
        verify(root, package_tag)
        return True
    return False


def release_ci(api, root, tag, ref, source_sha='', build_run_id='', build_workflow_id=''):
    if ref != 'refs/tags/' + tag:
        raise ValueError('Package dispatch must execute the exact tag workflow recipe')
    release = verify(root, tag)
    if source_sha and source_sha != release['gitCommit']:
        raise ValueError('Package source differs from coordinator')
    if build_run_id:
        receipt = verify_ci(api, int(build_run_id), release['gitCommit'], BUILD_WORKFLOW, BUILD_JOBS)
        if str(receipt['workflowId']) != build_workflow_id:
            raise ValueError('Build workflow ID differs from coordinator')
        return receipt
    if build_workflow_id:
        raise ValueError('Incomplete coordinator producer identity')
    return coordinate_build(api, release['gitCommit'], tag)


def execute(api, root, receipt):
    prepared = prepare(root, receipt)
    release = ensure_tag(api, root, prepared['tag'], prepared['sourceSha'])
    public = published_nightly(api, prepared['tag'], release)
    result = {'schemaVersion': 1, 'preparation': prepared, 'publishedNightly': public}
    if public:
        result['skipped'] = True
        return result
    # Release's CI coordinator observes Build in parallel with native/AppImage jobs.
    result['packageDispatch'] = coordinate_packages(api, prepared['sourceSha'], prepared['tag'],
                                                    prepared['runId'], {})
    if result['packageDispatch']['status'] == 'completed':
        # Publication may have finished after our first public snapshot.
        result['publishedNightly'] = published_nightly(api, prepared['tag'], release)
        if not result['publishedNightly']:
            raise ValueError('Original package run completed without verified public nightly: '
                             + str(result['packageDispatch']['runId']))
    return result


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('pin', 'prepare', 'execute', 'build-mode', 'release-ci'))
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--receipt', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.command == 'build-mode':
        result = {'coordinated': str(build_mode(args.root, os.environ['GITHUB_EVENT_NAME'],
                  os.environ['GITHUB_REF'], os.environ.get('PACKAGE_TAG', ''))).lower()}
    else:
        api = GitHub(os.environ['GITHUB_REPOSITORY'])
        if args.command == 'pin':
            attempt = int(os.environ['GITHUB_RUN_ATTEMPT'])
            if attempt > 1:
                subprocess.run(['gh', 'run', 'download', os.environ['GITHUB_RUN_ID'],
                    '--repo', api.repository, '--name', 'nightly-preparation-' + os.environ['GITHUB_RUN_ID'],
                    '--dir', str(args.receipt.parent)], check=True, timeout=60)
            result = pin(api, int(os.environ['GITHUB_RUN_ID']), attempt, args.receipt)
            if attempt == 1:
                write(args.receipt, result)
        elif args.command == 'prepare':
            result = prepare(args.root, json.loads(args.receipt.read_text()))
            write(args.receipt, result)
        elif args.command == 'execute':
            result = execute(api, args.root, json.loads(args.receipt.read_text()))
        else:
            result = release_ci(api, args.root, os.environ['RELEASE_TAG'], os.environ['GITHUB_REF'],
                os.environ.get('SOURCE_SHA', ''), os.environ.get('BUILD_RUN_ID', ''),
                os.environ.get('BUILD_WORKFLOW_ID', ''))
    if args.output:
        write(args.output, result)
    outputs = dict(result)
    for key in ('sourceSha', 'runId', 'workflowId'):
        if key in result:
            outputs[{'sourceSha': 'source_sha', 'runId': 'build_run_id', 'workflowId': 'build_workflow_id'}[key]] = result[key]
    if os.environ.get('GITHUB_OUTPUT'):
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as stream:
            for key, value in outputs.items():
                if isinstance(value, (str, int)):
                    stream.write(f'{key}={value}\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
