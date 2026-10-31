#!/usr/bin/env python3
"""Keep five calendar nightlies and all repository/site/source dependencies.

Only GitHub release IDs are deleted; git refs are never mutated. Any unreadable
protection input aborts the entire plan before a deletion can be attempted.
Requires gh and zstd (for createrepo_c's primary.xml.zst).
"""
import argparse
import bz2
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import xml.etree.ElementTree as ET

from github_api import GitHub
from release_identity import parse_tag, parse_version
from support_assets import snapshot

REPOSITORY = 'patrick-hudson/put.gd-desktop'
SITE = 'https://app.put.gd/_site/releases.json'
MIRROR = 'https://pkg.put.gd/'
LIMIT = 16 * 1024 * 1024
REPO_NS = '{http://linux.duke.edu/metadata/repo}'
PRIMARY_NS = '{http://linux.duke.edu/metadata/common}'
MANIFESTS = {'source-delivery.json', 'proton-source-catalog.json'}


ALLOWED_HOSTS = {'app.put.gd', 'pkg.put.gd'}


def _allowed(url):
    parts = urlsplit(url)
    return parts.scheme == 'https' and parts.hostname in ALLOWED_HOSTS


class _AllowlistRedirect(HTTPRedirectHandler):
    # Protection metadata may only come from our own hosts over HTTPS, even after a redirect.
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not _allowed(newurl):
            raise ValueError('Protection metadata redirected off the allowed hosts: ' + newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def fetch(url):
    # Public metadata only: never forward the GitHub token to the site/CDN.
    if not _allowed(url):
        raise ValueError('Protection metadata must come from an allowed HTTPS host: ' + url)
    opener = build_opener(_AllowlistRedirect)
    with opener.open(Request(url, headers={'User-Agent': 'putgd-nightly-retention',
                                           'Cache-Control': 'no-cache'}), timeout=60) as response:
        data = response.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError('Protection metadata exceeds size limit: ' + url)
    return data


def package_tag(version):
    identity = parse_version(version.replace('~', '-'))
    return 'v' + identity.version


def apt_tags(data):
    tags = set()
    for block in data.decode('utf-8').strip().split('\n\n'):
        if not block.strip():
            continue  # An empty stable suite is valid before its first release.
        fields = {}
        for line in block.splitlines():
            if line.startswith((' ', '\t')):
                continue
            key, sep, value = line.partition(':')
            if not sep or key in fields:
                raise ValueError('Malformed Packages stanza')
            fields[key] = value.strip()
        if 'Package' not in fields:
            raise ValueError('Missing Package field')
        if fields['Package'] == 'putgd':
            tags.add(package_tag(fields['Version']))
    return tags


def primary_tags(data):
    root = ET.fromstring(data)
    if root.tag != PRIMARY_NS + 'metadata':
        raise ValueError('Invalid RPM primary metadata')
    packages = root.findall(PRIMARY_NS + 'package')
    if int(root.attrib['packages']) != len(packages):
        raise ValueError('Incomplete RPM primary metadata')
    tags = set()
    for package in packages:
        name = package.findtext(PRIMARY_NS + 'name')
        if not name:
            raise ValueError('Missing RPM package name')
        if name == 'putgd':
            version = package.find(PRIMARY_NS + 'version')
            tags.add(package_tag(version.attrib['ver']))
    return tags


def rpm_tags(base, get):
    root = ET.fromstring(get(base + 'repodata/repomd.xml'))
    if root.tag != REPO_NS + 'repomd':
        raise ValueError('Invalid repomd metadata')
    entries = [entry for entry in root.findall(REPO_NS + 'data') if entry.get('type') == 'primary']
    if len(entries) != 1:
        raise ValueError('No unique RPM primary index')
    entry = entries[0]
    href = entry.find(REPO_NS + 'location').attrib['href']
    if not re.fullmatch(r'repodata/[A-Za-z0-9._-]+', href):
        raise ValueError('Unsafe RPM primary location')
    data = get(base + href)
    checksum = entry.find(REPO_NS + 'checksum')
    if checksum.get('type') != 'sha256' or hashlib.sha256(data).hexdigest() != checksum.text:
        raise ValueError('RPM primary checksum mismatch')
    if len(data) != int(entry.findtext(REPO_NS + 'size')):
        raise ValueError('RPM primary size mismatch')
    if href.endswith('.zst'):
        data = subprocess.check_output(['zstd', '-q', '-d', '-c'], input=data, timeout=60)
    elif href.endswith('.gz'):
        data = gzip.decompress(data)
    elif href.endswith('.xz'):
        data = lzma.decompress(data)
    elif href.endswith('.bz2'):
        data = bz2.decompress(data)
    elif not href.endswith('.xml'):
        raise ValueError('Unsupported RPM primary compression')
    if len(data) > LIMIT or len(data) != int(entry.findtext(REPO_NS + 'open-size')):
        raise ValueError('RPM primary open size mismatch')
    return primary_tags(data)


def repository_tags(get=fetch):
    indexes = {}
    for distro in ('ubuntu24.04', 'ubuntu26.04', 'debian13'):
        for suite in ('nightly', 'stable'):
            url = MIRROR + f'apt/{distro}/dists/{suite}/main/binary-amd64/Packages'
            indexes[url] = sorted(apt_tags(get(url)))
    for suite in ('nightly', 'stable'):
        base = MIRROR + f'rpm/fedora/44/x86_64/{suite}/'
        indexes[base + 'repodata/repomd.xml'] = sorted(rpm_tags(base, get))
    return indexes


def nightly(release):
    try:
        identity = parse_tag(release['tag_name'])
    except ValueError:
        return None
    if (identity.channel == 'nightly' and not identity.legacy
            and release['draft'] is False and release['prerelease'] is True):
        return identity.key
    return None


def source_tag(tag):
    return 'sources-' + tag + '-files'


def links(api, release, by_tag):
    """Check both manifests, including older catalogs with in-release parts."""
    linked = set()

    def target(tag, filename=None):
        other = by_tag.get(tag)
        if other is None or (not release['draft'] and other['draft']):
            raise ValueError('Source link has no public release: ' + tag)
        if filename and filename not in {a['name'] for a in other['assets']}:
            raise ValueError('Source link has no asset: ' + tag + '/' + filename)
        linked.add(tag)

    def visit(value):
        if isinstance(value, dict):
            for item in value.values():
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)
        elif isinstance(value, str) and value.startswith('https://github.com/' + api.repository + '/releases/'):
            url = urlsplit(value)
            tail = url.path.split('/releases/', 1)[1].split('/')
            if url.query or url.fragment or len(tail) not in (2, 3):
                raise ValueError('Malformed source release URL')
            if tail[0] == 'tag' and len(tail) == 2:
                target(unquote(tail[1]))
            elif tail[0] == 'download' and len(tail) == 3:
                target(unquote(tail[1]), unquote(tail[2]))
            else:
                raise ValueError('Malformed source release URL')

    names = [asset['name'] for asset in release['assets']]
    if len(names) != len(set(names)):
        raise ValueError('Duplicate release assets')
    if source_tag(release['tag_name']) in by_tag and 'source-delivery.json' not in names:
        raise ValueError('Installer with separate sources has no source-delivery.json')
    for asset in release['assets']:
        if asset['name'] not in MANIFESTS:
            continue
        if not 0 < asset['size'] <= LIMIT or asset['state'] != 'uploaded':
            raise ValueError('Incomplete source manifest')
        data = api.asset_bytes(asset['id'])
        if len(data) != asset['size']:
            raise ValueError('Source manifest size mismatch')
        if asset.get('digest') and asset['digest'] != 'sha256:' + hashlib.sha256(data).hexdigest():
            raise ValueError('Source manifest digest mismatch')
        manifest = json.loads(data)
        if manifest['schemaVersion'] != 1:
            raise ValueError('Unknown source manifest schema')
        if asset['name'] == 'source-delivery.json':
            if manifest['repository'] != api.repository or not manifest['assets']:
                raise ValueError('Invalid source delivery')
            for name in manifest['assets']:
                target(manifest['tag'], name)
            if manifest['url'] != f'https://github.com/{api.repository}/releases/tag/{manifest["tag"]}':
                raise ValueError('Source delivery URL differs from its tag')
        else:
            parts = manifest['delivery']['parts']
            if not parts:
                raise ValueError('Empty Proton source catalog')
            for part in parts:
                if 'url' not in part:
                    target(release['tag_name'], part['file'])
                else:
                    url = urlsplit(part['url'])
                    prefix = f'/{api.repository}/releases/download/'
                    tail = url.path[len(prefix):].split('/')
                    if (url.scheme != 'https' or url.netloc != 'github.com' or url.query or url.fragment
                            or not url.path.startswith(prefix) or len(tail) != 2
                            or unquote(tail[1]) != part['file']):
                        raise ValueError('Proton source URL differs from catalog file')
                    target(unquote(tail[0]), part['file'])
        visit(manifest)
    return linked


