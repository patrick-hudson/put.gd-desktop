#!/usr/bin/env python3
"""Signed apt and dnf repositories at pkg.put.gd, fed from published GitHub releases (#293 phase 2).

A systemd user timer on one lab builder runs `pkgrepo.py run`. Each run:

1. lists the published releases (anonymous GitHub API) and takes the newest
   nightly and stable newer than what the repositories carry;
2. verifies the release: the tag's commit, release-builds.json, SHA256SUMS, each
   package's bytes against SHA256SUMS, its receipt and GitHub's digest, and the
   package's own name, version and architecture;
3. builds the next repository state in a copy of the channel's workspace:
   reprepro for apt/<label>/dists/<channel>, createrepo_c for
   rpm/fedora/44/x86_64/<channel>;
4. signs it: nightly here with the builder's nightly key (InRelease,
   Release.gpg, repomd.xml.asc, SHA256SUMS.minisig, the feed's .minisig);
   stable is staged unsigned until sign_stable.py on the workstation returns
   the stable key's signatures, which are checked here before use;
5. uploads packages, then indexes, then the signed metadata, invalidates what
   changed, reads InRelease and repomd.xml back through the CDN, and only then
   writes the update feed, so the feed never names a version apt or dnf cannot
   fetch;
6. leaves a receipt, locally and at receipts/<tag>.json.

Objects that leave the repositories are deleted an hour later, after every
cached InRelease or repomd.xml that names them has expired.

Lab mode (`publish-local --scope lab`) publishes locally built, tagged packages
under pkg.put.gd/lab/ with the same key and the same path, for acceptance before
the packages that carry the repository are merged.

No key material or credential is ever an argument, a log line or receipt
content: gpg and minisign read their keys from GNUPGHOME and the key file, and
boto3 reads the AWS profile from the files named by AWS_CONFIG_FILE and
AWS_SHARED_CREDENTIALS_FILE.
"""
import argparse
from dataclasses import dataclass, field
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_identity import parse_tag, parse_version  # noqa: E402
import update_feed  # noqa: E402

REPOSITORY = 'patrick-hudson/put.gd-desktop'
DEB_LABELS = ('ubuntu24.04', 'ubuntu26.04', 'debian13')
RPM_LABELS = {'fedora44': 'rpm/fedora/44/x86_64'}
LABELS = DEB_LABELS + tuple(RPM_LABELS)
CHANNELS = ('nightly', 'stable')
SCOPES = {'live': '', 'lab': 'lab/'}
PACKAGE_ASSET = re.compile(r'^putgd-(?P<version>[0-9][0-9A-Za-z.-]*)-(?P<label>ubuntu24\.04|ubuntu26\.04|debian13|fedora44)'
                           r'-x86_64\.(?P<ext>deb|rpm)$')
SAFE_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._+~-]{0,199}$')
HEX64 = re.compile(r'^[0-9a-f]{64}$')
IMMUTABLE = 'public, max-age=31536000, immutable'
SHORT = 'public, max-age=300'
TOP_METADATA = {'InRelease', 'Release', 'Release.gpg', 'repomd.xml', 'repomd.xml.asc'}
MAX_JSON = 64 * 1024 * 1024
MAX_PACKAGE = 512 * 1024 * 1024
CONTENT_TYPES = {
    '.deb': 'application/vnd.debian.binary-package', '.rpm': 'application/x-rpm', '.gz': 'application/gzip',
    '.xz': 'application/x-xz', '.zst': 'application/zstd', '.bz2': 'application/x-bzip2', '.xml': 'application/xml',
    '.asc': 'application/pgp-signature', '.gpg': 'application/pgp-signature', '.json': 'application/json',
    '.minisig': 'text/plain; charset=utf-8', '.txt': 'text/plain; charset=utf-8', '.sqlite': 'application/vnd.sqlite3',
}


class SignerError(Exception):
    pass


def now_utc():
    return dt.datetime.now(dt.timezone.utc)


def iso(moment):
    return moment.strftime('%Y-%m-%dT%H:%M:%SZ')


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def version_key(version):
    """Order versions as packages do: base, then nightly < candidate < stable, then the number."""
    identity = parse_version(version)
    base = tuple(int(part) for part in identity.base_version.split('.'))
    if identity.channel == 'nightly':
        return base + (0, identity.count)
    if identity.channel == 'candidate':
        return base + (1, int(version.rsplit('.', 1)[1]))
    return base + (2, 0)


def repo_channel(tag):
    """The repository a release feeds: nightlies and stable releases; candidates stay drafts."""
    identity = parse_tag(tag)
    return identity.channel if identity.channel in CHANNELS else None


@dataclass
class Config:
    state: Path
    gnupghome: Path
    minisign_key: Path
    keys: Path
    nightly_fingerprint: str
    stable_fingerprint: str
    bucket: str = 'putgd-pkg'
    distribution: str = 'EY4KKRLYNFZ1W'
    feed_distributions: list = field(default_factory=list)
    base_url: str = 'https://pkg.put.gd'
    repository: str = REPOSITORY
    aws_profile: str = 'publish'
    rpm_sign: bool = False
    dnf_keep: int = 3
    delete_after: int = 3600
    readback_timeout: int = 600
    invalidation_timeout: int = 1200

    @classmethod
    def load(cls, path, **overrides):
        values = json.loads(Path(path).read_text()) if path and Path(path).exists() else {}
        values.update({key: value for key, value in overrides.items() if value is not None})
        for key in ('state', 'gnupghome', 'minisign_key', 'keys'):
            values[key] = Path(os.path.expanduser(str(values[key])))
        for key in ('nightly_fingerprint', 'stable_fingerprint'):
            if not re.fullmatch(r'[0-9A-F]{40}', values.get(key, '')):
                raise SignerError(key + ' must be a 40-digit uppercase fingerprint')
        return cls(**values)

    def fingerprint(self, channel):
        return self.nightly_fingerprint if channel == 'nightly' else self.stable_fingerprint


