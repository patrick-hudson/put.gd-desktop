#!/usr/bin/env python3
"""Sign the staged stable repositories with the stable key, on the workstation (#293 phase 2).

The signer on the builder stages each stable release (and, once, the empty stable
repositories) unsigned. This script fetches the few KiB it signs over the
existing SSH pin and checks every package hash in them against the tag's
SHA256SUMS, downloaded from GitHub independently. It downloads the fedora44 RPM
from GitHub, checks it the same way and signs it with rpmsign (dnf checks
package signatures; the repodata is unsigned, see pkgrepo.py). It signs the
apt Release files, SHA256SUMS and the feed with the stable GPG and minisign
keys (gpg-agent and minisign ask for their passphrases), and copies the
signatures and the signed RPM back. The signer's next run checks them against
the stable public keys and publishes. The stable private keys never leave this
computer. Needs gpg, minisign, rpmsign (the rpm package) and gh.

Usage: tools/releases/sign_stable.py [--tag vX.Y.Z | --empty] [--host d69]
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from release_identity import parse_tag  # noqa: E402
import update_feed  # noqa: E402

HOSTS = {'d69': ('patrick@192.168.68.69', '/home/patrick/work/put.gd/work/remote-builders/d69/known_hosts')}
STATE = '.local/state/putgd-signer'
DEB_ASSET = 'putgd-{version}-{label}-x86_64.deb'


def ssh_command(host):
    address, known_hosts = HOSTS[host]
    return ['ssh', '-i', str(Path.home() / '.ssh/awskey_110724'), '-o', 'IdentitiesOnly=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + known_hosts, address]


def remote_read(host, path):
    return subprocess.run(ssh_command(host) + ['cat', '--', path], capture_output=True, check=True).stdout


def remote_write(host, path, data):
    subprocess.run(ssh_command(host) + [f'umask 077 && cat > {path}.part && mv {path}.part {path}'],
                   input=data, check=True)


def stanzas(text):
    return [dict(re.findall(r'^([A-Za-z0-9-]+): ?(.*)$', block, re.M)) for block in text.split('\n\n') if block.strip()]


def release_hashes(release_text):
    """Release's SHA256 section: path -> sha256."""
    section = release_text.split('\nSHA256:\n', 1)[1]
    hashes = {}
    for line in section.splitlines():
        if not line.startswith(' '):
            break
        digest, _, name = line.split()
        hashes[name] = digest
    return hashes


def check_staged(manifest, files, tag, sums):
    """Every apt package the staged metadata names is the tag's own asset; empty repositories name none."""
    version = parse_tag(tag).package_version if tag else None
    for packages in manifest['check']['packages']:
        label = packages.split('/')[1]
        release = files[packages.rsplit('/', 3)[0] + '/Release'].decode()
        if release_hashes(release).get('main/binary-amd64/Packages') != hashlib.sha256(files[packages]).hexdigest():
            raise SystemExit(f'{label}: Release does not cover the staged Packages')
        entries = stanzas(files[packages].decode())
        expected = [] if tag is None else [(version, sums[DEB_ASSET.format(version=tag[1:], label=label)])]
        if [(entry.get('Version'), entry.get('SHA256')) for entry in entries if entry.get('Package') == 'putgd'] != expected \
                or len(entries) != len(expected):
            raise SystemExit(f'{label}: staged Packages does not list exactly {tag or "nothing"}')
    rpm = manifest.get('rpm')
    if (rpm is None) != (tag is None) or (rpm and (rpm['asset'] != f'putgd-{tag[1:]}-fedora44-x86_64.rpm'
                                                   or rpm['sha256'] != sums.get(rpm['asset']))):
        raise SystemExit('The staged RPM is not the tag\'s fedora44 asset')


def gpg(arguments, data, fingerprint):
    return subprocess.run(['gpg', '--batch', '--yes', '--local-user', fingerprint, *arguments, '--output', '-'],
                          input=data, capture_output=True, check=True).stdout


def sign_rpm(asset, file_name, digest, tag, fingerprint, directory):
    """Download the tag's RPM from GitHub, check it and sign a copy named as its header names it."""
    subprocess.run(['gh', 'release', 'download', tag, '--repo', 'patrick-hudson/put.gd-desktop', '--pattern', asset,
                    '--dir', str(directory)], check=True)
    path = Path(directory) / asset
    if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
        raise SystemExit('The downloaded RPM differs from SHA256SUMS')
    if not re.fullmatch(r'putgd-[0-9][0-9A-Za-z.~-]*\.x86_64\.rpm', file_name):
        raise SystemExit('Unexpected RPM file name in the stage')
    signed = path.with_name(file_name)
    path.rename(signed)
    subprocess.run(['rpmsign', '--define', '_gpg_name ' + fingerprint, '--addsign', str(signed)], check=True)
    return signed.read_bytes()


