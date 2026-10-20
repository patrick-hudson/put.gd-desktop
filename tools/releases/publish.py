#!/usr/bin/env python3
"""Stage verified release drafts; publish accepted packages or automated nightlies.

Drafts and nightlies are staged by the workflow run that built and tested the
exact tag, after every required job in that run passed. The run identity is
recorded in the publication receipt and release notes.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from collect import sha256
from verify_tag import verify
from github_api import GitHub
from nightly import published_nightly, public_inventory
from release_identity import parse_tag, parse_version



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


def verify_remote_draft(tag, directory, listed, commit, *, nightly=False):
    """Verify the uploaded draft, including checksums, before the public mutation."""
    drafts = matching_releases(tag)
    if len(drafts) != 1 or drafts[0]['draft'] is not True:
        raise ValueError('Publication requires exactly one matching existing draft')
    before = drafts[0]
    if nightly and before.get('prerelease') is not True:
        raise ValueError('Nightly draft must be a prerelease')
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
            or asset_snapshot(after, expected, directory) != assets or remote_tag_commit(tag) != commit
            or (nightly and after.get('prerelease') is not True)):
        raise ValueError('Remote draft or tag changed during verification')
    return {'releaseId': before['id'], 'assets': assets, 'sha256': expected}


def release_title(release):
    """Name the GitHub release; nightlies say so in words rather than in a suffix."""
    identity = parse_version(release['version'])
    if identity.channel == 'nightly':
        return f'put.gd {identity.base_version} nightly {identity.count}'
    return 'put.gd ' + release['version']


def stage_draft(tag, directory, listed, release, notes):
    """Resume only a matching draft; never replace unverified existing assets."""
    if remote_tag_commit(tag) != release['gitCommit']:
        raise ValueError('Remote tag differs from the release source commit')
    matches = matching_releases(tag)
    if len(matches) > 1 or (matches and matches[0]['draft'] is not True):
        raise ValueError('Staging requires a unique draft; published releases are immutable here')
    if not matches:
        command = ['gh', 'release', 'create', tag, '--verify-tag', '--draft',
                   '--title', release_title(release), '--notes-file', str(notes)]
        if release['prerelease']:
            command.append('--prerelease')
        if release.get('channel') == 'nightly':
            command.append('--latest=false')
        subprocess.run(command, check=True)
        # GitHub may briefly omit a newly created draft from List releases.
        # Retry only missing results; never repeat creation or hide conflicts/errors.
        for attempt in range(5):
            if attempt:
                delay = 2 ** (attempt - 1)
                print(f'Waiting {delay}s for created draft visibility ({attempt + 1}/5)',
                      file=sys.stderr, flush=True)
                time.sleep(delay)
            matches = matching_releases(tag)
            if matches:
                break
        if not matches:
            raise ValueError('Draft creation succeeded, but the draft is still not visible after five reads; '
                             'inspect existing drafts before rerunning')
        if len(matches) != 1 or matches[0]['draft'] is not True:
            raise ValueError('Created draft could not be uniquely verified')
    if release.get('channel') == 'nightly' and matches[0].get('prerelease') is not True:
        raise ValueError('Nightly draft must be a prerelease')
    expected = {**listed, 'SHA256SUMS': sha256(directory / 'SHA256SUMS')}
    assets = asset_snapshot(matches[0], expected, directory)
    verify_uploaded_assets(assets, expected)
    if release.get('channel') == 'nightly':
        after = matching_releases(tag)
        if (len(after) != 1 or after[0].get('id') != matches[0]['id']
                or after[0].get('draft') is not True or after[0].get('prerelease') is not True
                or asset_snapshot(after[0], expected, directory) != assets
                or remote_tag_commit(tag) != release['gitCommit']):
            raise ValueError('Nightly draft or tag changed during staging verification')
    missing = sorted(set(expected) - set(assets))
    if missing:
        subprocess.run(['gh', 'release', 'upload', tag, *[str(directory / name) for name in missing]], check=True)
    # An interrupted upload can be retried: completed assets must match exactly;
    # missing files are added. Incomplete/conflicting assets require inspection.


def run_provenance():
    """Identify the workflow run whose required jobs built, tested and collected these assets."""
    try:
        run_id, attempt = int(os.environ['GITHUB_RUN_ID']), int(os.environ['GITHUB_RUN_ATTEMPT'])
        repository, workflow = os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_WORKFLOW_REF']
    except (KeyError, ValueError) as error:
        raise ValueError('Automated publication requires the GitHub Actions run identity') from error
    if run_id <= 0 or attempt <= 0 or not workflow.startswith(repository + '/.github/workflows/'):
        raise ValueError('Automated publication run identity is invalid')
    server = os.environ.get('GITHUB_SERVER_URL', 'https://github.com')
    return {'repository': repository, 'workflowRef': workflow, 'runId': run_id, 'runAttempt': attempt,
            'url': f'{server}/{repository}/actions/runs/{run_id}'}


def publish_nightly(args, release, listed):
    """Publish one automated nightly, retaining exact run and byte evidence."""
    if set(listed) | {'SHA256SUMS'} != public_inventory(args.tag):
        raise ValueError('Collected nightly inventory must contain all fifteen public assets')
    producer = run_provenance()
    api = GitHub(producer['repository'])
    expected = {**listed, 'SHA256SUMS': sha256(args.directory / 'SHA256SUMS')}
    public = published_nightly(api, args.tag, release)
    if public and public['assetSha256'] != expected:
        raise ValueError('Public nightly bytes conflict with original collected inventory')
    receipt = {'schemaVersion': 1, 'tag': args.tag, 'channel': 'nightly',
               'gitCommit': release['gitCommit'], 'source': release,
               'verificationClass': 'automated-build-and-package', 'producer': producer}
    if public:
        receipt.update(published=True, unchanged=True, verifiedPublishedNightly=public)
    else:
        notes = args.directory / 'release-notes.md'
        identity = parse_tag(args.tag)
        notes.write_text(f'Nightly {identity.count} of {identity.base_version}: a build-tested development '
            f'release of commit {release["gitCommit"]}.\n\n'
            'Automated coverage: Linux test suites, Windows/macOS portable compilation, '
            'Ubuntu 24.04/26.04 DEB and Fedora 44 RPM audits, AppImage packaging, '
            'and production compilation of the exact curated source tarball. '
            'This does not certify current graphical compositor behavior, memory acceptance or live S3 behavior.\n\n'
            'Individual DEBs, RPM, AppImage, source tarball, GNOME ZIP, release-builds.json, '
            'combined ZIP and SHA256SUMS are available separately. DEB/RPM use distro Qt. '
            'AppImage bundles dynamically linked Qt 6.8.3 and requires glibc 2.39 or newer. '
            'Its screen recorder bundles x264, x265 and a GPL FFmpeg, so the AppImage is a GPL-2.0-or-later '
            'combined work; their corresponding source is the appimage-media-source tarball. '
            'GNOME 50 requires the included host extension.\n\n'
            'Proton corresponding source is delivered as two separate transport parts plus '
            'proton-source-catalog.json. Verify SHA256SUMS, concatenate part1 then part2, '
            'and unpack the resulting tar.gz. The combined ZIP includes the catalog; '
            'download the two source parts separately.\n\n'
            'The debug tar.zst holds every build\'s split debug files by GNU build ID, for symbolizing '
            'crash records (docs/diagnostics.md); it is not part of the combined ZIP.\n\n'
            + 'Built, tested and packaged by ' + producer['url'] + '\n\n'
            + 'Channel/source and automated producer evidence:\n\n```json\n'
            + json.dumps(receipt, indent=2) + '\n```\n')
        stage_draft(args.tag, args.directory, listed, release, notes)
        verified = verify_remote_draft(args.tag, args.directory, listed, release['gitCommit'], nightly=True)
        final = matching_releases(args.tag)
        if (len(final) != 1 or final[0].get('id') != verified['releaseId']
                or final[0].get('draft') is not True or final[0].get('prerelease') is not True
                or asset_snapshot(final[0], expected, args.directory) != verified['assets']
                or remote_tag_commit(args.tag) != release['gitCommit']):
            raise ValueError('Nightly draft or tag changed before publication')
        subprocess.run(['gh', 'release', 'edit', args.tag, '--draft=false',
                        '--prerelease', '--latest=false'], check=True)
        public = published_nightly(api, args.tag, release)
        if (not public or public['releaseId'] != verified['releaseId']
                or public['assetSha256'] != expected):
            raise ValueError('Published nightly metadata or bytes could not be verified')
        receipt.update(verifiedRemoteDraft=verified, verifiedPublishedNightly=public, published=True)
    (args.directory / 'publication.json').write_text(json.dumps(receipt, indent=2) + '\n')


SITE_REPOSITORY = 'patrick-hudson/put.gd-site'


def notify_site(tag):
    """Tell the download site a release is public; its six-hour schedule is the backstop.

    Manual publication runs with the operator's own gh login, which can reach the
    site repository. Nightlies are published with the workflow token, which cannot,
    so nightly.yml sends this event itself after publish.py --nightly returns.
    """
    prerelease = 'true' if parse_tag(tag).prerelease else 'false'
    command = ['gh', 'api', f'repos/{SITE_REPOSITORY}/dispatches', '-f', 'event_type=release-published',
               '-f', f'client_payload[tag]={tag}', '-F', f'client_payload[prerelease]={prerelease}']
    try:
        subprocess.run(command, check=True)
    except (subprocess.CalledProcessError, OSError) as error:
        print(f'Warning: could not notify {SITE_REPOSITORY} of {tag}: {error}. '
              'Its deploy refreshes within six hours, or run its deploy workflow by hand.',
              file=sys.stderr, flush=True)
        return False
    print(f'Notified {SITE_REPOSITORY} of {tag}; its deploy refreshes the download links.', flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--directory', required=True, type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--publish', action='store_true', help='Publish the existing draft after acceptance')
    mode.add_argument('--nightly', action='store_true', help='Publish a strict nightly after automated verification')
    parser.add_argument('--acceptance', type=Path, help='Current package acceptance receipt')
    args = parser.parse_args()
    release = verify(Path.cwd(), args.tag)
    if args.nightly and (parse_tag(args.tag).channel != 'nightly' or release.get('channel') != 'nightly'):
        raise ValueError('Automated nightly publication requires a verified strict nightly tag')
    if args.nightly and args.acceptance:
        parser.error('--nightly uses automated evidence, not package acceptance')
    if args.publish and release.get('channel') == 'nightly':
        parser.error('Nightly publication requires --nightly from the run that built it')
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
    if set(listed) | {'SHA256SUMS'} != public_inventory(args.tag):
        raise ValueError('Collected release inventory must include all fifteen public assets, including Proton and AppImage media sources and debug symbols')
    if args.nightly:
        publish_nightly(args, release, listed)
        return
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
        receipt = {'schemaVersion': 1, 'tag': args.tag, 'gitCommit': release['gitCommit'],
                   'verifiedRemoteDraft': verified, 'published': True}
        publication = args.directory / 'publication.json'
        # The receipt records the publication before the site is told about it:
        # a failed notification never turns a published release into a failed run.
        publication.write_text(json.dumps(receipt, indent=2) + '\n')
        receipt['siteNotified'] = notify_site(args.tag)
        publication.write_text(json.dumps(receipt, indent=2) + '\n')
        return
    # Staging runs only after CI, packaging and collection passed in this same run.
    producer = run_provenance()
    kind = 'release candidate' if release['channel'] == 'candidate' else 'release'
    notes = args.directory / 'release-notes.md'
    notes.write_text('Linux x86_64 ' + kind + ' with individual Ubuntu DEBs, a Fedora RPM, an AppImage, a buildable source tarball and a combined ZIP.\n\n'
                     'DEB/RPM use distro Qt. AppImage dynamically bundles Qt 6.8.3 and requires glibc 2.39 or newer. '
                     'Its screen recorder bundles x264, x265 and a GPL FFmpeg, so the AppImage is a GPL-2.0-or-later combined work; '
                     'their corresponding source is the appimage-media-source tarball. '
                     'GNOME 50 requires the included host extension. Flatpak needs no host helper, except that Plasma Wayland loses direct screenshots; it is tracked in issue #83 (docs/flatpak.md).\n\n'
                     'Proton corresponding source accompanies the packages in two transport parts and proton-source-catalog.json. '
                     'Verify SHA256SUMS, concatenate part1 then part2, and unpack the resulting tar.gz. '
                     'The combined ZIP includes the catalog; download the two source parts separately.\n\n'
                     'The debug tar.zst holds every build\'s split debug files by GNU build ID, for symbolizing crash records '
                     '(docs/diagnostics.md); it is not part of the combined ZIP.\n\n'
                     'Compiled source identities, dependency inventories, licenses and SHA256 checksums accompany the builds. '
                     'Package-specific acceptance is recorded before this draft is published.\n\n'
                     'Built, tested and packaged by ' + producer['url'] + '\n')
    stage_draft(args.tag, args.directory, listed, release, notes)
    (args.directory / 'staging.json').write_text(json.dumps({
        'schemaVersion': 1, 'tag': args.tag, 'channel': release['channel'],
        'gitCommit': release['gitCommit'], 'producer': producer, 'draft': True}, indent=2) + '\n')


if __name__ == '__main__':
    main()
