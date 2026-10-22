#!/usr/bin/env python3
"""Verify installed Proton runtime identity and redistribution materials."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import stat

REDISTRIBUTION_FILES = (
    'BUN-THIRD-PARTY-NOTICES.txt.gz', 'BUN-NOTICE-PROVENANCE.json.gz',
    'BUN-LINUX-ATTRIBUTION.md', 'OPENPGP-PREBUNDLED-NOTICES.txt',
    'LICENSE.crypto-GPL-3.0', 'LICENSE.pmcrypto-MIT', 'LICENSE.srp-MIT',
    'LICENSE.boltffi-MIT', 'CRYPTO-PROVENANCE.md', 'SOURCES.json', 'REBUILD.md',
    'OPENPGP-REBUILD.patch',
)


# tools/proton/provision_runtime.py WINDOWS_REDISTRIBUTION_FILES: the Windows helper ships
# these beside it under proton\redistribution\ (no separate documentation copy).
WINDOWS_REDISTRIBUTION_FILES = (
    'BUN-THIRD-PARTY-NOTICES.txt.gz', 'BUN-NOTICE-PROVENANCE.json.gz',
    'BUN-LINUX-ATTRIBUTION.md', 'BUN-WINDOWS-ATTRIBUTION.md', 'BUN-WINDOWS-SCOPE.json',
    'LICENSE.icu4c-78.3', 'OPENPGP-PREBUNDLED-NOTICES.txt', 'LICENSE.crypto-GPL-3.0',
    'LICENSE.pmcrypto-MIT', 'LICENSE.srp-MIT', 'LICENSE.boltffi-MIT', 'CRYPTO-PROVENANCE.md',
    'SOURCES-WINDOWS.json', 'REBUILD.md', 'REBUILD-WINDOWS.md', 'OPENPGP-REBUILD.patch',
)
WINDOWS_HELPER = 'putgd-proton-runtime.exe'
# The fields the Windows catalog shares with the Linux one: the same six packets and overlay.
SHARED_CATALOG_FIELDS = ('artifacts', 'delivery', 'publication', 'distributionRequirement')


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def audit_proton(root: Path, *, required=False, expected_manifest: dict | None = None) -> dict | None:
    root = root.resolve()
    private = root / 'usr/libexec/putgd/proton'
    shared_manifest = root / 'usr/share/putgd/proton-runtime-manifest.json'
    docs = root / 'usr/share/doc/putgd/proton'
    if not required and not any(path.exists() for path in (private, shared_manifest, docs)):
        return None

    def file(path: Path) -> Path:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError('Required Proton file missing or empty: ' + str(path.relative_to(root)))
        if not path.resolve().is_relative_to(root):
            raise ValueError('Proton file escapes package: ' + str(path.relative_to(root)))
        return path

    manifest_path = file(private / 'manifest.json')
    manifest = json.loads(manifest_path.read_text())
    if manifest != json.loads(file(shared_manifest).read_text()):
        raise ValueError('Proton runtime manifest copies disagree')
    if expected_manifest is not None and manifest != expected_manifest:
        raise ValueError('Proton package differs from expected runtime manifest')
    if manifest.get('schemaVersion') != 1 or manifest.get('protocolVersion') != 1:
        raise ValueError('Unsupported Proton runtime manifest/protocol')
    helper = file(private / 'putgd-proton-runtime')
    if stat.S_IMODE(helper.stat().st_mode) != 0o755:
        raise ValueError('Proton helper executable mode must be 0755')

    def digest(path: Path, expected: str) -> None:
        if sha256(file(path)) != expected:
            raise ValueError('Proton digest mismatch: ' + str(path.relative_to(root)))

    digest(helper, manifest['sha256'])
    digest(private / 'THIRD-PARTY-NOTICES.txt', manifest['dependencyNoticesSha256'])
    digest(docs / 'THIRD-PARTY-NOTICES.txt', manifest['dependencyNoticesSha256'])
    records = manifest['redistributionFilesSha256']
    if set(records) != set(REDISTRIBUTION_FILES):
        raise ValueError('Incomplete Proton redistribution manifest')
    for name, expected in records.items():
        digest(private / 'redistribution' / name, expected)
        digest(docs / name, expected)
    catalog = json.loads((private / 'redistribution/SOURCES.json').read_text())
    pins = catalog.get('sourcePins', {})
    if (catalog.get('materialsReady') is not True or catalog.get('helperSha256') != manifest['sha256']
            or pins.get('sdk') != manifest['sourceReference']
            or pins.get('crypto') != manifest['cryptoSourceReference']
            or not catalog.get('artifacts')):
        raise ValueError('Proton source catalog does not match the selected runtime/materials')
    return {'helper': str(helper.relative_to(root)), 'runtimeSha256': manifest['sha256'],
            'runtimeManifestSha256': sha256(manifest_path), 'manifest': manifest,
            'redistributionFiles': len(records),
            'sourceCatalogSha256': records['SOURCES.json'],
            'desktopPrerequisites': ['default web browser', 'unlocked Secret Service-compatible keychain'],
            'scope': 'package runtime and source/notices inventory; not live desktop acceptance or source publication'}


def windows_catalog_mismatches(catalog: dict, linux_catalog: dict) -> list[str]:
    """Where SOURCES-WINDOWS.json no longer describes the Linux catalog's packets and overlay."""
    mismatches = [field for field in SHARED_CATALOG_FIELDS if catalog.get(field) != linux_catalog.get(field)]
    pins = catalog.get('sourcePins', {})
    mismatches += ['sourcePins.' + name for name, value in linux_catalog.get('sourcePins', {}).items() if pins.get(name) != value]
    overlay, linux_overlay = catalog.get('applicationSourceOverlay', {}), linux_catalog.get('applicationSourceOverlay', {})
    for field in ('files', 'baseSourceArtifact', 'baseSourceArtifactSha256', 'repository', 'sourceCommit'):
        if overlay.get(field) != linux_overlay.get(field):
            mismatches.append('applicationSourceOverlay.' + field)
    return mismatches


