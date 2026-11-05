#!/usr/bin/env python3
"""Compile a byte-verified curated source export with the normal release preset."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tarfile
import time

from audit_native import sha256


def check_build(receipt_path, sdk_prefix, sdk_source, output, *, bundle_proton=False,
                proton_source=None, proton_crypto_source=None, proton_offline=False):
    receipt = json.loads(receipt_path.read_text())
    if receipt['format'] != 'source.tar.gz':
        raise ValueError('Expected a curated source archive receipt')
    artifact = receipt['artifact']
    name = artifact['file']
    if Path(name).name != name:
        raise ValueError('Unsafe source archive filename')
    archive = receipt_path.parent / name
    if archive.is_symlink() or sha256(archive) != artifact['sha256'] or archive.stat().st_size != artifact['bytes']:
        raise ValueError('Source archive bytes differ from receipt')
    archive_root = receipt['archiveRoot']
    if Path(archive_root).name != archive_root or archive_root in ('', '.', '..'):
        raise ValueError('Unsafe source archive root')
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    parameters = {'schemaVersion': 1, 'archive': artifact, 'identity': receipt['identity'],
                  'sdkPrefix': str(sdk_prefix), 'sdkSource': str(sdk_source),
                  'bundleProton': bundle_proton,
                  'protonSource': str(proton_source) if proton_source else None,
                  'protonCryptoSource': str(proton_crypto_source) if proton_crypto_source else None,
                  'protonOffline': proton_offline,
                  'scope': 'Actual source-export production compilation; no desktop or memory acceptance'}
    (output / 'parameters.json').write_text(json.dumps(parameters, indent=2) + '\n')
    result = {'schemaVersion': 1, 'passed': False, 'archive': artifact, 'commands': []}
    started = time.monotonic()
    try:
        with tarfile.open(archive, 'r:gz') as stream:
            stream.extractall(output, filter='data')
        source = output / archive_root
        if (source / '.git').exists():
            raise ValueError('Source archive must not contain Git metadata')
        identity = receipt['identity']
        commands = [
            ['cmake', '--preset', 'release', '-DBUILD_TESTING=OFF', '-DPUTGD_LAB_FIXTURE=OFF',
             '-DPUTGD_PHYSICAL_BENCHMARKS=OFF', '-DCMAKE_PREFIX_PATH=' + str(sdk_prefix),
             '-DPUTGD_AWS_SOURCE_DIR=' + str(sdk_source),
             '-DPUTGD_AWS_RUNTIME_DIR=' + str(sdk_prefix / 'lib'),
             '-DPUTGD_SOURCE_COMMIT=' + identity['gitCommit'],
             '-DPUTGD_SOURCE_TREE=' + identity['gitTree'],
             '-DPUTGD_SOURCE_COMMIT_COUNT=' + str(identity['commitCount']),
             '-DPUTGD_SOURCE_COMMIT_TIME=' + str(receipt['timestamp'])],
            ['cmake', '--build', '--preset', 'release'],
        ]
        if bundle_proton:
            provision = [sys.executable, 'tools/proton/provision_runtime.py']
            if proton_source:
                provision += ['--source', str(proton_source)]
            if proton_crypto_source:
                provision += ['--crypto-source', str(proton_crypto_source)]
            if proton_offline:
                provision.append('--offline')
            commands[0].append('-DPUTGD_BUNDLE_PROTON=ON')
            commands.insert(0, provision)
        with (output / 'build.log').open('x') as log:
            for command in commands:
                result['commands'].append(command)
                subprocess.run(command, cwd=source, stdout=log, stderr=subprocess.STDOUT, check=True)
        build = source / 'build/release'
        manifest = json.loads((build / 'build-identity/build-info.json').read_text())
        if (manifest['sourceProvenance'] != 'supplied' or manifest['gitCommit'] != identity['gitCommit']
                or manifest['gitTree'] != identity['gitTree'] or manifest['commitCount'] != identity['commitCount']
                or manifest['version'] != receipt['localBuild']['version']):
            raise ValueError('Extracted source compiled identity differs from supplied provenance')
        verified = []
        for name in ('putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker', 'putgd-b2-worker', 'putgd-kde-region', 'putgd-record'):
            program = build / name
            if name == 'putgd-kde-region' and not program.exists():
                continue  # Minimum-Qt Cinnamon builder does not declare this optional target.
            if name == 'putgd-record' and not program.exists():
                continue  # Built only with GStreamer/FFmpeg development libraries.
            observed = json.loads(subprocess.check_output([str(program), '--version-json'], text=True))
            if observed != manifest:
                raise ValueError('Archive-built helper identity mismatch: ' + name)
            verified.append(name)
        if bundle_proton:
            runtime = source / 'build/dependencies/proton-runtime'
            runtime_manifest = json.loads((runtime / 'manifest.json').read_text())
            helper_hash = sha256(runtime / 'putgd-proton-runtime')
            bundled_hash = sha256(build / 'proton/putgd-proton-runtime')
            if helper_hash != runtime_manifest['sha256'] or bundled_hash != helper_hash:
                raise ValueError('Extracted-source Proton helper differs from manifest or bundled bytes')
            result['rebuiltProton'] = {'sha256': helper_hash, 'bundledSha256': bundled_hash,
                                      'manifestSha256': sha256(runtime / 'manifest.json')}
        result.update(passed=True, compiledBuild=manifest, identityMatchedPrograms=verified)
    finally:
        result['wallSeconds'] = time.monotonic() - started
        (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--sdk-prefix', type=Path, required=True)
    parser.add_argument('--sdk-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New private output directory')
    parser.add_argument('--bundle-proton', action='store_true', help='Rebuild and bundle the Linux x86-64 Proton helper from exported inputs')
    parser.add_argument('--proton-source', type=Path, help='Existing pinned Proton SDK checkout (read-only)')
    parser.add_argument('--proton-crypto-source', type=Path, help='Existing approved crypto checkout (read-only)')
    parser.add_argument('--proton-offline', action='store_true', help='Require cached locked npm inputs and both existing Proton checkouts')
    args = parser.parse_args()
    if not args.bundle_proton and (args.proton_source or args.proton_crypto_source or args.proton_offline):
        parser.error('Proton provisioning options require --bundle-proton')
    result = check_build(args.receipt.resolve(), args.sdk_prefix.resolve(), args.sdk_source.resolve(), args.output.resolve(),
                         bundle_proton=args.bundle_proton,
                         proton_source=args.proton_source.resolve() if args.proton_source else None,
                         proton_crypto_source=args.proton_crypto_source.resolve() if args.proton_crypto_source else None,
                         proton_offline=args.proton_offline)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