# --- GitHub --------------------------------------------------------------------------------------------------

class GitHub:
    """Anonymous, read-only access to the repository's releases; the builder holds no GitHub token."""

    def __init__(self, repository, opener=None):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
            raise SignerError('Exact repository owner/name required')
        self.repository = repository
        self.opener = opener or urllib.request.urlopen

    def _get(self, url, limit, accept='application/vnd.github+json'):
        request = urllib.request.Request(url, headers={'Accept': accept, 'User-Agent': 'put.gd-pkg-signer/1'})
        with self.opener(request, timeout=60) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise SignerError('Response larger than expected: ' + url)
        return data

    def api(self, path):
        return json.loads(self._get(f'https://api.github.com/repos/{self.repository}/{path}', 16 * 1024 * 1024))

    def releases(self):
        return [release for release in self.api('releases?per_page=40') if not release.get('draft')]

    def tag_commit(self, tag):
        value = self.api('git/ref/tags/' + tag)['object']
        for _ in range(8):
            if value.get('type') == 'commit':
                return value['sha']
            if value.get('type') != 'tag':
                break
            value = self.api('git/tags/' + value['sha'])['object']
        raise SignerError('Tag does not resolve to a commit')

    def download(self, url, destination, size):
        prefix = f'https://github.com/{self.repository}/releases/download/'
        if not url.startswith(prefix):
            raise SignerError('Asset URL is outside the repository releases: ' + url)
        request = urllib.request.Request(url, headers={'User-Agent': 'put.gd-pkg-signer/1'})
        written = 0
        partial = destination.with_name(destination.name + '.part')
        with self.opener(request, timeout=600) as response, open(partial, 'wb') as stream:
            for block in iter(lambda: response.read(1 << 20), b''):
                written += len(block)
                if written > size:
                    raise SignerError('Asset is larger than GitHub reports: ' + destination.name)
                stream.write(block)
        if written != size:
            raise SignerError('Asset size differs from GitHub: ' + destination.name)
        partial.replace(destination)


# --- Verification --------------------------------------------------------------------------------------------

def control_fields(path, label, run=subprocess.run):
    """Package name, version and architecture as the package itself states them."""
    if label in RPM_LABELS:
        result = run(['rpm', '-qp', '--nosignature', '--nodigest', '--qf', '%{NAME}\\n%{VERSION}\\n%{ARCH}\\n', str(path)],
                     capture_output=True, text=True, check=True)
    else:
        result = run(['dpkg-deb', '-f', str(path), 'Package', 'Version', 'Architecture'],
                     capture_output=True, text=True, check=True)
    lines = [line.split(':', 1)[-1].strip() for line in result.stdout.strip().splitlines()]
    if len(lines) != 3:
        raise SignerError('Cannot read package fields: ' + path.name)
    return dict(zip(('name', 'version', 'architecture'), lines))


def check_package(path, label, package_version, run=subprocess.run):
    fields = control_fields(path, label, run)
    architecture = 'x86_64' if label in RPM_LABELS else 'amd64'
    expected = {'name': 'putgd', 'version': package_version, 'architecture': architecture}
    if fields != expected:
        raise SignerError(f'{path.name} states {fields}, expected {expected}')
    return fields


def verify_release(release, github, workdir, run=subprocess.run):
    """Download and verify one published release; returns the inputs the repositories are built from."""
    tag = release['tag_name']
    identity = parse_tag(tag)
    channel = repo_channel(tag)
    if release.get('draft') or channel is None:
        raise SignerError('Only published nightly and stable releases feed the repositories')
    if bool(release.get('prerelease')) != (channel == 'nightly'):
        raise SignerError('Release prerelease flag does not match its tag')
    assets = {}
    for asset in release.get('assets', []):
        name = asset['name']
        if not SAFE_NAME.match(name) or name in assets:
            raise SignerError('Unsafe or duplicate asset name: ' + name)
        if asset.get('state', 'uploaded') != 'uploaded':
            raise SignerError('Asset is not fully uploaded: ' + name)
        assets[name] = asset
    for required in ('SHA256SUMS', 'release-builds.json'):
        if required not in assets:
            raise SignerError('Release has no ' + required)
    workdir.mkdir(parents=True, exist_ok=True)
    checks = []

    def fetch(name, limit):
        asset = assets[name]
        if asset['size'] > limit:
            raise SignerError(name + ' is larger than allowed')
        path = workdir / name
        github.download(asset['browser_download_url'], path, asset['size'])
        digest = sha256_file(path)
        if asset.get('digest') and asset['digest'] != 'sha256:' + digest:
            raise SignerError(name + ' differs from the digest GitHub reports')
        return path, digest

    sums_path, _ = fetch('SHA256SUMS', 1024 * 1024)
    sums = update_feed.parse_sums(sums_path.read_text())
    if set(sums) != set(assets) - {'SHA256SUMS'}:
        raise SignerError('SHA256SUMS does not list exactly the release assets')
    builds_path, builds_digest = fetch('release-builds.json', MAX_JSON)
    if builds_digest != sums['release-builds.json']:
        raise SignerError('release-builds.json differs from SHA256SUMS')
    checks.append('SHA256SUMS lists exactly the release assets')
    builds = json.loads(builds_path.read_text())
    source = builds.get('release', {})
    commit = github.tag_commit(tag)
    expected = {'tag': tag, 'version': identity.version, 'packageVersion': identity.package_version, 'gitCommit': commit}
    if any(source.get(key) != value for key, value in expected.items()):
        raise SignerError('release-builds.json names another tag, version or commit')
    checks.append('tag resolves to the commit release-builds.json names')
    packages = {}
    for name in sorted(assets):
        match = PACKAGE_ASSET.match(name)
        if not match:
            continue
        label = match['label']
        if match['version'] != identity.version or label in packages:
            raise SignerError('Unexpected package asset: ' + name)
        receipt = builds.get('receipts', {}).get(label, {}).get('package', {})
        if (receipt.get('sha256') != sums[name] or receipt.get('size') != assets[name]['size']
                or receipt.get('format') != match['ext'].upper()):
            raise SignerError(name + ' differs from its release-builds.json receipt')
        path, digest = fetch(name, MAX_PACKAGE)
        if digest != sums[name]:
            raise SignerError(name + ' differs from SHA256SUMS')
        fields = check_package(path, label, identity.package_version, run)
        packages[label] = {'name': name, 'path': path, 'sha256': digest, 'size': assets[name]['size'],
                           'checks': ['SHA256SUMS', 'release-builds.json receipt',
                                      'GitHub digest' if assets[name].get('digest') else 'GitHub digest absent',
                                      'package fields ' + ' '.join(fields.values())]}
    if set(packages) != set(LABELS):
        raise SignerError('Release lacks packages for ' + ', '.join(sorted(set(LABELS) - set(packages))))
    return {'tag': tag, 'channel': channel, 'version': identity.version, 'packageVersion': identity.package_version,
            'commit': commit, 'release': release, 'sums': sums, 'sumsPath': sums_path, 'packages': packages,
            'checks': checks, 'source': 'github'}