def audit_proton_windows(root: Path, *, required=False, expected_manifest: dict | None = None,
                         linux_catalog: dict | None = None) -> dict | None:
    """The Windows layout: proton\\ beside putgd.exe in the zip folder or the installed folder."""
    root = root.resolve()
    private = root / 'proton'
    if not required and not private.exists():
        return None

    def file(path: Path) -> Path:
        if not path.is_file() or path.is_symlink() or not path.stat().st_size:
            raise ValueError('Required Proton file missing or empty: ' + path.relative_to(root).as_posix())
        return path

    manifest_path = file(private / 'manifest.json')
    manifest = json.loads(manifest_path.read_text())
    if expected_manifest is not None and manifest != expected_manifest:
        raise ValueError('Proton package differs from expected runtime manifest')
    if (manifest.get('schemaVersion') != 1 or manifest.get('protocolVersion') != 1
            or manifest.get('platform') != 'windows-x64' or manifest.get('artifact') != WINDOWS_HELPER):
        raise ValueError('Not a protocol-1 windows-x64 Proton runtime manifest')
    if manifest.get('redistributionComplete') is not True:
        raise ValueError('Proton runtime is not redistributable: ' + manifest.get('redistributionNote', 'incomplete'))

    def digest(path: Path, expected: str) -> None:
        if sha256(file(path)) != expected:
            raise ValueError('Proton digest mismatch: ' + path.relative_to(root).as_posix())

    helper = private / WINDOWS_HELPER
    digest(helper, manifest['sha256'])
    digest(private / 'THIRD-PARTY-NOTICES.txt', manifest['dependencyNoticesSha256'])
    records = manifest['redistributionFilesSha256']
    if set(records) != set(WINDOWS_REDISTRIBUTION_FILES):
        raise ValueError('Incomplete Proton redistribution manifest')
    for name, expected in records.items():
        digest(private / 'redistribution' / name, expected)
    expected_paths = {WINDOWS_HELPER, 'manifest.json', 'THIRD-PARTY-NOTICES.txt',
                      *('redistribution/' + name for name in WINDOWS_REDISTRIBUTION_FILES)}
    present = {path.relative_to(private).as_posix() for path in private.rglob('*') if not path.is_dir()}
    if present != expected_paths:
        raise ValueError('Unexpected Proton files: ' + ', '.join(sorted(present ^ expected_paths)))
    catalog = json.loads((private / 'redistribution/SOURCES-WINDOWS.json').read_text())
    scope = json.loads((private / 'redistribution/BUN-WINDOWS-SCOPE.json').read_text())
    runtime = manifest.get('bunTargetRuntime', {})
    pins = catalog.get('sourcePins', {})
    if (catalog.get('platform') != 'windows-x64' or catalog.get('materialsReady') is not True
            or catalog.get('helperSha256') != manifest['sha256'] or catalog.get('bunTargetRuntime') != runtime
            or scope.get('bunExecutable', {}).get('executableSha256') != runtime.get('executableSha256')
            or pins.get('sdk') != manifest['sourceReference']
            or pins.get('crypto') != manifest['cryptoSourceReference']
            or not catalog.get('artifacts') or not catalog.get('applicationSourceOverlay', {}).get('files')):
        raise ValueError('Proton source catalog does not match the selected Windows runtime/materials')
    if linux_catalog is not None and (mismatches := windows_catalog_mismatches(catalog, linux_catalog)):
        raise ValueError('Windows source catalog drifted from SOURCES.json: ' + ', '.join(mismatches))
    return {'helper': helper.relative_to(root).as_posix(), 'runtimeSha256': manifest['sha256'],
            'runtimeManifestSha256': sha256(manifest_path), 'bunExecutableSha256': runtime['executableSha256'],
            'redistributionFiles': len(records), 'sourceCatalogSha256': records['SOURCES-WINDOWS.json'],
            'desktopPrerequisites': ['default web browser', 'Windows Credential Manager in the signed-in session'],
            'scope': 'package runtime and source/notices inventory; not live desktop acceptance or source publication'}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, help='Expected provisioned runtime manifest')
    parser.add_argument('--windows', action='store_true', help='Audit a Windows folder (proton\\ beside putgd.exe)')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    expected = json.loads(args.manifest.read_text()) if args.manifest else None
    if args.windows:
        linux_catalog = json.loads((Path(__file__).resolve().parents[2] / 'tools/proton/notices/SOURCES.json').read_text())
        result = audit_proton_windows(args.root, required=True, expected_manifest=expected, linux_catalog=linux_catalog)
    else:
        result = audit_proton(args.root, required=True, expected_manifest=expected)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