def minisign(path, trusted):
    signature = path.with_name(path.name + '.minisig')
    subprocess.run(['minisign', '-S', '-s', str(Path.home() / '.minisign/putgd-stable.key'), '-m', str(path),
                    '-x', str(signature), '-t', trusted], check=True)
    return signature.read_bytes()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    which = parser.add_mutually_exclusive_group(required=True)
    which.add_argument('--tag')
    which.add_argument('--empty', action='store_true', help='the empty stable repositories, signed once')
    parser.add_argument('--host', choices=sorted(HOSTS), default='d69')
    parser.add_argument('--scope', choices=['live', 'lab'], default='live')
    arguments = parser.parse_args(argv)
    if arguments.tag and parse_tag(arguments.tag).channel != 'stable':
        raise SystemExit('Only stable tags are signed with the stable key')
    stage = f'{STATE}/stage/{arguments.scope}/stable/{arguments.tag or "empty"}'
    manifest = json.loads(remote_read(arguments.host, stage + '/manifest.json'))
    if manifest.get('schemaVersion') != 2:
        raise SystemExit('The stage was made by an older signer')
    if manifest['tag'] != arguments.tag:
        raise SystemExit('The staged manifest names another tag')
    fingerprint = manifest['stableFingerprint']
    keys = subprocess.run(['gpg', '--batch', '--with-colons', '--list-secret-keys', fingerprint],
                          capture_output=True, text=True)
    if keys.returncode != 0 or f'fpr:::::::::{fingerprint}:' not in keys.stdout:
        raise SystemExit('The stable signing key the stage names is not on this computer')
    files = {}
    for relative, digest in manifest['sha256'].items():
        data = remote_read(arguments.host, f'{stage}/workspace/tree/{relative}')
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit('Staged file differs from its manifest: ' + relative)
        files[relative] = data
    sums = {}
    if arguments.tag:
        with tempfile.TemporaryDirectory(prefix='pgsign-') as directory:
            subprocess.run(['gh', 'release', 'download', arguments.tag, '--repo', 'patrick-hudson/put.gd-desktop',
                            '--pattern', 'SHA256SUMS', '--dir', directory], check=True)
            github_sums = (Path(directory) / 'SHA256SUMS').read_bytes()
        if github_sums != files[f'releases/{arguments.tag}/SHA256SUMS']:
            raise SystemExit("The staged SHA256SUMS differs from the release's")
        sums = update_feed.parse_sums(github_sums.decode())
    check_staged(manifest, files, arguments.tag, sums)
    feed = json.loads(files['update/v1/stable.json'])
    update_feed.check_feed(feed)
    if (feed['release'] or {}).get('tag') != arguments.tag:
        raise SystemExit('The staged stable feed names another release')
    print(f'Staged {arguments.tag or "empty stable repositories"}: every package hash matches; signing now.')
    signatures = {}
    for relative in manifest['sign']['clearsign']:
        signatures[relative + '.InRelease'] = gpg(['--clearsign', '--digest-algo', 'SHA512'], files[relative], fingerprint)
        signatures[relative + '.gpg'] = gpg(['--armor', '--detach-sign', '--digest-algo', 'SHA512'], files[relative],
                                            fingerprint)
    with tempfile.TemporaryDirectory(prefix='pgsign-') as directory:
        if manifest['rpm']:
            rpm = manifest['rpm']
            signatures[rpm['file']] = sign_rpm(rpm['asset'], rpm['file'], rpm['sha256'], arguments.tag, fingerprint, directory)
        for relative in manifest['sign']['minisign']:
            path = Path(directory) / Path(relative).name
            path.write_bytes(files[relative])
            signatures[relative + '.minisig'] = minisign(path, f'put.gd {arguments.tag or "stable"} stable')
    for name, data in signatures.items():
        remote_write(arguments.host, f'{stage}/signatures/{name.replace("/", "__")}', data)

    print(f'Copied {len(signatures)} signatures to {arguments.host}; the signer publishes on its next run.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