def local_inputs(tag, directory, run=subprocess.run):
    """Lab inputs: locally built, tagged packages beside their SHA256SUMS."""
    identity = parse_tag(tag)
    if identity.channel != 'nightly':
        raise SignerError('Lab publication takes nightly tags only')
    directory = Path(directory)
    sums_path = directory / 'SHA256SUMS'
    sums = update_feed.parse_sums(sums_path.read_text())
    packages = {}
    for name, digest in sorted(sums.items()):
        match = PACKAGE_ASSET.match(name)
        if not match:
            continue
        if match['version'] != identity.version or match['label'] in packages:
            raise SignerError('Unexpected package: ' + name)
        path = directory / name
        if path.is_symlink() or not path.is_file() or sha256_file(path) != digest:
            raise SignerError(name + ' differs from SHA256SUMS')
        fields = check_package(path, match['label'], identity.package_version, run)
        packages[match['label']] = {'name': name, 'path': path, 'sha256': digest, 'size': path.stat().st_size,
                                    'checks': ['SHA256SUMS', 'package fields ' + ' '.join(fields.values())]}
    if set(packages) != set(LABELS):
        raise SignerError('Lab directory lacks packages for ' + ', '.join(sorted(set(LABELS) - set(packages))))
    published = iso(now_utc())
    release = {'tag_name': tag, 'published_at': published,
               'html_url': f'https://github.com/{REPOSITORY}/releases/tag/{tag}',
               'assets': [{'name': p['name'], 'size': p['size'],
                           'browser_download_url': f'https://github.com/{REPOSITORY}/releases/download/{tag}/{p["name"]}'}
                          for p in packages.values()]}
    return {'tag': tag, 'channel': 'nightly', 'version': identity.version, 'packageVersion': identity.package_version,
            'commit': None, 'release': release, 'sums': sums, 'sumsPath': sums_path, 'packages': packages,
            'checks': ['lab build: SHA256SUMS and package fields only'], 'source': 'lab'}


# --- Workspaces ----------------------------------------------------------------------------------------------

def clone_workspace(source, destination):
    """Copy a workspace; package files are hard links (never modified), everything else is copied."""
    if destination.exists():
        shutil.rmtree(destination)
    if not source.exists():
        destination.mkdir(parents=True)
        return

    def copy(src, dst):
        if src.endswith(('.deb', '.rpm')):
            os.link(src, dst)
        else:
            shutil.copy2(src, dst)
    shutil.copytree(source, destination, symlinks=False, copy_function=copy)


def distributions_stanza(channel, label, fingerprint):
    lines = ['Origin: put.gd', 'Label: put.gd', f'Suite: {channel}', f'Codename: {channel}', 'Architectures: amd64',
             'Components: main', f'Description: put.gd {channel} packages for {label}']
    # Stable is signed on the workstation; reprepro then writes only Release.
    if fingerprint:
        lines.append('SignWith: ' + fingerprint)
    return '\n'.join(lines) + '\n'


