"""Update feed contract v1 (#293): build one channel's feed from a GitHub release and check it.

The package signer writes the feeds only after the apt and dnf repositories carry
the release, so a DEB/RPM user is never told about a version apt or dnf cannot
fetch yet. This contract moved here from put.gd-site's tools/releases.py.
"""
import re

CHANNELS = ('stable', 'nightly')
FEED_SCHEMA = 1
# Feed asset key -> the release asset name it matches.
ASSET_PATTERNS = {
    'ubuntu24.04': re.compile(r'^putgd-[^/]+-ubuntu24\.04-x86_64\.deb$'),
    'ubuntu26.04': re.compile(r'^putgd-[^/]+-ubuntu26\.04-x86_64\.deb$'),
    'debian13': re.compile(r'^putgd-[^/]+-debian13-x86_64\.deb$'),
    'fedora44': re.compile(r'^putgd-[^/]+-fedora44-x86_64\.rpm$'),
    'appimage': re.compile(r'^putgd-[^/]+-x86_64\.AppImage$'),
}
ZSYNC_PERMALINK = 'https://app.put.gd/dl/{channel}/appimage.zsync'
SHA256_LINE = re.compile(r'^([0-9a-fA-F]{64}) [ *](.+)$')
GENERATED_AT = re.compile(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ')


def parse_sums(text):
    """Map file name -> lowercase sha256 from SHA256SUMS text (sha256sum text or binary mode)."""
    sums = {}
    for line in text.splitlines():
        match = SHA256_LINE.match(line.strip())
        if match:
            if match.group(2) in sums:
                raise ValueError('SHA256SUMS lists a file twice: ' + match.group(2))
            sums[match.group(2)] = match.group(1).lower()
    return sums


def feed(channel, release, sums, generated_at):
    """The feed document for one channel; release is a GitHub release object or None."""
    document = {'schema': FEED_SCHEMA, 'channel': channel, 'generatedAt': generated_at, 'release': None}
    if release is None:
        return document
    names = {asset['name']: asset for asset in release.get('assets', [])}
    assets = {}
    for key, pattern in ASSET_PATTERNS.items():
        name = next((name for name in sorted(names) if pattern.match(name)), None)
        if not name:
            continue
        entry = {'name': name, 'url': names[name]['browser_download_url'], 'size': names[name]['size']}
        if name in sums:
            entry['sha256'] = sums[name]
        if key == 'appimage' and name + '.zsync' in names:
            entry['zsync'] = ZSYNC_PERMALINK.format(channel=channel)
        assets[key] = entry
    document['release'] = {
        'tag': release['tag_name'],
        'version': release['tag_name'].removeprefix('v'),
        'publishedAt': release['published_at'],
        'notesUrl': release['html_url'],
        'assets': assets,
    }
    return document


def check_feed(document):
    """Raise ValueError unless the document follows feed contract v1."""
    def need(ok, what):
        if not ok:
            raise ValueError(f"{document.get('channel')} feed: {what}")

    need(set(document) == {'schema', 'channel', 'generatedAt', 'release'}, f'top-level keys {sorted(document)}')
    need(document['schema'] == FEED_SCHEMA, 'schema is not 1')
    need(document['channel'] in CHANNELS, 'unknown channel')
    need(isinstance(document['generatedAt'], str) and GENERATED_AT.fullmatch(document['generatedAt']),
         'generatedAt is not UTC ISO 8601')
    release = document['release']
    if release is None:
        return
    need(set(release) == {'tag', 'version', 'publishedAt', 'notesUrl', 'assets'}, f'release keys {sorted(release)}')
    need(release['version'] == release['tag'].removeprefix('v'), 'version is not the tag without its v')
    need(release['notesUrl'].startswith('https://'), 'notesUrl is not https')
    need(set(release['assets']) <= set(ASSET_PATTERNS), f"asset keys {sorted(release['assets'])}")
    for key, asset in release['assets'].items():
        allowed = {'name', 'url', 'size', 'sha256'} | ({'zsync'} if key == 'appimage' else set())
        need({'name', 'url', 'size'} <= set(asset) <= allowed, f'{key} keys {sorted(asset)}')
        need(isinstance(asset['size'], int) and asset['size'] > 0, f'{key} size')
        need(asset['url'].startswith('https://'), f'{key} url is not https')
        need('sha256' not in asset or re.fullmatch(r'[0-9a-f]{64}', asset['sha256']), f'{key} sha256')
        need('zsync' not in asset or asset['zsync'] == ZSYNC_PERMALINK.format(channel=document['channel']),
             f'{key} zsync')