def inventory(api):
    return [release for page in api.json('releases?per_page=100', paginate=True) for release in page]


def plan(api, releases, indexes, site):
    by_tag = {release['tag_name']: release for release in releases}
    if len(by_tag) != len(releases) or len({r['id'] for r in releases}) != len(releases):
        raise ValueError('Ambiguous release inventory')
    nightlies = sorted((r for r in releases if nightly(r)), key=nightly, reverse=True)
    newest = {r['tag_name'] for r in nightlies[:5]}
    protected = {tag for tags in indexes.values() for tag in tags}
    # Match put.gd-site tools/releases.py's published-at channel selection too.
    if nightlies:
        protected.add(max(nightlies, key=lambda r: r['published_at'])['tag_name'])
    if site['repo'] != api.repository:
        raise ValueError('Download site identifies another repository')
    for channel in ('nightly', 'stable'):
        if site[channel] is not None:
            protected.add(site[channel]['tag'])
    candidates = {r['tag_name'] for r in nightlies} - newest - protected
    # Never sweep orphan, stable, draft, legacy or unknown source releases.
    candidates |= {source_tag(tag) for tag in candidates
                   if source_tag(tag) in by_tag and by_tag[source_tag(tag)]['draft'] is False
                   and by_tag[source_tag(tag)]['prerelease'] is True}
    keep = set(by_tag) - candidates
    pending = list(keep)
    while pending:
        tag = pending.pop()
        dependencies = links(api, by_tag[tag], by_tag)
        if source_tag(tag) in by_tag:
            dependencies.add(source_tag(tag))
        for dependency in dependencies - keep:
            keep.add(dependency)
            pending.append(dependency)
    deleted = set(by_tag) - keep
    # A linked installer can have become protected while walking manifests.
    if any(tag.startswith('sources-') and tag[len('sources-'):-len('-files')] not in deleted for tag in deleted):
        raise ValueError('Cannot delete sources without deleting their installer')
    # Installers first; if a deletion fails, their sources remain intact.
    ordered = sorted(deleted, key=lambda tag: (tag.startswith('sources-'), tag))
    return {'keepNewest': sorted(newest), 'protected': sorted(protected),
            'packageIndexes': indexes, 'keep': sorted(keep),
            'delete': [{'tag': tag, 'releaseId': by_tag[tag]['id']} for tag in ordered]}


def run(api, *, dry_run, get=fetch, output=None):
    releases = inventory(api)
    result = plan(api, releases, repository_tags(get), json.loads(get(SITE)))
    result['dryRun'] = dry_run
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2), flush=True)
    if not dry_run and result['delete']:
        # Re-read every protection input and release immediately before mutation.
        current = inventory(api)
        if ({r['tag_name']: snapshot(r) for r in current}
                != {r['tag_name']: snapshot(r) for r in releases}
                or plan(api, current, repository_tags(get), json.loads(get(SITE))) != {
                    key: value for key, value in result.items() if key != 'dryRun'}):
            raise ValueError('Retention inputs changed; rerun to calculate a fresh plan')
        for item in result['delete']:
            api.json('releases/' + str(item['releaseId']), method='DELETE')
            print('Deleted release only: ' + item['tag'], flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY', REPOSITORY))
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--output', type=Path, help='Retain the calculated plan as JSON')
    args = parser.parse_args()
    run(GitHub(args.repository), dry_run=args.dry_run, output=args.output)


if __name__ == '__main__':
    main()
