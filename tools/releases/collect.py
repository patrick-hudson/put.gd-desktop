#!/usr/bin/env python3
"""Collect only byte-verified assets matching one exact tagged source identity."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

from audit_native import PROGRAMS, sha256
from verify_tag import verify
import proton_sources
import appimage_media_sources
from release_bundle import create_gnome_zip, create_release_bundle


def identity_matches(manifest, release):
    fields = ('version', 'baseVersion', 'packageVersion', 'gitCommit', 'gitTree', 'commitCount')
    return (all(manifest.get(field) == release[field] for field in fields) and
            manifest.get('dirty') is False and manifest.get('sourceProvenance') == 'git')


def asset_path(directory, filename, suffix):
    if Path(filename).name != filename or not filename.endswith(suffix):
        raise ValueError('Invalid release asset filename')
    path = directory / filename
    if not path.is_file() or path.is_symlink():
        raise ValueError('Release asset is missing or is a symlink')
    return path



def verified_source(directory, release):
    receipt = json.loads((directory / 'source-archive.json').read_text())
    if receipt.get('format') != 'source.tar.gz' or not identity_matches(receipt['identity'], release):
        raise ValueError('Source archive identity differs from release')
    artifact = receipt['artifact']
    archive = asset_path(directory, artifact['file'], '-source.tar.gz')
    if sha256(archive) != artifact['sha256'] or archive.stat().st_size != artifact['bytes']:
        raise ValueError('Source archive bytes differ from compiled receipt')
    build = json.loads((directory / 'source-build-results.json').read_text())
    version_suffix = f"dev.{release['commitCount']}+g{release['gitCommit'][:12]}"
    expected = {'schemaVersion': 1, 'version': release['baseVersion'] + '-' + version_suffix,
                'baseVersion': release['baseVersion'], 'packageVersion': release['baseVersion'] + '~' + version_suffix,
                'gitCommit': release['gitCommit'], 'gitTree': release['gitTree'], 'commitCount': release['commitCount'],
                'dirty': None, 'sourceProvenance': 'supplied'}
    programs = build.get('identityMatchedPrograms')
    required = set(PROGRAMS) - {'putgd-launcher'}
    # Optional helpers check_source_build.py reports when the builder has their dependencies.
    allowed = required | {'putgd-kde-region', 'putgd-record'}
    if (build.get('passed') is not True or build.get('compiledBuild') != expected
            or not isinstance(programs, list) or not all(isinstance(name, str) for name in programs)
            or len(programs) != len(set(programs)) or not required.issubset(programs)
            or not set(programs).issubset(allowed)):
        raise ValueError('Source archive has no matching successful production build')
    return archive, receipt, build


def collect(root, inputs, output, tag):
    release = verify(root, tag)
    timestamp = int(subprocess.check_output(['git', '-C', str(root), 'show', '-s', '--format=%ct',
                                             release['gitCommit']], text=True).strip())
    # Resolve everything before making an output directory or copying assets.
    selected = []
    receipts = {}
    for distro, kind, suffix in [('ubuntu24.04', 'DEB', '.deb'), ('ubuntu26.04', 'DEB', '.deb'), ('fedora44', 'RPM', '.rpm')]:
        directory = inputs / ('release-native-' + distro)
        receipt = json.loads((directory / 'native-audit.json').read_text())
        package = receipt['package']
        if not identity_matches(receipt['manifest'], release) or package['format'] != kind or package['architecture'] != ('amd64' if kind == 'DEB' else 'x86_64'):
            raise ValueError('Native asset compiled identity/format differs from release: ' + distro)
        path = asset_path(directory, package['file'], suffix)
        if sha256(path) != package['sha256'] or path.stat().st_size != package['size']:
            raise ValueError('Native asset bytes differ from audited receipt: ' + distro)
        name = 'putgd-' + release['version'] + '-' + distro + '-x86_64' + suffix
        selected.append((path, name))
        receipts[distro] = receipt
    directory = inputs / 'release-appimage'
    receipt = json.loads((directory / 'appimage-build.json').read_text())
    if receipt['format'] != 'AppImage' or receipt['architecture'] != 'x86_64' or not identity_matches(receipt['build'], release):
        raise ValueError('AppImage compiled identity/format differs from release')
    image = asset_path(directory, receipt['artifact']['file'], '.AppImage')
    if sha256(image) != receipt['artifact']['sha256'] or image.stat().st_size != receipt['artifact']['bytes']:
        raise ValueError('AppImage bytes differ from audited receipt')
    selected.append((image, 'putgd-' + release['version'] + '-x86_64.AppImage'))
    receipts['appimage'] = receipt
    # The image is a GPL combined work: its codec sources ship beside it.
    media_receipt = json.loads((directory / appimage_media_sources.RECEIPT_NAME).read_text())
    media_source = appimage_media_sources.verify(directory, media_receipt, receipt)
    selected.append((media_source, appimage_media_sources.archive_name(release['version'])))
    receipts['appimageMediaSource'] = media_receipt
    source, source_receipt, source_build = verified_source(inputs / 'release-native-ubuntu24.04', release)
    selected.append((source, 'putgd-' + release['version'] + '-source.tar.gz'))
    receipts['source'] = source_receipt
    receipts['sourceBuild'] = source_build
    source_directory = inputs / 'proton-sources'
    catalog_path = source_directory / proton_sources.CATALOG_NAME
    current_catalog = root / 'tools/proton/notices/SOURCES.json'
    if (catalog_path.is_symlink() or not catalog_path.is_file()
            or catalog_path.read_bytes() != current_catalog.read_bytes()):
        raise ValueError('Proton source catalog differs from current release source')
    catalog = proton_sources.catalog_value(catalog_path)
    proton_sources.verify(source_directory, catalog)
    catalog_sha = sha256(catalog_path)
    rebuilt = source_build.get('rebuiltProton', {})
    if (rebuilt.get('sha256') != catalog['helperSha256']
            or rebuilt.get('bundledSha256') != catalog['helperSha256']):
        raise ValueError('Source build has no matching rebuilt Proton helper')
    for kind in ('ubuntu24.04', 'ubuntu26.04', 'fedora44', 'appimage'):
        inventory = (receipts[kind].get('postDeploymentAudit', {}).get('proton', {}) if kind == 'appimage'
                     else receipts[kind].get('proton', {}))
        if (inventory.get('runtimeSha256') != catalog['helperSha256']
                or inventory.get('sourceCatalogSha256') != catalog_sha):
            raise ValueError('Proton package runtime/source catalog differs from delivered source: ' + kind)
    for name in (*proton_sources.PART_NAMES, proton_sources.CATALOG_NAME):
        selected.append((source_directory / name, name))
    receipts['protonSource'] = {'helperSha256': catalog['helperSha256'],
        'sourceCatalogSha256': catalog_sha, 'delivery': catalog['delivery']}
    output.mkdir(parents=True, exist_ok=False)
    for path, name in selected:
        shutil.copy2(path, output / name)
    # GNOME integration is a host extension, including for AppImage users.
    extension = output / ('putgd-gnome50-' + release['version'] + '.zip')
    create_gnome_zip(root, extension, timestamp)
    (output / 'release-builds.json').write_text(json.dumps({'schemaVersion': 1, 'release': release, 'receipts': receipts}, indent=2) + '\n')
    member_names = [name for _, name in selected if name not in proton_sources.PART_NAMES] + [extension.name, 'release-builds.json']
    combined = output / ('putgd-' + release['version'] + '-release.zip')
    create_release_bundle(output, combined, release['version'], timestamp, member_names)
    (output / 'SHA256SUMS').write_text(''.join(sha256(output / name) + '  ' + name + '\n'
        for name in sorted([*member_names, *proton_sources.PART_NAMES, combined.name])), encoding='ascii')
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    args = parser.parse_args()
    print(json.dumps(collect(args.root, args.inputs, args.output, args.tag)))


if __name__ == '__main__':
    main()
