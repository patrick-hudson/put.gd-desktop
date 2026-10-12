#!/usr/bin/env python3
"""Stage a verified GitHub prerelease; publish only after package acceptance."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from collect import sha256
from verify_tag import verify



def github_json(*arguments):
    return json.loads(subprocess.check_output(['gh', 'api', *arguments], text=True))


def remote_tag_commit(tag):
    value = github_json('repos/{owner}/{repo}/git/ref/tags/' + tag)['object']
    # Annotated tags may reference other tags. Bound traversal and require a commit.
    for _ in range(8):
        if value['type'] == 'commit':
            return value['sha']
        if value['type'] != 'tag':
            break
        value = github_json('repos/{owner}/{repo}/git/tags/' + value['sha'])['object']
    raise ValueError('Remote release tag does not resolve to a bounded commit')



def matching_releases(tag):
    # Authenticated List releases includes drafts; release-by-tag does not.
    pages = github_json('--paginate', '--slurp', 'repos/{owner}/{repo}/releases?per_page=100')
    return [release for page in pages for release in page if release['tag_name'] == tag]


def asset_snapshot(metadata, expected, directory):
    assets = metadata['assets']
    names = {asset['name'] for asset in assets}
    if len(names) != len(assets) or not names.issubset(expected):
        raise ValueError('Remote draft asset inventory differs from accepted assets')
    values = {}
    for asset in assets:
        name = asset['name']
        if asset['state'] != 'uploaded' or asset['size'] != (directory / name).stat().st_size:
            raise ValueError('Remote draft asset is incomplete or has a different size')
        values[name] = {key: asset[key] for key in ('id', 'size', 'updated_at')}
    return values


def verify_uploaded_assets(assets, expected):
    with tempfile.TemporaryDirectory(prefix='pg-release-verify-') as temporary:
        for name, asset in assets.items():
            asset_id = asset['id']
            if type(asset_id) is not int or asset_id <= 0:
                raise ValueError('Remote asset ID is invalid')
            path = Path(temporary) / name
            with path.open('wb') as stream:
                subprocess.run(['gh', 'api', '--method', 'GET', '-H', 'Accept: application/octet-stream',
                                'repos/{owner}/{repo}/releases/assets/' + str(asset_id)],
                               check=True, stdout=stream)
            if not path.is_file() or path.is_symlink() or sha256(path) != expected[name]:
                raise ValueError('Remote draft bytes differ from accepted assets: ' + name)


def verify_remote_draft(tag, directory, listed, commit):
    """Verify the uploaded draft, including checksums, before the public mutation."""
    drafts = matching_releases(tag)
    if len(drafts) != 1 or drafts[0]['draft'] is not True:
        raise ValueError('Publication requires exactly one matching existing draft')
    before = drafts[0]
    endpoint = 'repos/{owner}/{repo}/releases/' + str(before['id'])
    if remote_tag_commit(tag) != commit:
        raise ValueError('Remote tag differs from the accepted source commit')
    expected = {**listed, 'SHA256SUMS': sha256(directory / 'SHA256SUMS')}
    assets = asset_snapshot(before, expected, directory)
    if set(assets) != set(expected):
        raise ValueError('Remote draft asset inventory differs from accepted assets')
    verify_uploaded_assets(assets, expected)
    after = github_json(endpoint)
    if (after['id'] != before['id'] or after['tag_name'] != tag or after['draft'] is not True
            or asset_snapshot(after, expected, directory) != assets or remote_tag_commit(tag) != commit):
        raise ValueError('Remote draft or tag changed during verification')
    return {'releaseId': before['id'], 'assets': assets, 'sha256': expected}


def stage_draft(tag, directory, listed, release, notes):
    """Resume only a matching draft; never replace unverified existing assets."""
    if remote_tag_commit(tag) != release['gitCommit']:
        raise ValueError('Remote tag differs from the release source commit')
    matches = matching_releases(tag)
    if len(matches) > 1 or (matches and matches[0]['draft'] is not True):
        raise ValueError('Staging requires a unique draft; published releases are immutable here')
    if not matches:
        command = ['gh', 'release', 'create', tag, '--verify-tag', '--draft',
                   '--title', 'put.gd ' + release['version'], '--notes-file', str(notes)]
        if release['prerelease']:
            command.append('--prerelease')
        subprocess.run(command, check=True)
        matches = matching_releases(tag)
        if len(matches) != 1 or matches[0]['draft'] is not True:
            raise ValueError('Created draft could not be uniquely verified')
    expected = {**listed, 'SHA256SUMS': sha256(directory / 'SHA256SUMS')}
    assets = asset_snapshot(matches[0], expected, directory)
    verify_uploaded_assets(assets, expected)
    missing = sorted(set(expected) - set(assets))
    if missing:
        subprocess.run(['gh', 'release', 'upload', tag, *[str(directory / name) for name in missing]], check=True)
    # An interrupted upload can be retried: completed assets must match exactly;
    # missing files are added. Incomplete/conflicting assets require inspection.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--directory', required=True, type=Path)
    parser.add_argument('--publish', action='store_true', help='Publish the existing draft after acceptance')
    parser.add_argument('--acceptance', type=Path, help='Current package acceptance receipt')
    args = parser.parse_args()
    release = verify(Path.cwd(), args.tag)
    bundle = json.loads((args.directory / 'release-builds.json').read_text())
    if bundle['release'] != release:
        raise ValueError('Collected release source identity changed')
    listed = {}
    for line in (args.directory / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if Path(name).name != name or sha256(args.directory / name) != digest:
            raise ValueError('Collected release checksum failed')
        if name in listed:
            raise ValueError('Duplicate collected release checksum')
        listed[name] = digest
    if args.publish:
        if not args.acceptance:
            parser.error('--acceptance is required to publish a tested draft')
        acceptance = json.loads(args.acceptance.read_text())
        if acceptance.get('tag') != args.tag or acceptance.get('gitCommit') != release['gitCommit'] or acceptance.get('status') != 'passed':
            raise ValueError('Acceptance must match this exact release source')
        if acceptance.get('assetSha256') != listed:
            raise ValueError('Acceptance must match every exact collected asset')
        verified = verify_remote_draft(args.tag, args.directory, listed, release['gitCommit'])
        subprocess.run(['gh', 'release', 'edit', args.tag, '--draft=false'], check=True)
        (args.directory / 'publication.json').write_text(json.dumps({
            'schemaVersion': 1, 'tag': args.tag, 'gitCommit': release['gitCommit'],
            'verifiedRemoteDraft': verified, 'published': True}, indent=2) + '\n')
        return
    # A source tag alone does not replace current cross-platform compilation CI.
    runs = json.loads(subprocess.check_output(['gh', 'run', 'list', '--workflow', 'Build', '--commit', release['gitCommit'], '--limit', '100', '--json', 'headSha,status,conclusion'], text=True))
    if not any(run['headSha'] == release['gitCommit'] and run['status'] == 'completed' and run['conclusion'] == 'success' for run in runs):
        raise ValueError('No passing Build workflow for this exact release commit')
    notes = args.directory / 'release-notes.md'
    notes.write_text('Linux x86_64 prerelease with individual Ubuntu DEBs, a Fedora RPM, an AppImage, a buildable source tarball and a combined ZIP.\n\n'
                     'DEB/RPM use distro Qt. AppImage dynamically bundles Qt 6.8.3 and requires glibc 2.39 or newer. '
                     'GNOME 50 requires the included host extension. Flatpak is tracked in issue #14.\n\n'
                     'Compiled source identities, dependency inventories, licenses and SHA256 checksums accompany the builds. '
                     'Full release certification remains separate from this prerelease; package-specific acceptance is recorded before publication.\n')
    stage_draft(args.tag, args.directory, listed, release, notes)


if __name__ == '__main__':
    main()
