#!/usr/bin/env python3
"""Publish immutable, linked source downloads separately from the installers."""
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import quote, unquote, urlsplit

from audit_native import sha256
from github_api import full_sha, remote_tag_commit, tag_exists

LAYOUT = 'installers-v1'


def source_tag(release):
    # Download-site channel selection recognizes a terminal -nightly suffix.
    # A source-only release must never be mistaken for the installer release.
    return 'sources-' + release['tag'] + '-files'


def inventory(version):
    return {*(f'putgd-{version}-{distro}-x86_64{suffix}' for distro, suffix in (
        ('ubuntu24.04', '.deb'), ('ubuntu26.04', '.deb'), ('debian13', '.deb'), ('fedora44', '.rpm'))),
        f'putgd-{version}-x86_64.AppImage', f'putgd-{version}-x86_64.AppImage.zsync',
        *(f'putgd-{version}-{label}.cdx.json' for label in
          ('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44', 'appimage')),
        'SHA256SUMS', 'release-builds.json', 'source-delivery.json', 'proton-source-catalog.json'}


def describe(directory, release, repository):
    """Bind source downloads to this exact release and include their checksum file."""
    tag = source_tag(release)
    assets = {p.name: {'sha256': sha256(p), 'bytes': p.stat().st_size}
              for p in sorted(directory.iterdir()) if p.is_file() and not p.is_symlink()}
    checksums = directory / 'SHA256SUMS'
    checksums.write_text(''.join(f'{item["sha256"]}  {name}\n' for name, item in assets.items()))
    assets[checksums.name] = {'sha256': sha256(checksums), 'bytes': checksums.stat().st_size}
    return {'schemaVersion': 1, 'repository': repository, 'tag': tag,
            'gitCommit': release['gitCommit'],
            'url': f'https://github.com/{repository}/releases/tag/{tag}', 'assets': assets}


def validate(delivery, release, repository):
    if (delivery.get('schemaVersion') != 1 or delivery.get('repository') != repository
            or delivery.get('tag') != source_tag(release)
            or delivery.get('gitCommit') != full_sha(release['gitCommit'])
            or delivery.get('url') != f'https://github.com/{repository}/releases/tag/{delivery["tag"]}'):
        raise ValueError('Source delivery differs from release identity')
    version = release['version']
    expected = {f'putgd-{version}-source.tar.gz', f'putgd-{version}-appimage-complete-source.tar.gz',
                f'putgd-gnome50-{version}.zip', 'SHA256SUMS'}
    if set(delivery.get('assets', {})) != expected:
        raise ValueError('Source delivery inventory is incomplete')
    for item in delivery['assets'].values():
        if (type(item.get('bytes')) is not int or item['bytes'] <= 0
                or not re.fullmatch('[0-9a-f]{64}', str(item.get('sha256', '')))):
            raise ValueError('Invalid source delivery checksum or size')


def verify_asset(api, asset, expected):
    """Use GitHub's SHA-256, falling back to a bounded disk download on older assets."""
    if (type(asset.get('id')) is not int or asset['id'] <= 0
            or asset.get('state') != 'uploaded' or asset.get('size') != expected['bytes']):
        raise ValueError('Source asset is incomplete or has another size')
    digest = asset.get('digest')
    if digest is not None:
        if digest != 'sha256:' + expected['sha256']:
            raise ValueError('Source asset digest differs from pinned bytes')
        return
    with tempfile.TemporaryDirectory(prefix='pg-source-verify-') as temp:
        path = Path(temp) / 'asset'
        with path.open('wb') as stream:
            subprocess.run(['gh', 'api', '-H', 'Accept: application/octet-stream',
                            f'repos/{api.repository}/releases/assets/{asset["id"]}'],
                           check=True, stdout=stream, timeout=900)
        if path.stat().st_size != expected['bytes'] or sha256(path) != expected['sha256']:
            raise ValueError('Source asset bytes differ from pinned bytes')


def snapshot(metadata):
    return {**{k: metadata.get(k) for k in ('id', 'tag_name', 'draft', 'prerelease')},
            'assets': sorted(({k: a.get(k) for k in ('id', 'name', 'size', 'state', 'digest', 'updated_at')}
                              for a in metadata['assets']), key=lambda a: a['name'])}


def verify_metadata(api, metadata, expected, *, complete=True):
    names = [asset['name'] for asset in metadata['assets']]
    if (len(set(names)) != len(names) or not set(names).issubset(expected)
            or (complete and set(names) != set(expected))):
        raise ValueError('Source release inventory differs from collected files')
    for asset in metadata['assets']:
        verify_asset(api, asset, expected[asset['name']])
    after = api.json('releases/' + str(metadata['id']))
    if snapshot(after) != snapshot(metadata):
        raise ValueError('Source release changed during verification')


