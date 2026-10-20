#!/usr/bin/env python3
"""Stage allowlisted packaging evidence at one flat Actions artifact root."""
import argparse
from pathlib import Path
import shutil


def stage(build, output, kind):
    output.mkdir(parents=True, exist_ok=False)
    selected = [build / 'release-tag.json', build / 'ci-sdk-environment.json']
    if kind == 'native':
        packages = build / 'release-packages'
        selected += [packages / name for name in ('native-audit.json', 'build-info.json', 'cpack.log')]
        selected += list(packages.glob('*.deb')) + list(packages.glob('*.rpm'))
        selected += [build / 'source-export/source-archive.json']
        selected += list((build / 'source-export').glob('*-source.tar.gz'))
    else:
        packages = build / 'appimage-output'
        selected += [packages / 'appimage-build.json', build / 'appimage-dependencies.json',
                     build / 'release/build-identity/build-info.json']
        selected += list(packages.glob('*.AppImage'))
        selected += [packages / 'appimage-media-source.json'] + list(packages.glob('*-appimage-media-source.tar.gz'))
    # Split debug files by build ID and their manifest (#124, debug_symbols.py).
    selected += sorted((build / 'debug-symbols').glob('debug-symbols-*'))
    # Partial failed-build evidence may be uploaded; the collector still requires
    # all successful receipts and exact bytes before it can stage a release.
    for source in selected:
        if source.is_file() and not source.is_symlink():
            shutil.copy2(source, output / source.name)
    if kind == 'native':
        for source_name, target_name in [('results.json', 'source-build-results.json'), ('parameters.json', 'source-build-parameters.json'), ('build.log', 'source-build.log')]:
            source = build / 'source-build' / source_name
            if source.is_file() and not source.is_symlink():
                shutil.copy2(source, output / target_name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=Path('build'))
    parser.add_argument('--output', type=Path, default=Path('build/release-artifact'))
    parser.add_argument('--kind', choices=('native', 'appimage'), required=True)
    args = parser.parse_args()
    stage(args.build, args.output, args.kind)
