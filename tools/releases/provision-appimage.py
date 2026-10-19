#!/usr/bin/env python3
"""Provision pinned AppImage dependencies with their upstream builds (CMake, FFmpeg configure)."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def run(*argv):
    subprocess.run(list(argv), check=True)


def checkout(item, destination, license_only=False):
    if destination.exists():
        raise ValueError('Dependency source destination must be new: ' + str(destination))
    run('git', 'init', str(destination))
    run('git', '-C', str(destination), 'remote', 'add', 'origin', item['repository'])
    run('git', '-C', str(destination), 'fetch', '--depth', '1', '--filter=blob:none', 'origin', item['commit'])
    if license_only:
        run('git', '-C', str(destination), 'sparse-checkout', 'init', '--no-cone')
        run('git', '-C', str(destination), 'sparse-checkout', 'set', '--no-cone',
            '/LICENSE*', '/LICENSES/', '**/LICENSE*', '**/COPYING*',
            '**/NOTICE*', '**/COPYRIGHT*', '**/qt_attribution.json')
    run('git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD')
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != item['commit']:
        raise ValueError('Dependency checkout differs from locked commit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-root', required=True, type=Path)
    parser.add_argument('--skip-build', action='store_true', help='Verify source/license checkouts after exact installed-prefix cache restore')
    args = parser.parse_args()
    root = Path.cwd()
    lock = json.loads((root / 'packaging/appimage/tools.lock.json').read_text())['buildInputs']
    prefix = root / 'build/appimage-deps'
    sources = root / 'build/dependencies/appimage'
    sources.mkdir(parents=True, exist_ok=True)
    records = []
    for key, name in (('extraCmakeModules', 'ecm'), ('qtKeychain', 'qtkeychain'), ('layerShellQt', 'layershell')):
        item = lock[key]
        source = sources / name
        checkout(item, source)
        build = root / 'build' / ('appimage-' + name)
        options = ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
                   '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_PREFIX_PATH=' + str(prefix) + ';' + str(args.qt_root),
                   '-DBUILD_TESTING=OFF']
        if name == 'qtkeychain':
            options += ['-DBUILD_WITH_QT6=ON', '-DBUILD_TEST_APPLICATION=OFF', '-DBUILD_TRANSLATIONS=OFF']
        if name == 'layershell':
            options += ['-DKDE_INSTALL_QTPLUGINDIR=' + str(prefix / 'lib/qt6/plugins')]
        if not args.skip_build:
            run('cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja', *options)
            run('cmake', '--build', str(build), '--parallel', '2')
            run('cmake', '--install', str(build))
        records.append({'name': name, 'commit': item['commit'], 'source': str(source)})
    # putgd-record's export libraries: the pinned upstream FFmpeg, configured
    # LGPL-only with the components packaging/appimage/media.json lists.
    item = lock['ffmpeg']
    source = sources / 'ffmpeg'
    checkout(item, source)
    configure = json.loads((root / 'packaging/appimage/media.json').read_text())['ffmpeg']['configure']
    if not args.skip_build:
        build = root / 'build/appimage-ffmpeg'
        build.mkdir(parents=True, exist_ok=False)
        subprocess.run([str(source / 'configure'), '--prefix=' + str(prefix), '--libdir=' + str(prefix / 'lib'), *configure],
                       check=True, cwd=build)
        run('make', '-C', str(build), '-j' + str(os.cpu_count() or 2))
        run('make', '-C', str(build), 'install')
    records.append({'name': 'ffmpeg', 'commit': item['commit'], 'source': str(source), 'configure': configure})
    for item in lock['qtLicenseSources']:
        name = item['repository'].rsplit('/', 1)[1].removesuffix('.git')
        source = sources / name
        checkout(item, source, license_only=True)
        records.append({'name': name, 'commit': item['commit'], 'source': str(source), 'licenseOnly': True})
    (root / 'build/appimage-dependencies.json').write_text(json.dumps(records, indent=2) + '\n')


if __name__ == '__main__':
    main()
