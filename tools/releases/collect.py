#!/usr/bin/env python3
"""Collect only byte-verified assets matching one exact tagged source identity."""
import argparse
import json
from pathlib import Path
import shutil
import zipfile

from audit_native import PROGRAMS, sha256
from verify_tag import verify


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
    allowed = required | {'putgd-kde-region'}
    if (build.get('passed') is not True or build.get('compiledBuild') != expected
            or not isinstance(programs, list) or not all(isinstance(name, str) for name in programs)
            or len(programs) != len(set(programs)) or not required.issubset(programs)
            or not set(programs).issubset(allowed)):
        raise ValueError('Source archive has no matching successful production build')
    return archive, receipt, build


def collect(root, inputs, output, tag):
    release = verify(root, tag)
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
    source, source_receipt, source_build = verified_source(inputs / 'release-native-ubuntu24.04', release)
    selected.append((source, 'putgd-' + release['version'] + '-source.tar.gz'))
    receipts['source'] = source_receipt
    receipts['sourceBuild'] = source_build
    output.mkdir(parents=True, exist_ok=False)
    for path, name in selected:
        shutil.copy2(path, output / name)
    # GNOME integration is a host extension, including for AppImage users.
    extension = output / ('putgd-gnome50-' + release['version'] + '.zip')
    with zipfile.ZipFile(extension, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name in ('extension.js', 'metadata.json'):
            archive.write(root / 'integrations/gnome' / name, name)
        archive.write(root / 'LICENSE', 'LICENSE')
    (output / 'release-builds.json').write_text(json.dumps({'schemaVersion': 1, 'release': release, 'receipts': receipts}, indent=2) + '\n')
    files = sorted(path for path in output.iterdir() if path.is_file())
    (output / 'SHA256SUMS').write_text(''.join(sha256(path) + '  ' + path.name + '\n' for path in files))
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
