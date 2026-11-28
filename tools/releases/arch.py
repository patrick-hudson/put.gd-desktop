#!/usr/bin/env python3
"""Package and audit the shared CMake install with Arch's makepkg (non-root)."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from audit_native import audit_tree, sha256
from audit_file_manager_plugins import audit_plugins


def pkgver(version):
    # libalpm orders an alpha suffix before the same numeric final version.
    # No '-' is allowed inside pkgver; retain the compiled identity in the payload.
    return version.replace('-nightly', 'nightly').replace('-dev+', 'dev.').replace('~', '')


def linked_libcrypto(elf_dependencies):
    """The pacman soname entry (libcrypto.so=4-64) for the OpenSSL the package's ELF files link."""
    needed = {name for item in elf_dependencies.values() for name in item.get('needed', []) if name.startswith('libcrypto.so.')}
    if len(needed) != 1:
        raise ValueError('Arch package must link exactly one libcrypto soname, found: ' + ', '.join(sorted(needed)))
    return 'libcrypto.so=' + needed.pop().split('.so.', 1)[1] + '-64'


def installer_libcrypto(installer):
    """The soname packaging/install.sh asks pacman about before it installs the package."""
    found = re.findall(r"^[ ]*arch_libcrypto='(libcrypto\.so=\d+-64)'$", installer, re.M)
    if len(found) != 1:
        raise ValueError("packaging/install.sh must set arch_libcrypto='libcrypto.so=N-64' exactly once")
    return found[0]


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
                     'usr/lib/qt6/plugins/kf6/kfileitemaction/putgd.so',
                     'usr/share/Thunar/sendto/putgd-upload.desktop',
                     'usr/share/nemo/actions/putgd-upload-encrypted.nemo_action',
                     'usr/share/Thunar/sendto/putgd-upload-encrypted.desktop'):
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
    # pacman resolves in listed order; tesseract first makes it also pick a default "tessdata" (Afrikaans).
    if dependencies.index('tesseract-data-eng') > dependencies.index('tesseract'):
        raise ValueError('tesseract-data-eng must be listed before tesseract')
    result = audit_tree(extracted, identity, require_kde=True, require_proton=True, package_target='archlinux')
    result['fileManagerPlugins'] = audit_plugins(extracted, build, package_target='archlinux')
    # The installer decides between this package and the AppImage by asking pacman for this soname, so it
    # must name what the binaries link. Arch moves OpenSSL on its own schedule: bump the installer then.
    libcrypto = linked_libcrypto(result['elfDependencies'])
    if libcrypto != installer_libcrypto(Path('packaging/install.sh').read_text()):
        raise ValueError(f"packaging/install.sh arch_libcrypto must be '{libcrypto}', the OpenSSL this package links")
    result['package'] = {'format': 'PACMAN', 'name': 'putgd', 'version': metadata['pkgver'],
                         'architecture': 'x86_64', 'requires': dependencies, 'libcrypto': libcrypto, 'file': package.name,
                         'sha256': sha256(package), 'size': package.stat().st_size}
    (output / 'native-audit.json').write_text(json.dumps(result, indent=2) + '\n')
    shutil.copy2(build / 'build-identity/build-info.json', output / 'build-info.json')


if __name__ == '__main__':
    main()