class Workspace:
    def __init__(self, root, channel, config, run=subprocess.run):
        self.root, self.channel, self.config, self.run = Path(root), channel, config, run
        self.tree = self.root / 'tree'

    def env(self):
        return dict(os.environ, GNUPGHOME=str(self.config.gnupghome))

    def reprepro(self, label, *arguments):
        command = ['reprepro', '--silent', '--basedir', str(self.root / 'reprepro' / label),
                   '--outdir', str(self.tree / 'apt' / label), *arguments]
        self.run(command, check=True, env=self.env(), capture_output=True, text=True)

    def rpm_dir(self, label='fedora44'):
        return self.tree / RPM_LABELS[label] / self.channel

    def prepare(self):
        signer = self.config.nightly_fingerprint if self.channel == 'nightly' else None
        for label in DEB_LABELS:
            conf = self.root / 'reprepro' / label / 'conf'
            conf.mkdir(parents=True, exist_ok=True)
            (conf / 'distributions').write_text(distributions_stanza(self.channel, label, signer))
        (self.rpm_dir() / 'Packages').mkdir(parents=True, exist_ok=True)

    def published(self):
        path = self.root / 'published.json'
        return json.loads(path.read_text()) if path.exists() else None

    def export_empty(self):
        for label in DEB_LABELS:
            self.reprepro(label, 'export', self.channel)
        self.createrepo(None)

    def include(self, inputs, rpm_path):
        for label in DEB_LABELS:
            self.reprepro(label, '-C', 'main', 'includedeb', self.channel, str(inputs['packages'][label]['path']))
        self.createrepo(rpm_path)

    def createrepo(self, rpm_path):
        packages = self.rpm_dir() / 'Packages'
        if rpm_path:
            # The header's name, not the release asset's: dnf users see this one.
            target = packages / rpm_file_name(rpm_path, self.run)
            if not target.exists():
                shutil.copy2(rpm_path, target)
        # Keep the newest few for `dnf downgrade`; older ones stay on GitHub.
        rpms = sorted(packages.glob('putgd-*.rpm'), key=lambda path: version_key(rpm_version(path.name)), reverse=True)
        for old in rpms[self.config.dnf_keep:]:
            old.unlink()
        self.run(['createrepo_c', '--quiet', '--update', '--unique-md-filenames', '--checksum', 'sha256',
                  '--no-database', str(self.rpm_dir())], check=True, capture_output=True, text=True)

    def sign_nightly(self):
        """Detached signatures the builder makes itself; reprepro already wrote InRelease and Release.gpg."""
        repomd = self.rpm_dir() / 'repodata' / 'repomd.xml'
        signature = repomd.with_name('repomd.xml.asc')
        self.run(['gpg', '--batch', '--yes', '--quiet', '--local-user', self.config.nightly_fingerprint, '--armor',
                  '--detach-sign', '--output', str(signature), str(repomd)], check=True, env=self.env(),
                 capture_output=True, text=True)
        return [repomd]

    def write_published(self, inputs):
        record = {'tag': inputs['tag'], 'version': inputs['version'], 'publishedAt': iso(now_utc())} if inputs else None
        (self.root / 'published.json').write_text(json.dumps(record) + '\n')


def rpm_version(filename):
    """putgd-0.1.1~nightly.588-1.fc44.x86_64.rpm -> 0.1.1-nightly.588."""
    match = re.match(r'^putgd-(?P<version>[0-9.]+(?:~(?:nightly|rc)\.[0-9]+)?)-', filename)
    if not match:
        raise SignerError('Unexpected RPM file name: ' + filename)
    return match['version'].replace('~', '-')


def rpm_file_name(path, run=subprocess.run):
    result = run(['rpm', '-qp', '--nosignature', '--nodigest', '--qf', '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}.rpm',
                  str(path)], capture_output=True, text=True, check=True)
    name = result.stdout.strip()
    if not SAFE_NAME.match(name):
        raise SignerError('Unexpected RPM header name: ' + name)
    return name


# --- Signatures ----------------------------------------------------------------------------------------------