def verify_proton(api, catalog):
    """The large unchanged sources already have a public, checksum-pinned home."""
    for item in catalog['delivery']['parts']:
        url = urlsplit(item.get('url', ''))
        prefix = '/' + api.repository + '/releases/download/'
        if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment
                or not url.path.startswith(prefix)):
            raise ValueError('Proton source link must name this repository on GitHub')
        tail = url.path[len(prefix):].split('/')
        if len(tail) != 2 or unquote(tail[1]) != item['file']:
            raise ValueError('Proton source URL differs from catalog file')
        metadata = api.json('releases/tags/' + quote(unquote(tail[0]), safe=''))
        matches = [asset for asset in metadata['assets'] if asset['name'] == item['file']]
        if (metadata.get('draft') is not False or metadata.get('prerelease') is not True
                or len(matches) != 1):
            raise ValueError('Proton source must be publicly available as a prerelease, never stable/Latest')
        verify_asset(api, matches[0], item)
        if snapshot(api.json('releases/' + str(metadata['id']))) != snapshot(metadata):
            raise ValueError('Proton source release changed during verification')


def publish(api, delivery, release, directory):
    """Resume a matching source draft; never replace existing tags or assets."""
    validate(delivery, release, api.repository)
    tag, expected = delivery['tag'], delivery['assets']
    for name, item in expected.items():
        path = directory / name
        if (path.is_symlink() or not path.is_file() or path.stat().st_size != item['bytes']
                or sha256(path) != item['sha256']):
            raise ValueError('Local source asset differs from collected bytes')
    if not tag_exists(api, tag):
        api.json('git/refs', method='POST', data={'ref': 'refs/tags/' + tag, 'sha': release['gitCommit']})
    if remote_tag_commit(api, tag) != release['gitCommit']:
        raise ValueError('Immutable source tag identifies another commit')
    matches = [r for page in api.json('releases?per_page=100', paginate=True) for r in page
               if r['tag_name'] == tag]
    if len(matches) > 1:
        raise ValueError('Ambiguous source releases')
    if not matches:
        metadata = api.json('releases', method='POST', data={
            'tag_name': tag, 'name': 'Sources and GNOME integration for ' + release['version'],
            'draft': True, 'prerelease': True, 'make_latest': 'false',
            'body': 'Corresponding sources and optional GNOME extension for '
                    f'[{release["tag"]}](https://github.com/{api.repository}/releases/tag/{release["tag"]}). '
                    'Installers are on that release. Verify SHA256SUMS before use. '
                    'The Proton source catalog on the installer release links the unchanged dependency sources.'})
    else:
        metadata = matches[0]
    if metadata.get('prerelease') is not True:
        raise ValueError('Source release must remain separate from stable installers')
    public = metadata.get('draft') is False
    verify_metadata(api, metadata, expected, complete=public)
    if not public:
        missing = set(expected) - {a['name'] for a in metadata['assets']}
        if missing:
            subprocess.run(['gh', 'release', 'upload', tag, '--repo', api.repository,
                            *[str(directory / name) for name in sorted(missing)]], check=True, timeout=1800)
        metadata = api.json('releases/' + str(metadata['id']))
        if metadata.get('draft') is not True or metadata.get('prerelease') is not True:
            raise ValueError('Source release changed before publication')
        verify_metadata(api, metadata, expected)
        if remote_tag_commit(api, tag) != release['gitCommit']:
            raise ValueError('Source tag changed before publication')
        api.json('releases/' + str(metadata['id']), method='PATCH',
                 data={'draft': False, 'prerelease': True, 'make_latest': 'false'})
    return verify_public(api, delivery, release)


def verify_public(api, delivery, release):
    validate(delivery, release, api.repository)
    metadata = api.json('releases/tags/' + quote(delivery['tag'], safe=''))
    if metadata.get('draft') is not False or metadata.get('prerelease') is not True:
        raise ValueError('Corresponding sources are not published')
    if remote_tag_commit(api, delivery['tag']) != release['gitCommit']:
        raise ValueError('Source tag differs from installer commit')
    verify_metadata(api, metadata, delivery['assets'])
    if remote_tag_commit(api, delivery['tag']) != release['gitCommit']:
        raise ValueError('Source tag changed during verification')
    return {'releaseId': metadata['id'], 'tag': delivery['tag'], 'gitCommit': release['gitCommit']}
