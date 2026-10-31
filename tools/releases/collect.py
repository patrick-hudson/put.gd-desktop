#!/usr/bin/env python3
"""Collect only byte-verified assets matching one exact tagged source identity."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from audit_native import PROGRAMS, sha256
from verify_tag import verify
import proton_sources
import appimage_complete_sources
import appimage_update
import debug_symbols
import support_assets
from release_bundle import create_gnome_zip, create_release_bundle

# Package matrix names in .github/workflows/package.yml, each built on its own distro.
NATIVE_TARGETS = (('ubuntu24.04', 'DEB', '.deb'), ('ubuntu26.04', 'DEB', '.deb'),
                  ('debian13', 'DEB', '.deb'), ('fedora44', 'RPM', '.rpm'))


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


def verified_sboms(directory, release, packages):
    """Each package's CycloneDX SBOM (sbom.py, #420), naming exactly that package's bytes."""
    receipts = json.loads((directory / 'sbom-receipts.json').read_text())['sboms']
    if set(receipts) != set(packages):
        raise ValueError('SBOMs differ from the release packages')
    selected = []
    for label, package in packages.items():
        receipt = receipts[label]
        name = 'putgd-' + release['version'] + '-' + label + '.cdx.json'
        path = asset_path(directory, receipt['file'], '.cdx.json')
        if receipt['file'] != name or sha256(path) != receipt['sha256'] or receipt['packageSha256'] != sha256(package):
            raise ValueError('SBOM differs from its receipt or package: ' + label)
        if json.loads(path.read_text())['metadata']['component'].get('hashes') != [{'alg': 'SHA-256', 'content': receipt['packageSha256']}]:
            raise ValueError('SBOM names another package: ' + label)
        selected.append((path, name))
    return selected, receipts


def verified_source(directory, release, *, require_build=True):
    receipt = json.loads((directory / 'source-archive.json').read_text())
    if receipt.get('format') != 'source.tar.gz' or not identity_matches(receipt['identity'], release):
        raise ValueError('Source archive identity differs from release')
    artifact = receipt['artifact']
    archive = asset_path(directory, artifact['file'], '-source.tar.gz')
    if sha256(archive) != artifact['sha256'] or archive.stat().st_size != artifact['bytes']:
        raise ValueError('Source archive bytes differ from compiled receipt')
    build_path = directory / 'source-build-results.json'
    if not require_build and not build_path.exists():
        return archive, receipt, None
    build = json.loads(build_path.read_text())
    version_suffix = f"dev+g{release['gitCommit'][:12]}"
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


def verified_zsync(directory, receipt, image, name, release, repository):
    """The image's update information and .zsync must be exactly this release's."""
    update = receipt.get('update') or {}
    information = appimage_update.update_information(release['tag'])
    if update.get('information') != information or appimage_update.read_update_information(image) != information:
        raise ValueError('AppImage update information differs from its release channel')
    zsync = update.get('zsync') or {}
    path = asset_path(directory, zsync.get('file', ''), '.AppImage.zsync')
    if sha256(path) != zsync.get('sha256') or path.stat().st_size != zsync.get('bytes'):
        raise ValueError('AppImage .zsync bytes differ from audited receipt')
    appimage_update.asset_url(repository, release['tag'], name)
    url = appimage_update.mirror_url(release['tag'], name)
    if zsync.get('url') != url:
        raise ValueError('AppImage .zsync names another URL than the release mirror')
    appimage_update.verify_zsync(path, image, name, url)
    return path


def collect(root, inputs, output, tag, repository=appimage_update.REPOSITORY, *, installers_only=False):
    release = verify(root, tag)
    timestamp = int(subprocess.check_output(['git', '-C', str(root), 'show', '-s', '--format=%ct',
                                             release['gitCommit']], text=True).strip())
    # Resolve everything before making an output directory or copying assets.
    selected = []
    receipts = {}
    packages = {}
    for distro, kind, suffix in NATIVE_TARGETS:
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
        packages[distro] = path
        receipts[distro] = receipt
    directory = inputs / 'release-appimage'
    receipt = json.loads((directory / 'appimage-build.json').read_text())
    if receipt['format'] != 'AppImage' or receipt['architecture'] != 'x86_64' or not identity_matches(receipt['build'], release):
        raise ValueError('AppImage compiled identity/format differs from release')
    image = asset_path(directory, receipt['artifact']['file'], '.AppImage')
    if sha256(image) != receipt['artifact']['sha256'] or image.stat().st_size != receipt['artifact']['bytes']:
        raise ValueError('AppImage bytes differ from audited receipt')
    name = 'putgd-' + release['version'] + '-x86_64.AppImage'
    selected.append((image, name))
    # AppImageUpdate, Gear Lever and AppImageLauncher read the embedded update
    # information and this .zsync (#293).
    zsync = verified_zsync(directory, receipt, image, name, release, repository)
    selected.append((zsync, appimage_update.zsync_name(name)))
    receipts['appimage'] = receipt
    sboms, receipts['sbom'] = verified_sboms(inputs / 'release-sbom', release, {**packages, 'appimage': image})
    selected += sboms
    # The image is a GPL combined work: its codec sources ship beside it.
    media_receipt = json.loads((directory / appimage_complete_sources.RECEIPT_NAME).read_text())
    media_source = appimage_complete_sources.verify(directory, media_receipt, receipt)
    selected.append((media_source, appimage_complete_sources.archive_name(release['version'])))
    receipts['appimageCompleteSource'] = media_receipt
    source, source_receipt, source_build = verified_source(inputs / 'release-native-ubuntu24.04', release,
                                                         require_build=not installers_only or release['channel'] == 'stable')
    selected.append((source, 'putgd-' + release['version'] + '-source.tar.gz'))
    receipts['source'] = source_receipt
    receipts['sourceBuild'] = source_build
    source_directory = inputs / 'proton-sources'
    current_catalog = root / 'tools/proton/notices/SOURCES.json'
    catalog_path = current_catalog if installers_only else source_directory / proton_sources.CATALOG_NAME
    if (catalog_path.is_symlink() or not catalog_path.is_file()
            or catalog_path.read_bytes() != current_catalog.read_bytes()):
        raise ValueError('Proton source catalog differs from current release source')
    catalog = proton_sources.catalog_value(catalog_path)
    if not installers_only:
        proton_sources.verify(source_directory, catalog)
    catalog_sha = sha256(catalog_path)
    rebuilt = (source_build or {}).get('rebuiltProton', {})
    if (source_build is not None and (rebuilt.get('sha256') != catalog['helperSha256']
            or rebuilt.get('bundledSha256') != catalog['helperSha256'])):
        raise ValueError('Source build has no matching rebuilt Proton helper')
    for kind in (*(distro for distro, _, _ in NATIVE_TARGETS), 'appimage'):
        inventory = (receipts[kind].get('postDeploymentAudit', {}).get('proton', {}) if kind == 'appimage'
                     else receipts[kind].get('proton', {}))
        if (inventory.get('runtimeSha256') != catalog['helperSha256']
                or inventory.get('sourceCatalogSha256') != catalog_sha):
            raise ValueError('Proton package runtime/source catalog differs from delivered source: ' + kind)
    if installers_only:
        selected.append((catalog_path, proton_sources.CATALOG_NAME))
    else:
        for name in (*proton_sources.PART_NAMES, proton_sources.CATALOG_NAME):
            selected.append((source_directory / name, name))
    receipts['protonSource'] = {'helperSha256': catalog['helperSha256'],
        'sourceCatalogSha256': catalog_sha, 'delivery': catalog['delivery']}
    if installers_only:
        output.mkdir(parents=True, exist_ok=False)
        sources = output / 'sources'
        sources.mkdir()
        for path, name in selected:
            shutil.copy2(path, (sources if name.endswith('-source.tar.gz') else output) / name)
        create_gnome_zip(root, sources / ('putgd-gnome50-' + release['version'] + '.zip'), timestamp)
        delivery = support_assets.describe(sources, release, repository)
        support_assets.validate(delivery, release, repository)
        (output / 'source-delivery.json').write_text(json.dumps(delivery, indent=2) + '\n')
        (output / 'release-builds.json').write_text(json.dumps({
            'schemaVersion': 2, 'assetLayout': support_assets.LAYOUT, 'release': release,
            'receipts': receipts, 'sources': delivery}, indent=2) + '\n')
        names = {p.name for p in output.iterdir() if p.is_file()}
        if names | {'SHA256SUMS'} != support_assets.inventory(release['version']):
            raise ValueError('Installer release inventory differs from expected assets')
        (output / 'SHA256SUMS').write_text(''.join(sha256(output / name) + '  ' + name + '\n'
                                                 for name in sorted(names)), encoding='ascii')
        return release
    # Every build's split debug files, checked against its packaged binaries'
    # build IDs when it was staged (#124).
    debug = debug_symbols.prepare({label: inputs / debug_symbols._artifact(label) for label in debug_symbols.LABELS},
                                  lambda build: identity_matches(build, release))
    output.mkdir(parents=True, exist_ok=False)
    for path, name in selected:
        shutil.copy2(path, output / name)
    receipts['debugSymbols'] = debug_symbols.write(debug, output, release['version'], timestamp)
    debug_name = receipts['debugSymbols']['file']
    # GNOME integration is a host extension, including for AppImage users.
    extension = output / ('putgd-gnome50-' + release['version'] + '.zip')
    create_gnome_zip(root, extension, timestamp)
    (output / 'release-builds.json').write_text(json.dumps({'schemaVersion': 1, 'release': release, 'receipts': receipts}, indent=2) + '\n')
    member_names = [name for _, name in selected if name not in proton_sources.PART_NAMES] + [extension.name, 'release-builds.json']
    combined = output / ('putgd-' + release['version'] + '-release.zip')
    create_release_bundle(output, combined, release['version'], timestamp, member_names)
    (output / 'SHA256SUMS').write_text(''.join(sha256(output / name) + '  ' + name + '\n'
        for name in sorted([*member_names, *proton_sources.PART_NAMES, combined.name, debug_name])), encoding='ascii')
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    parser.add_argument('--installers-only', action='store_true',
                        help='Publish installers/support metadata; link sources and retain debug files in CI')
    parser.add_argument('--repository', default=os.environ.get('GITHUB_REPOSITORY') or appimage_update.REPOSITORY,
                        help='GitHub OWNER/NAME whose release serves the assets')
    args = parser.parse_args()
    print(json.dumps(collect(args.root, args.inputs, args.output, args.tag, args.repository,
                             installers_only=args.installers_only)))


if __name__ == '__main__':
    main()