def minisign(config, path, trusted, run=subprocess.run):
    signature = path.with_name(path.name + '.minisig')
    run(['minisign', '-S', '-q', '-s', str(config.minisign_key), '-m', str(path), '-x', str(signature), '-t', trusted],
        check=True, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    return signature


def verify_gpg(keyring, signed, signature=None, run=subprocess.run):
    """True when signed (clearsigned, or with a detached signature) verifies against exactly this keyring."""
    command = ['gpgv', '--keyring', str(keyring)] + ([str(signature), str(signed)] if signature else [str(signed)])
    return run(command, capture_output=True, text=True).returncode == 0


def verify_minisign(public_key, path, signature, run=subprocess.run):
    return run(['minisign', '-V', '-q', '-p', str(public_key), '-m', str(path), '-x', str(signature)],
               capture_output=True, text=True).returncode == 0


def clearsigned_body(path):
    """The text a clearsigned InRelease covers, with dash-escaping undone."""
    lines = Path(path).read_text().split('\n')
    start = lines.index('') + 1
    end = lines.index('-----BEGIN PGP SIGNATURE-----')
    return '\n'.join(line[2:] if line.startswith('- ') else line for line in lines[start:end]) + '\n'


# --- Publishing ----------------------------------------------------------------------------------------------

def content_type(key):
    name = key.rsplit('/', 1)[-1]
    if name in ('InRelease', 'Release', 'Packages', 'SHA256SUMS'):
        return 'text/plain; charset=utf-8'
    return CONTENT_TYPES.get(Path(name).suffix, 'application/octet-stream')


def cache_control(key):
    name = key.rsplit('/', 1)[-1]
    if name.endswith(('.deb', '.rpm')) or ('/repodata/' in key and re.match(r'^[0-9a-f]{64}-', name)):
        return IMMUTABLE
    return SHORT


def phase(key):
    """Upload order: packages, indexes, signed top-level metadata, per-release files; feeds and receipts last."""
    name = key.rsplit('/', 1)[-1]
    if name.endswith(('.deb', '.rpm')):
        return 0
    if '/update/' in '/' + key or '/receipts/' in '/' + key:
        return 4
    if '/releases/' in '/' + key:
        return 3
    if name in TOP_METADATA:
        return 2
    return 1


def tree_files(tree):
    files = {}
    if tree.exists():
        for path in sorted(tree.rglob('*')):
            if path.is_file() and not path.name.endswith('.part'):
                files[path.relative_to(tree).as_posix()] = path
    return files


class Publisher:
    """Uploads a workspace tree to the bucket under a scope prefix, in phases, and tracks what it put there."""

    def __init__(self, config, scope, channel, s3, cloudfront, http=None, clock=time.time, sleep=time.sleep):
        self.config, self.prefix, self.channel = config, SCOPES[scope], channel
        self.s3, self.cloudfront = s3, cloudfront
        self.http = http or (lambda url: urllib.request.urlopen(
            urllib.request.Request(url, headers={'User-Agent': 'put.gd-pkg-signer/1', 'Cache-Control': 'no-cache'}),
            timeout=60).read())
        self.clock, self.sleep = clock, sleep
        state = config.state / 'published' / f'{scope}-{channel}.json'
        self.state_path = state
        self.state = json.loads(state.read_text()) if state.exists() else {'objects': {}, 'pending': {}}

    def save(self):
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.state_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(self.state, indent=1, sort_keys=True) + '\n')
        temporary.replace(self.state_path)

    def upload(self, tree, phases, receipt):
        """Upload every changed file of these phases; returns the keys that need invalidating."""
        files = tree_files(tree)
        changed = []
        for relative, path in sorted(files.items(), key=lambda item: (phase(item[0]), item[0])):
            if phase(relative) not in phases:
                continue
            key = self.prefix + relative
            digest = sha256_file(path)
            if self.state['objects'].get(key) == digest:
                continue
            with open(path, 'rb') as body:
                self.s3.put_object(Bucket=self.config.bucket, Key=key, Body=body, ContentType=content_type(key),
                                   CacheControl=cache_control(key))
            self.state['objects'][key] = digest
            self.state['pending'].pop(key, None)
            self.save()
            receipt['s3']['uploaded'].append({'key': key, 'sha256': digest})
            if cache_control(key) != IMMUTABLE:
                changed.append(key)
        return changed

    def retire(self, tree, receipt):
        """Mark objects that left the tree; delete those gone for longer than delete_after."""
        present = {self.prefix + relative for relative in tree_files(tree)}
        moment = self.clock()
        for key in sorted(set(self.state['objects']) - present):
            self.state['pending'].setdefault(key, moment)
        due = [key for key, since in sorted(self.state['pending'].items()) if moment - since >= self.config.delete_after]
        for key in due:
            self.s3.delete_object(Bucket=self.config.bucket, Key=key)
            self.state['objects'].pop(key, None)
            self.state['pending'].pop(key)
            receipt['s3']['deleted'].append(key)
        self.save()
        return due

    def invalidate(self, keys, receipt, distributions=None):
        if not keys:
            return
        paths = sorted({'/' + key for key in keys})
        for distribution in distributions or [self.config.distribution]:
            for start in range(0, len(paths), 1000):
                batch = paths[start:start + 1000]
                reference = f'putgd-signer-{int(self.clock() * 1000)}-{start}'
                answer = self.cloudfront.create_invalidation(DistributionId=distribution, InvalidationBatch={
                    'Paths': {'Quantity': len(batch), 'Items': batch}, 'CallerReference': reference})
                identifier = answer['Invalidation']['Id']
                receipt['invalidations'].append({'distribution': distribution, 'id': identifier, 'paths': len(batch)})
                deadline = self.clock() + self.config.invalidation_timeout
                while self.cloudfront.get_invalidation(DistributionId=distribution, Id=identifier)[
                        'Invalidation']['Status'] != 'Completed':
                    if self.clock() > deadline:
                        raise SignerError('Invalidation did not complete: ' + identifier)
                    self.sleep(10)

    def readback(self, tree, relatives, receipt):
        """Fetch each file through the CDN until it matches the local bytes."""
        deadline = self.clock() + self.config.readback_timeout
        for relative in relatives:
            expected = sha256_file(tree / relative)
            url = f'{self.config.base_url}/{self.prefix}{relative}'
            while True:
                try:
                    served = hashlib.sha256(self.http(url)).hexdigest()
                except Exception:  # retried until the deadline
                    served = None
                if served == expected:
                    receipt['readback'].append({'url': url, 'sha256': expected})
                    break
                if self.clock() > deadline:
                    raise SignerError('The CDN does not serve the new ' + relative)
                self.sleep(15)


def readback_files(workspace):
    files = [f'apt/{label}/dists/{workspace.channel}/{name}' for label in DEB_LABELS
             for name in ('InRelease', 'main/binary-amd64/Packages')]
    return files + [f'{RPM_LABELS["fedora44"]}/{workspace.channel}/repodata/repomd.xml']


def aws_clients(config):
    import boto3  # only the signer host needs it
    session = boto3.Session(profile_name=config.aws_profile)
    return session.client('s3'), session.client('cloudfront')


# --- Runs ----------------------------------------------------------------------------------------------------

def new_receipt(inputs, scope):
    return {'schemaVersion': 1, 'tag': inputs['tag'] if inputs else None, 'channel': None, 'scope': scope,
            'startedAt': iso(now_utc()), 'finishedAt': None, 'result': 'running',
            'source': {}, 'inputs': [], 'rpm': [], 'signatures': [], 's3': {'uploaded': [], 'deleted': []},
            'invalidations': [], 'readback': [], 'feed': None, 'error': None}


def write_receipt(config, receipt, name):
    directory = config.state / 'receipts' / receipt['scope']
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / (name + '.json')
    path.write_text(json.dumps(receipt, indent=1) + '\n')
    return path


