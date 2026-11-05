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
    'LICENSE.boltffi-MIT', 'LICENSE.jsmimeparser-MIT', 'LICENSE.MPL-2.0', 'LICENSE.icu4c-78.3', 'CRYPTO-PROVENANCE.md', 'SOURCES.json', 'REBUILD.md',
    'OPENPGP-REBUILD.patch',
)


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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, help='Expected provisioned runtime manifest')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    expected = json.loads(args.manifest.read_text()) if args.manifest else None
    result = audit_proton(args.root, required=True, expected_manifest=expected)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
