#!/usr/bin/env python3
"""Build the Windows portable zip and the per-user MSI from a cross build (#404).

    python3 src/windows/packaging/package.py --build build/windows --output build/windows-packages

Reads the build's manifest (build-identity/build-info.json) and its Windows
version mapping (windows-version.json, written at configure time), checks that
both executables carry that identity, and writes:

    putgd-VERSION-windows-x86_64.zip   put.gd/ with the executables, manifest and licence
    putgd-VERSION-windows-x86_64.msi   wixl build of putgd.wxs (unless --no-msi)
    windows-packages.json              the identity and each file's size and SHA-256
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
EXECUTABLES = ('putgd.exe', 'putgd-cli.exe')
# The calendar grammar of GenerateBuildInfo.cmake, with its -nightly and -dev+g… forms.
VERSION = re.compile(r'^[2-9][0-9]\.[1-9][0-9]{2,3}\.[1-9][0-9]{0,8}(-nightly|-dev\+g[0-9a-f]{12}(\.dirty)?)?$')
# Fixed entry times keep the zip's bytes a function of its contents.
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


class PackageError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def identity(build):
    try:
        manifest = json.loads((build / 'build-identity/build-info.json').read_text())
        mapping = json.loads((build / 'windows-version.json').read_text())
    except (OSError, ValueError) as error:
        raise PackageError(f'{build} is not a configured Windows build: {error}') from error
    version = manifest.get('version', '')
    if not VERSION.match(version) or mapping.get('version') != version:
        raise PackageError(f'The build manifest ({version!r}) and windows-version.json ({mapping.get("version")!r}) disagree')
    if not re.fullmatch(r'[0-9a-f]{40}', manifest.get('gitCommit', '')):
        raise PackageError('The build manifest has no full commit')
    # Each executable embeds the manifest's version and commit (protocol::buildIdentity)
    # and the version resource's ProductVersion; a stale executable from another
    # commit or version fails here instead of being packaged under this label.
    product_version = (version + '\0').encode('utf-16-le')
    for name in EXECUTABLES:
        path = build / name
        if not path.is_file():
            raise PackageError(f'{path} is missing; build both executables first')
        data = path.read_bytes()
        if data[:2] != b'MZ':
            raise PackageError(f'{path} is not a Windows executable')
        for needle, what in ((version.encode(), 'version'), (manifest['gitCommit'].encode(), 'commit'),
                             (product_version, 'ProductVersion resource')):
            if needle not in data:
                raise PackageError(f'{name} does not carry this build\'s {what}; rebuild both executables')
    return manifest, mapping


def stage(build, directory):
    for name in EXECUTABLES:
        shutil.copyfile(build / name, directory / name)
    shutil.copyfile(build / 'build-identity/build-info.json', directory / 'build-info.json')
    # Windows readers expect CRLF in a .txt file.
    licence = (ROOT / 'LICENSE').read_text().replace('\r\n', '\n').replace('\n', '\r\n')
    (directory / 'LICENSE.txt').write_bytes(licence.encode())
    return sorted(path.name for path in directory.iterdir())


def write_zip(directory, names, destination):
    with zipfile.ZipFile(destination, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in names:
            entry = zipfile.ZipInfo('put.gd/' + name, ZIP_TIME)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            archive.writestr(entry, (directory / name).read_bytes())


def write_msi(directory, manifest, mapping, destination, wixl):
    command = [wixl, '-a', 'x64', '-D', 'Version=' + manifest['version'], '-D', 'MsiVersion=' + mapping['msiVersion'],
               '-D', f'SourceDir={directory}', '-D', f'IconFile={ROOT / "src/windows/resident/app.ico"}',
               '-o', str(destination), str(HERE / 'putgd.wxs')]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # wixl reports some schema problems on stderr and still exits 0, so the output
    # must exist and carry no GLib criticals.
    if result.returncode != 0 or not destination.is_file() or 'CRITICAL' in result.stdout:
        raise PackageError('wixl failed:\n' + result.stdout)


def package(build, output, msi=True, wixl='wixl'):
    manifest, mapping = identity(build)
    output.mkdir(parents=True, exist_ok=True)
    stem = f'putgd-{manifest["version"]}-windows-x86_64'
    assets = []
    with tempfile.TemporaryDirectory(prefix='pgwin-') as temporary:
        directory = Path(temporary)
        names = stage(build, directory)
        targets = [(output / (stem + '.zip'), lambda path: write_zip(directory, names, path), 'windows-zip')]
        if msi:
            targets.append((output / (stem + '.msi'), lambda path: write_msi(directory, manifest, mapping, path, wixl), 'windows-msi'))
        for path, write, target in targets:
            partial = path.with_name(path.name + '.partial')
            partial.unlink(missing_ok=True)
            write(partial)
            partial.replace(path)
            assets.append({'target': target, 'name': path.name, 'size': path.stat().st_size, 'sha256': sha256(path)})
    receipt = {'schemaVersion': 1, 'version': manifest['version'], 'gitCommit': manifest['gitCommit'],
               'fileVersion': mapping['fileVersion'], 'msiVersion': mapping['msiVersion'],
               'executables': {name: sha256(build / name) for name in EXECUTABLES}, 'assets': assets}
    (output / 'windows-packages.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True, help='the configured and built src/windows build directory')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--no-msi', action='store_true', help='only the portable zip')
    parser.add_argument('--wixl', default='wixl')
    arguments = parser.parse_args()
    try:
        receipt = package(arguments.build, arguments.output, not arguments.no_msi, arguments.wixl)
    except PackageError as error:
        print(error, file=sys.stderr)
        return 1
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