class Signer:
    def __init__(self, config, github=None, clients=None, run=subprocess.run, log=print):
        self.config, self.run, self.log = config, run, log
        self.github = github or GitHub(config.repository)
        self._clients = clients

    def clients(self):
        if self._clients is None:
            self._clients = aws_clients(self.config)
        return self._clients

    def workspace(self, scope, channel, name='current'):
        return Workspace(self.config.state / 'workspaces' / scope / channel / name, channel, self.config, self.run)

    def stage_root(self, scope, tag):
        return self.config.state / 'stage' / scope / 'stable' / (tag or 'empty')

    # Nightly (and, after signing, stable) publication shares this path.
    def build_next(self, scope, channel, inputs, receipt, root=None):
        current = self.workspace(scope, channel)
        workspace = Workspace(root or current.root.with_name('next'), channel, self.config, self.run)
        clone_workspace(current.root, workspace.root)
        workspace.prepare()
        if inputs is None:
            workspace.export_empty()
            return workspace
        rpm = inputs['packages']['fedora44']
        if self.config.rpm_sign and channel == 'nightly':
            with tempfile.TemporaryDirectory(prefix='rpmsign-', dir=self.config.state) as directory:
                workspace.include(inputs, self.rpm_signed_copy(rpm, Path(directory), receipt))
        else:
            workspace.include(inputs, rpm['path'])
        sums = workspace.tree / 'releases' / inputs['tag'] / 'SHA256SUMS'
        sums.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(inputs['sumsPath'], sums)
        feed = workspace.tree / 'update' / 'v1' / f'{channel}.json'
        feed.parent.mkdir(parents=True, exist_ok=True)
        document = update_feed.feed(channel, inputs['release'], inputs['sums'], iso(now_utc()))
        update_feed.check_feed(document)
        feed.write_text(json.dumps(document, indent=2) + '\n')
        return workspace

    def rpm_signed_copy(self, rpm, directory, receipt):
        target = directory / rpm_file_name(rpm['path'], self.run)
        shutil.copy2(rpm['path'], target)
        self.run(['rpmsign', '--define', '_gpg_name ' + self.config.nightly_fingerprint, '--define',
                  '_gpg_path ' + str(self.config.gnupghome), '--addsign', str(target)], check=True,
                 capture_output=True, text=True, env=dict(os.environ, GNUPGHOME=str(self.config.gnupghome)))
        receipt['rpm'].append({'asset': rpm['name'], 'githubSha256': rpm['sha256'], 'repoSha256': sha256_file(target),
                               'signedWith': self.config.nightly_fingerprint})
        return target

    def record_inputs(self, receipt, inputs):
        receipt['channel'] = inputs['channel']
        receipt['source'] = {'kind': inputs['source'], 'repository': self.config.repository, 'commit': inputs['commit'],
                             'checks': inputs['checks']}
        receipt['inputs'] = [{'label': label, 'name': package['name'], 'sha256': package['sha256'],
                              'size': package['size'], 'checks': package['checks']}
                             for label, package in sorted(inputs['packages'].items())]

    def sign_nightly(self, workspace, inputs, receipt):
        signed = workspace.sign_nightly()
        for label in DEB_LABELS:
            for name in ('InRelease', 'Release.gpg'):
                path = workspace.tree / 'apt' / label / 'dists' / workspace.channel / name
                if not path.exists():
                    raise SignerError('reprepro did not sign ' + label)
                signed.append(path)
        extra = []
        if inputs:
            trusted = f'put.gd {inputs["tag"]} {workspace.channel}'
            extra.append(minisign(self.config, workspace.tree / 'releases' / inputs['tag'] / 'SHA256SUMS', trusted, self.run))
            extra.append(minisign(self.config, workspace.tree / 'update' / 'v1' / f'{workspace.channel}.json', trusted, self.run))
        keyring = self.config.keys / 'putgd-nightly.gpg'
        for path in signed:
            # (signed file, detached signature); InRelease carries its own.
            pair = {'repomd.xml': (path, path.with_name('repomd.xml.asc')), 'InRelease': (path, None),
                    'Release.gpg': (path.with_name('Release'), path)}[path.name]
            if not verify_gpg(keyring, *pair, run=self.run):
                raise SignerError('A signature does not verify against the nightly key: ' + path.name)
            receipt['signatures'].append({'file': (pair[1] or pair[0]).relative_to(workspace.tree).as_posix(),
                                          'kind': 'openpgp', 'key': self.config.nightly_fingerprint})
        for path in extra:
            receipt['signatures'].append({'file': path.relative_to(workspace.tree).as_posix(), 'kind': 'minisign',
                                          'key': 'nightly'})

    def publish(self, scope, workspace, receipt):
        """Upload, invalidate, read back, then the feed and receipt; swap the workspace in on success."""
        s3, cloudfront = self.clients()
        publisher = Publisher(self.config, scope, workspace.channel, s3, cloudfront)
        changed = publisher.upload(workspace.tree, {0, 1, 2, 3}, receipt)
        publisher.invalidate(changed, receipt)
        publisher.readback(workspace.tree, readback_files(workspace), receipt)
        feed_keys = publisher.upload(workspace.tree, {4}, receipt)
        feed_distributions = [self.config.distribution] + (self.config.feed_distributions if scope == 'live' else [])
        publisher.invalidate([key for key in feed_keys if '/update/' in '/' + key], receipt, feed_distributions)
        feed = workspace.tree / 'update' / 'v1' / f'{workspace.channel}.json'
        if feed.exists():
            receipt['feed'] = {'key': SCOPES[scope] + f'update/v1/{workspace.channel}.json', 'sha256': sha256_file(feed)}
        current = self.workspace(scope, workspace.channel)
        retired = current.root.with_name('previous')
        if retired.exists():
            shutil.rmtree(retired)
        if current.root.exists():
            current.root.rename(retired)
        workspace.root.rename(current.root)
        shutil.rmtree(retired, ignore_errors=True)
        publisher.retire(current.tree, receipt)
        return publisher

    def finish(self, scope, receipt, name, publisher=None):
        receipt['finishedAt'] = iso(now_utc())
        path = write_receipt(self.config, receipt, name)
        if publisher and receipt['result'] == 'published':
            s3, _ = self.clients()
            key = SCOPES[scope] + f'receipts/{name}.json'
            s3.put_object(Bucket=self.config.bucket, Key=key, Body=path.read_bytes(), ContentType='application/json',
                          CacheControl=SHORT)
        self.log(json.dumps({'receipt': str(path), 'tag': receipt['tag'], 'result': receipt['result'],
                             'error': receipt['error']}))
        return path

    def publish_nightly(self, scope, inputs):
        receipt = new_receipt(inputs, scope)
        publisher = None
        try:
            self.record_inputs(receipt, inputs)
            workspace = self.build_next(scope, 'nightly', inputs, receipt)
            self.sign_nightly(workspace, inputs, receipt)
            publisher = self.publish(scope, workspace, receipt)
            self.workspace(scope, 'nightly').write_published(inputs)
            receipt['result'] = 'published'
        except Exception as error:
            receipt['result'], receipt['error'] = 'failed', f'{type(error).__name__}: {error}'
            self.finish(scope, receipt, inputs['tag'] + '.failed')
            raise
        return self.finish(scope, receipt, inputs['tag'], publisher)

    # Stable: stage here, sign on the workstation, publish after checking the signatures.
    def stage_stable(self, scope, inputs):
        stage = self.stage_root(scope, inputs['tag'] if inputs else None)
        if (stage / 'manifest.json').exists():
            return stage
        receipt = new_receipt(inputs, scope)
        if inputs:
            self.record_inputs(receipt, inputs)
        else:
            receipt['channel'] = 'stable'
        stage.mkdir(parents=True, exist_ok=True)
        workspace = self.build_next(scope, 'stable', inputs, receipt, root=stage / 'workspace')
        if inputs is None:
            feed = workspace.tree / 'update' / 'v1' / 'stable.json'
            feed.parent.mkdir(parents=True, exist_ok=True)
            document = update_feed.feed('stable', None, {}, iso(now_utc()))
            update_feed.check_feed(document)
            feed.write_text(json.dumps(document, indent=2) + '\n')
        to_sign = {'clearsign': [f'apt/{label}/dists/stable/Release' for label in DEB_LABELS],
                   'detach': [f'{RPM_LABELS["fedora44"]}/stable/repodata/repomd.xml'],
                   'minisign': ['update/v1/stable.json'] + ([f'releases/{inputs["tag"]}/SHA256SUMS'] if inputs else [])}
        check = {'packages': [f'apt/{label}/dists/stable/main/binary-amd64/Packages' for label in DEB_LABELS],
                 'primary': [f'{RPM_LABELS["fedora44"]}/stable/repodata/' + path.name
                             for path in (workspace.rpm_dir() / 'repodata').glob('*-primary.xml*')]}
        manifest = {'schemaVersion': 1, 'scope': scope, 'tag': inputs['tag'] if inputs else None,
                    'stableFingerprint': self.config.stable_fingerprint, 'sign': to_sign, 'check': check,
                    'sha256': {relative: sha256_file(workspace.tree / relative)
                               for group in (to_sign, check) for paths in group.values() for relative in paths},
                    'stagedAt': iso(now_utc())}
        (stage / 'signatures').mkdir(exist_ok=True)
        (stage / 'receipt.json').write_text(json.dumps(receipt, indent=1) + '\n')
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
        self.log(json.dumps({'staged': str(stage), 'tag': manifest['tag'],
                             'next': 'python3 tools/releases/sign_stable.py on the workstation'}))
        return stage

    def stable_signed(self, stage):
        manifest = json.loads((stage / 'manifest.json').read_text())
        signatures = stage / 'signatures'
        wanted = ([relative + suffix for relative in manifest['sign']['clearsign'] for suffix in ('.InRelease', '.gpg')]
                  + [relative + '.asc' for relative in manifest['sign']['detach']]
                  + [relative + '.minisig' for relative in manifest['sign']['minisign']])
        return all((signatures / name.replace('/', '__')).is_file() for name in wanted)

    def publish_stable(self, scope, stage):
        manifest = json.loads((stage / 'manifest.json').read_text())
        receipt = json.loads((stage / 'receipt.json').read_text())
        workspace = Workspace(stage / 'workspace', 'stable', self.config, self.run)
        signatures = stage / 'signatures'
        keyring = self.config.keys / 'putgd-stable.gpg'
        public = self.config.keys / 'putgd-stable.minisign.pub'
        name = manifest['tag'] or 'stable-empty'
        publisher = None
        try:
            for relative, digest in manifest['sha256'].items():
                if sha256_file(workspace.tree / relative) != digest:
                    raise SignerError('Staged file changed after staging: ' + relative)
            for relative in manifest['sign']['clearsign']:
                release = workspace.tree / relative
                inrelease = signatures / (relative.replace('/', '__') + '.InRelease')
                detached = signatures / (relative.replace('/', '__') + '.gpg')
                if not verify_gpg(keyring, inrelease, None, self.run) or clearsigned_body(inrelease) != release.read_text():
                    raise SignerError('InRelease does not verify against the stable key: ' + relative)
                if not verify_gpg(keyring, release, detached, self.run):
                    raise SignerError('Release.gpg does not verify against the stable key: ' + relative)
                shutil.copy2(inrelease, release.with_name('InRelease'))
                shutil.copy2(detached, release.with_name('Release.gpg'))
                receipt['signatures'] += [{'file': relative.rsplit('/', 1)[0] + '/' + file, 'kind': 'openpgp',
                                           'key': self.config.stable_fingerprint} for file in ('InRelease', 'Release.gpg')]
            for relative in manifest['sign']['detach']:
                signature = signatures / (relative.replace('/', '__') + '.asc')
                if not verify_gpg(keyring, workspace.tree / relative, signature, self.run):
                    raise SignerError('repomd.xml.asc does not verify against the stable key')
                shutil.copy2(signature, workspace.tree / (relative + '.asc'))
                receipt['signatures'].append({'file': relative + '.asc', 'kind': 'openpgp', 'key': self.config.stable_fingerprint})
            for relative in manifest['sign']['minisign']:
                signature = signatures / (relative.replace('/', '__') + '.minisig')
                if not verify_minisign(public, workspace.tree / relative, signature, self.run):
                    raise SignerError('minisign signature does not verify against the stable key: ' + relative)
                shutil.copy2(signature, workspace.tree / (relative + '.minisig'))
                receipt['signatures'].append({'file': relative + '.minisig', 'kind': 'minisign', 'key': 'stable'})
            publisher = self.publish(scope, workspace, receipt)
            self.workspace(scope, 'stable').write_published(
                {'tag': manifest['tag'], 'version': manifest['tag'][1:]} if manifest['tag'] else None)
            receipt['result'] = 'published'
            shutil.rmtree(stage)
        except Exception as error:
            receipt['result'], receipt['error'] = 'failed', f'{type(error).__name__}: {error}'
            self.finish(scope, receipt, name + '.failed')
            raise
        return self.finish(scope, receipt, name, publisher)

    def retire_only(self, scope):
        """Delete objects whose grace period ended, without publishing anything."""
        for channel in CHANNELS:
            current = self.workspace(scope, channel)
            if not current.root.exists():
                continue
            s3, cloudfront = self.clients()
            receipt = new_receipt(None, scope)
            deleted = Publisher(self.config, scope, channel, s3, cloudfront).retire(current.tree, receipt)
            if deleted:
                receipt.update(channel=channel, result='retired')
                self.finish(scope, receipt, f'retire-{channel}-{int(time.time())}')

    def run_once(self):
        """One timer run against the live scope."""
        releases = self.github.releases()
        newest = {}
        for release in releases:
            try:
                channel = repo_channel(release['tag_name'])
            except ValueError:
                continue
            if channel and (channel not in newest or version_key(release['tag_name'][1:]) >
                            version_key(newest[channel]['tag_name'][1:])):
                newest[channel] = release
        done = []
        nightly = newest.get('nightly')
        published = self.workspace('live', 'nightly').published()
        if nightly and (not published or version_key(nightly['tag_name'][1:]) > version_key(published['version'])):
            workdir = self.config.state / 'downloads' / nightly['tag_name']
            try:
                inputs = verify_release(nightly, self.github, workdir, self.run)
            except Exception as error:
                receipt = new_receipt({'tag': nightly['tag_name']}, 'live')
                receipt.update(channel='nightly', result='rejected', error=f'{type(error).__name__}: {error}')
                self.finish('live', receipt, nightly['tag_name'] + '.failed')
                raise
            self.publish_nightly('live', inputs)
            shutil.rmtree(workdir, ignore_errors=True)
            done.append(nightly['tag_name'])
        stable = newest.get('stable')
        published = self.workspace('live', 'stable').published()
        if stable and (not published or version_key(stable['tag_name'][1:]) > version_key(published['version'])):
            stage = self.stage_root('live', stable['tag_name'])
            if not (stage / 'manifest.json').exists():
                inputs = verify_release(stable, self.github, self.config.state / 'downloads' / stable['tag_name'], self.run)
                self.stage_stable('live', inputs)
            elif self.stable_signed(stage):
                self.publish_stable('live', stage)
                done.append(stable['tag_name'])
        empty = self.stage_root('live', None)
        if (empty / 'manifest.json').exists() and self.stable_signed(empty):
            self.publish_stable('live', empty)
            done.append('stable-empty')
        self.retire_only('live')
        return done


