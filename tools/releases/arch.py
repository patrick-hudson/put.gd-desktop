#!/usr/bin/env python3
"""Package and audit the shared CMake install with Arch's makepkg (non-root)."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

from audit_native import audit_tree, sha256
from audit_file_manager_plugins import audit_plugins


def pkgver(version):
    # libalpm orders an alpha suffix before the same numeric final version.
    # No '-' is allowed inside pkgver; retain the compiled identity in the payload.
    return version.replace('-nightly', 'nightly').replace('-dev+', 'dev.').replace('~', '')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=Path('build/release'))
    p.add_argument('--output', type=Path, default=Path('build/release-packages'))
    a = p.parse_args()
    build, output = a.build.resolve(), a.output.resolve()
    identity = json.loads((build / 'build-identity/build-info.json').read_text())
    output.mkdir(parents=True, exist_ok=False)
    recipe = Path('packaging/arch/PKGBUILD').read_text().replace('pkgver=0.0.0', 'pkgver=' + pkgver(identity['version']))
    (output / 'PKGBUILD').write_text(recipe)
    env = dict(os.environ, PUTGD_BUILD=str(build), PKGEXT='.pkg.tar.zst')
    subprocess.run(['makepkg', '--nodeps', '--noconfirm'], cwd=output, env=env, check=True)
    packages = list(output.glob('*.pkg.tar.zst'))
    if len(packages) != 1:
        raise ValueError('Expected exactly one Arch package')
    package = packages[0]
    extracted = output / 'extracted'
    extracted.mkdir()
    subprocess.run(['bsdtar', '-xf', str(package), '-C', str(extracted)], check=True)
    for required in ('usr/bin/putgd-record', 'usr/bin/putgd-region', 'usr/bin/putgd-clipboard-keeper',
                     'usr/share/nemo/actions/putgd-upload.nemo_action',
                     'usr/share/kio/servicemenus/putgd-upload.desktop',
                     'usr/share/Thunar/sendto/putgd-upload.desktop'):
        if not (extracted / required).is_file():
            raise ValueError('Missing required Arch payload: ' + required)
    metadata = dict(line.split(' = ', 1) for line in (extracted / '.PKGINFO').read_text().splitlines()
                    if ' = ' in line and not line.startswith('depend = '))
    if metadata['pkgname'] != 'putgd' or metadata['pkgver'] != pkgver(identity['version']) + '-1':
        raise ValueError('Arch package identity differs from compiled manifest')
    dependencies = [line[9:] for line in (extracted / '.PKGINFO').read_text().splitlines() if line.startswith('depend = ')]
    required = {'tesseract', 'tesseract-data-eng', 'zxing-cpp', 'qt6-imageformats', 'qt6-wayland', 'qt6-base>=6.4.2', 'gst-plugins-bad', 'gst-plugin-pipewire', 'ffmpeg', 'layer-shell-qt'}
    if not required.issubset(dependencies):
        raise ValueError('Missing Arch runtime dependency: ' + ', '.join(sorted(required - set(dependencies))))
    result = audit_tree(extracted, identity, require_kde=True, require_proton=True, package_target='archlinux')
    result['fileManagerPlugins'] = audit_plugins(extracted, build, package_target='archlinux')
    result['package'] = {'format': 'PACMAN', 'name': 'putgd', 'version': metadata['pkgver'],
                         'architecture': 'x86_64', 'requires': dependencies, 'file': package.name,
                         'sha256': sha256(package), 'size': package.stat().st_size}
    (output / 'native-audit.json').write_text(json.dumps(result, indent=2) + '\n')
    shutil.copy2(build / 'build-identity/build-info.json', output / 'build-info.json')


if __name__ == '__main__':
    main()
