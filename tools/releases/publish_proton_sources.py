#!/usr/bin/env python3
"""Publish pinned Proton source parts as a prerelease, never a stable installer."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit

from github_api import GitHub, full_sha, remote_tag_commit, tag_exists
from proton_sources import PART_NAMES, catalog_value
from support_assets import sha256, verify_metadata


def create_command(repository, tag, commit, notes):
    return ['gh', 'release', 'create', tag, '--repo', repository, '--target', commit,
            '--draft', '--prerelease', '--latest=false', '--title', tag,
            '--notes-file', str(notes)]


def publish_command(repository, tag):
    return ['gh', 'release', 'edit', tag, '--repo', repository, '--draft=false',
            '--prerelease', '--latest=false']


def verify_checksums(path, catalog):
    expected = {item['file']: item['sha256'] for item in catalog['delivery']['parts']}
    if 'file' in catalog['delivery']:
        expected[catalog['delivery']['file']] = catalog['delivery']['sha256']
    expected.update({item['filename']: item['sha256'] for item in catalog.get('artifacts', [])})
    records = {}
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]*)', line)
        if not match or match[2] in records:
            raise ValueError('Invalid or duplicate source checksum index entry')
        records[match[2]] = match[1]
    if records != expected:
        raise ValueError('Source checksum index differs from catalog')


def publish(api, catalog, directory, commit, notes):
    commit = full_sha(commit)
    if commit != full_sha(catalog['publication'].get('sourceTagCommit')):
        raise ValueError('Source commit differs from catalog publication pin')
    url = urlsplit(catalog['publication']['releaseUrl'])
    prefix = '/' + api.repository + '/releases/tag/'
    if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment
            or not url.path.startswith(prefix)):
        raise ValueError('Source publication must name this repository on GitHub')
    tag = unquote(url.path[len(prefix):])
    if not re.fullmatch(r'proton-sources-[0-9.]+-crypto[0-9.]+-bun[0-9.]+', tag):
        raise ValueError('Expected a versioned Proton source tag')
    expected = {item['file']: item for item in catalog['delivery']['parts']}
    if set(expected) != set(PART_NAMES):
        raise ValueError('Both pinned source parts are required')
    for name, item in expected.items():
        path = directory / name
        if (path.is_symlink() or not path.is_file() or path.stat().st_size != item['bytes']
                or sha256(path) != item['sha256']):
            raise ValueError('Local source part differs from catalog')
    checksums = directory / 'SHA256SUMS'
    if checksums.is_symlink() or not checksums.is_file():
        raise ValueError('Source checksum index required')
    verify_checksums(checksums, catalog)
    expected['SHA256SUMS'] = {'bytes': checksums.stat().st_size, 'sha256': sha256(checksums)}

    def verify_tag():
        if tag_exists(api, tag) and remote_tag_commit(api, tag) != commit:
            raise ValueError('Remote source tag targets another commit')

    def metadata():
        matches = [r for page in api.json('releases?per_page=100', paginate=True) for r in page
                   if r['tag_name'] == tag]
        if len(matches) != 1:
            raise ValueError('Expected exactly one Proton source release')
        value = matches[0]
        if value.get('prerelease') is not True:
            raise ValueError('Proton source release must be a prerelease, never stable/Latest')
        return value

    verify_tag()
    matches = [r for page in api.json('releases?per_page=100', paginate=True) for r in page
               if r['tag_name'] == tag]
    if not matches:
        subprocess.run(create_command(api.repository, tag, commit, notes), check=True)
    value = metadata()
    if value.get('draft') is True:
        if value.get('target_commitish') != commit:
            raise ValueError('Source draft targets another commit')
        verify_metadata(api, value, expected, complete=False)
        missing = set(expected) - {a['name'] for a in value['assets']}
        if missing:
            subprocess.run(['gh', 'release', 'upload', tag, '--repo', api.repository,
                            *[str(directory / name) for name in sorted(missing)]], check=True, timeout=3600)
        value = metadata()
        if value.get('draft') is not True or value.get('target_commitish') != commit:
            raise ValueError('Source draft changed before publication')
        verify_metadata(api, value, expected)
        verify_tag()
        subprocess.run(publish_command(api.repository, tag), check=True)
    value = metadata()
    if value.get('draft') is not False or remote_tag_commit(api, tag) != commit:
        raise ValueError('Public source release identity differs')
    verify_metadata(api, value, expected)
    return {'tag': tag, 'releaseId': value['id'], 'prerelease': True,
            'latestRequested': False, 'gitCommit': commit, 'published': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--notes', type=Path, required=True)
    parser.add_argument('--repository', default='patrick-hudson/put.gd-desktop')
    args = parser.parse_args()
    print(json.dumps(publish(GitHub(args.repository), catalog_value(args.catalog),
                             args.directory, args.commit, args.notes)))


if __name__ == '__main__':
    main()