def default_config_path():
    return Path(os.path.expanduser('~/.config/putgd-signer/signer.json'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--config', type=Path, default=default_config_path())
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('run', help='poll GitHub and publish (the timer runs this)')
    local = commands.add_parser('publish-local', help='publish locally built, tagged nightly packages (lab scope)')
    local.add_argument('--tag', required=True)
    local.add_argument('--assets', type=Path, required=True)
    local.add_argument('--scope', choices=['lab'], default='lab')
    commands.add_parser('init-stable', help='stage empty stable repositories for the workstation to sign')
    commands.add_parser('status', help='print what each scope and channel carries')
    arguments = parser.parse_args(argv)
    config = Config.load(arguments.config)
    config.state.mkdir(parents=True, exist_ok=True)
    with open(config.state / 'lock', 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(json.dumps({'skipped': 'another signer run holds the lock'}))
            return 0
        signer = Signer(config)
        if arguments.command == 'run':
            print(json.dumps({'published': signer.run_once()}))
        elif arguments.command == 'publish-local':
            published = signer.workspace(arguments.scope, 'nightly').published()
            if published and version_key(arguments.tag[1:]) <= version_key(published['version']):
                raise SignerError('The lab repository already carries ' + published['version'])
            signer.publish_nightly(arguments.scope, local_inputs(arguments.tag, arguments.assets))
        elif arguments.command == 'init-stable':
            if signer.workspace('live', 'stable').root.exists():
                raise SignerError('The stable repositories already exist')
            signer.stage_stable('live', None)
        else:
            status = {scope: {channel: signer.workspace(scope, channel).published() for channel in CHANNELS}
                      for scope in SCOPES}
            status['staged'] = sorted(str(path.parent.relative_to(config.state))
                                      for path in (config.state / 'stage').rglob('manifest.json'))
            print(json.dumps(status, indent=1))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (SignerError, ValueError, subprocess.CalledProcessError) as error:
        detail = error.stderr.strip()[-500:] if isinstance(error, subprocess.CalledProcessError) and error.stderr else ''
        print(json.dumps({'error': f'{type(error).__name__}: {error}', 'detail': detail}), file=sys.stderr)
        sys.exit(1)
