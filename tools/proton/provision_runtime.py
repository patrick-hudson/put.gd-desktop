#!/usr/bin/env python3
"""Build the pinned bundled Proton runtime before CMake; never called by CMake."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

from audit_redistribution import inventory

ROOT = Path(__file__).resolve().parent
SOURCE_PATHS = ['cli/src/credentials/credentials.ts', 'cli/src/credentials/constants.ts',
                'cli/src/credentials/secretCredentialsStore.ts', 'cli/src/credentials/parseCredentials.ts',
                'cli/src/credentials/interface.ts', 'cli/src/config.ts', 'cli/src/clientUid.ts',
                'cli/src/api/httpClient.ts', 'cli/src/api/driveAccountAdapter.ts', 'cli/src/api/apiRequirements.ts',
                'incubating/account/js/src', 'config/js/patches/@protontech%2Fcrypto@2.0.0.patch', 'LICENSE.md']

def run(args, cwd, capture=False):
    return subprocess.run(args, cwd=cwd, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout

def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def patch_once(path, before, after):
    content = path.read_text()
    if content.count(before) != 1:
        raise SystemExit(f'Pinned source patch did not match exactly once: {path.name}')
    path.write_text(content.replace(before, after))

CRYPTO_SOURCE_PATHS = ['packages/crypto/src', 'packages/crypto/package.json', 'LICENSE', 'README.md']
REDISTRIBUTION_FILES = ['BUN-THIRD-PARTY-NOTICES.txt.gz', 'BUN-NOTICE-PROVENANCE.json.gz',
                        'BUN-LINUX-ATTRIBUTION.md', 'OPENPGP-PREBUNDLED-NOTICES.txt',
                        'LICENSE.crypto-GPL-3.0', 'LICENSE.pmcrypto-MIT', 'LICENSE.srp-MIT', 'LICENSE.boltffi-MIT',
                        'CRYPTO-PROVENANCE.md', 'SOURCES.json', 'REBUILD.md', 'OPENPGP-REBUILD.patch']

# The Windows set: the same platform-superset Bun aggregate and JavaScript notices, the
# Windows scope and attribution, ICU 78.3's license (the WebKit Windows prebuild embeds it)
# and the Windows corresponding-source catalog and rebuild notes.
WINDOWS_REDISTRIBUTION_FILES = ['BUN-THIRD-PARTY-NOTICES.txt.gz', 'BUN-NOTICE-PROVENANCE.json.gz',
                                'BUN-LINUX-ATTRIBUTION.md', 'BUN-WINDOWS-ATTRIBUTION.md', 'BUN-WINDOWS-SCOPE.json',
                                'LICENSE.icu4c-78.3', 'OPENPGP-PREBUNDLED-NOTICES.txt', 'LICENSE.crypto-GPL-3.0',
                                'LICENSE.pmcrypto-MIT', 'LICENSE.srp-MIT', 'LICENSE.boltffi-MIT', 'CRYPTO-PROVENANCE.md',
                                'SOURCES-WINDOWS.json', 'REBUILD.md', 'REBUILD-WINDOWS.md', 'OPENPGP-REBUILD.patch']
TARGETS = {'linux-x64': {'bun': 'bun-linux-x64', 'suffix': '', 'output': 'proton-runtime'},
           'windows-x64': {'bun': 'bun-windows-x64', 'suffix': '.exe', 'output': 'proton-runtime-windows'}}
STDOUT_FIXTURE_BYTES = b'{"synthetic":"' + b'x' * (1024 * 1024 - 256) + b'"}\n'
REMOVE_PROBE = {'schemaVersion': 1, 'method': 'remove', 'destination': {'providerId': 'proton-drive',
                'profileId': 'smoke', 'profileRevision': 'smoke', 'settings': {},
                'credentials': {'mode': 'keychain', 'reference': 'smoke'}}}

def windows_bun_runtime(work, output, offline):
    """Fetch the locked Windows Bun runtime and verify it against package-lock.json."""
    locked = json.loads((work / 'package-lock.json').read_text())['packages']['node_modules/@oven/bun-windows-x64']
    algorithm, expected = locked['integrity'].split('-', 1)
    if algorithm != 'sha512':
        raise ValueError('Windows Bun runtime lock has an unexpected integrity algorithm')
    target = output / 'bun-windows-x64'
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    run(['npm', 'pack', f"@oven/bun-windows-x64@{locked['version']}", '--pack-destination', str(target),
         '--silent', *(['--offline'] if offline else [])], work, True)
    archives = list(target.glob('*.tgz'))
    if len(archives) != 1:
        raise ValueError('Windows Bun runtime download did not produce one archive')
    with archives[0].open('rb') as stream:
        if base64.b64encode(hashlib.file_digest(stream, 'sha512').digest()).decode() != expected:
            raise ValueError('Windows Bun runtime archive does not match package-lock.json integrity')
    with tarfile.open(archives[0]) as archive:
        member = archive.getmember('package/bin/bun.exe')
        if not member.isfile():
            raise ValueError('Windows Bun runtime archive has no regular bun.exe')
        executable = target / 'bun.exe'
        with archive.extractfile(member) as source, executable.open('wb') as destination:
            shutil.copyfileobj(source, destination)
    return executable, {'package': '@oven/bun-windows-x64', 'version': locked['version'], 'resolved': locked['resolved'],
                        'integrity': locked['integrity'], 'executableSha256': sha256(executable)}

def apply_crypto_source(work, source, lock):
    commit = run(['git', 'rev-parse', 'HEAD'], source, True).strip()
    if commit != lock['cryptoSourceReference']:
        raise ValueError('Crypto source commit does not match the approved pin')
    if run(['git', 'status', '--porcelain', '--untracked-files=all', '--ignored=matching', '--', *CRYPTO_SOURCE_PATHS], source, True).strip():
        raise ValueError('Crypto source checkout is dirty')
    if any((source / name).is_symlink() for name in CRYPTO_SOURCE_PATHS):
        raise ValueError('Crypto source contains a symlink root')
    package = source / 'packages/crypto'
    metadata = json.loads((package / 'package.json').read_text())
    if metadata['version'] != lock['cryptoVersion'] or metadata['license'] != 'GPL-3.0':
        raise ValueError('Crypto source version/license does not match the approved selection')
    if sha256(source / 'LICENSE') != lock['cryptoLicenseSha256']:
        raise ValueError('Crypto source license hash does not match the approved pin')
    for path in package.joinpath('src').rglob('*'):
        if path.is_symlink():
            raise ValueError('Crypto source contains a symlink')
    bootstrap = json.loads((work / 'package-lock.json').read_text())['packages']['node_modules/@protontech/crypto']
    if bootstrap['version'] != lock['cryptoRegistryBootstrapVersion']:
        raise ValueError('Crypto registry bootstrap version does not match the approved pin')
    target = work / 'node_modules/@protontech/crypto'
    shutil.rmtree(target / 'src')
    shutil.copytree(package / 'src', target / 'src')
    shutil.copyfile(package / 'package.json', target / 'package.json')
    shutil.copyfile(source / 'LICENSE', target / 'LICENSE')
    shutil.copyfile(source / 'README.md', target / 'SOURCE-README.md')
    return {'registryVersion': bootstrap['version'], 'version': metadata['version'],
            'sourceUrl': lock['cryptoSourceUrl'], 'sourceReference': commit, 'license': metadata['license']}

def write_notices(work, source, output, source_overrides, catalog='SOURCES.json'):
    compiled = json.loads((output / 'build-metafile.json').read_text())
    classified = {item['location']: item for item in inventory(work, compiled, source_overrides)}
    sections = ['put.gd Proton runtime: installed dependency license inventory.\n'
                'Includes build packages; this is not a claim that every package is embedded.\n'
                'Registry integrity identifies bootstrap inputs; approved source overrides below replace their code.\n'
                'Bun native and OpenPGP prebundled notices accompany this file under redistribution/.\n'
                f'Corresponding sources and rebuild instructions are catalogued in redistribution/{catalog}.\n',
                'Proton SDK/account source MIT notice:\n' + (source / 'LICENSE.md').read_text()]
    locked = json.loads((work / 'package-lock.json').read_text())['packages']
    for relative, package in sorted(locked.items()):
        if not relative:
            continue
        folder = work / relative
        metadata = folder / 'package.json'
        if not metadata.is_file():
            continue
        actual = json.loads(metadata.read_text())
        sections.append(f"\nPackage: {actual.get('name', relative)} {actual['version']}\n"
                        f"Classification: {classified[relative]['classification']}\n"
                        f"Declared license: {actual.get('license', 'not declared')}\n"
                        f"Integrity: {package.get('integrity', 'local source')}\n")
        if relative in source_overrides:
            sections.append('Selected official source override: ' + json.dumps(source_overrides[relative], sort_keys=True) + '\n')
        licenses = [work / notice['path'] for notice in classified[relative]['suppliedNotices']]
        if not licenses:
            sections.append('No top-level license text shipped in this npm package.\n')
        for item in licenses:
            sections.append(f'{item.name}:\n' + item.read_text(errors='replace'))
    notices = output / 'THIRD-PARTY-NOTICES.txt'
    notices.write_text('\n'.join(sections) + '\n')
    return notices

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Use an existing clean checkout of the pinned official source')
    parser.add_argument('--crypto-source', type=Path, help='Clean checkout of the approved GPL crypto source pin')
    parser.add_argument('--target', choices=sorted(TARGETS), default='linux-x64',
                        help='Runtime platform; windows-x64 cross-compiles and records checks to run on Windows')
    parser.add_argument('--output', type=Path, help='Default: build/dependencies/proton-runtime (Linux) or proton-runtime-windows')
    parser.add_argument('--offline', action='store_true', help='Require cached npm packages and an existing source checkout')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('Provisioning runs on a Linux x86-64 build host')
    lock = json.loads((ROOT / 'runtime-lock.json').read_text())
    if args.target not in lock['platforms']:
        parser.error(f'The runtime lock does not list {args.target}')
    target = TARGETS[args.target]
    cross = args.target != 'linux-x64'
    output = (args.output or ROOT.parents[1] / 'build/dependencies' / target['output']).resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = args.source.resolve() if args.source else output / 'sdk-source'
    if not source.exists():
        if args.offline:
            parser.error('--offline requires an existing pinned source checkout')
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', lock['sourceUrl'], str(source)], output)
        run(['git', 'sparse-checkout', 'init', '--cone'], source)
        run(['git', 'sparse-checkout', 'set', 'cli', 'incubating/account/js', 'config/js'], source)
        run(['git', 'checkout', '--detach', lock['sourceReference']], source)
    commit = run(['git', 'rev-parse', 'HEAD'], source, True).strip()
    if commit != lock['sourceReference']:
        parser.error('Source checkout does not match the pinned official commit')
    run(['git', 'diff', '--exit-code', 'HEAD', '--', *SOURCE_PATHS], source, True)
    crypto_source = args.crypto_source.resolve() if args.crypto_source else output / 'crypto-source'
    if not crypto_source.exists():
        if args.offline:
            parser.error('--offline requires the approved crypto source checkout')
        run(['git', 'clone', '--filter=blob:none', '--no-checkout', lock['cryptoSourceUrl'], str(crypto_source)], output)
        run(['git', 'sparse-checkout', 'init', '--cone'], crypto_source)
        run(['git', 'sparse-checkout', 'set', 'packages/crypto'], crypto_source)
        run(['git', 'checkout', '--detach', lock['cryptoSourceReference']], crypto_source)
    work = output / 'build-source'
    work.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'package.json', work / 'package.json')
    shutil.copyfile(ROOT / 'package-lock.json', work / 'package-lock.json')
    run(['npm', 'ci', '--ignore-scripts', '--no-audit', '--no-fund', *(['--offline'] if args.offline else [])], work)
    bun = work / 'node_modules/@oven/bun-linux-x64/bin/bun'
    if run([str(bun), '--version'], work, True).strip() != lock['bunVersion']:
        parser.error('Pinned Bun version check failed')
    upstream = work / 'upstream'
    upstream.mkdir(exist_ok=True)
    for relative in SOURCE_PATHS:
        src, dst = source / relative, upstream / relative
        if src.is_dir():
            if dst.exists():
                shutil.rmtree(dst)
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns('*.test.ts'))
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
    patch_once(upstream / 'cli/src/credentials/constants.ts',
               "export const CREDENTIALS_SERVICE = 'ch.proton.drive/drive-sdk-cli';",
               "export const CREDENTIALS_SERVICE = 'ch.proton.drive/putgd';")
    patch_once(upstream / 'cli/src/credentials/constants.ts', "export const CREDENTIALS_NAME = 'auth-session';",
               "export let CREDENTIALS_NAME = '';\nexport function configureCredentialsReference(reference: string) { CREDENTIALS_NAME = reference; }")
    shutil.copyfile(source / 'LICENSE.md', work / 'node_modules/@protontech/drive-sdk/LICENSE')
    crypto_override = apply_crypto_source(work, crypto_source, lock)
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/openpgp.ts',
               'import { config } from "openpgp/lightweight";', 'import { config } from "openpgp";')
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/openpgp.ts',
               'export * from "openpgp/lightweight";', 'export * from "openpgp";')
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/pmcrypto.d.ts',
               '} from "openpgp/lightweight";', '} from "openpgp";')
    crypto_folder = work / 'node_modules/@protontech/crypto'
    crypto_inputs = list((crypto_folder / 'src').rglob('*')) + [crypto_folder / name for name in ['package.json', 'LICENSE', 'SOURCE-README.md']]
    crypto_override['sha256'] = {path.relative_to(crypto_folder).as_posix(): sha256(path) for path in sorted(crypto_inputs) if path.is_file()}
    source_overrides = {'node_modules/@protontech/crypto': crypto_override}
    (work / 'source-overrides.json').write_text(json.dumps(source_overrides, indent=2) + '\n')
    for filename in ['runtime.ts', 'runtime.test.ts', 'account.ts', 'crypto-cache.ts', 'crypto-cache.test.ts', 'protocol.ts', 'protocol.test.ts', 'stdout.ts', 'progress.ts', 'progress.test.ts', 'stdout_fixture.ts']:
        shutil.copyfile(ROOT / filename, work / filename)
    config = {'compilerOptions': {'target': 'ESNext', 'module': 'ESNext', 'moduleResolution': 'Bundler',
              'strict': True, 'skipLibCheck': True, 'allowImportingTsExtensions': True, 'noEmit': True,
              'esModuleInterop': True, 'types': ['bun'], 'paths': {
                  'proton-drive-sdk-account': ['./upstream/incubating/account/js/src/index.ts'],
                  'proton-drive-sdk-account/*': ['./upstream/incubating/account/js/src/*'],
                  '@protontech/crypto': ['./node_modules/@protontech/crypto/src/index.ts'],
                  '@protontech/crypto/*': ['./node_modules/@protontech/crypto/src/*'],
                  '@protontech/drive-sdk': ['./node_modules/@protontech/drive-sdk/src/index.ts'],
                  '@protontech/drive-sdk/*': ['./node_modules/@protontech/drive-sdk/src/*']}},
              'include': ['runtime.ts', 'runtime.test.ts', 'account.ts', 'crypto-cache.ts', 'crypto-cache.test.ts', 'protocol.ts', 'protocol.test.ts', 'stdout.ts', 'progress.ts', 'progress.test.ts', 'stdout_fixture.ts']}
    (work / 'tsconfig.json').write_text(json.dumps(config, indent=2) + '\n')
    run([str(bun), 'test', './protocol.test.ts', './runtime.test.ts', './crypto-cache.test.ts', './progress.test.ts'], work)
    if cross:
        return provision_cross(args, lock, target, bun, work, source, output, commit, source_overrides)
    candidate = output / 'putgd-proton-runtime.new'
    run([str(bun), 'build', '--compile', '--target=bun-linux-x64', '--minify', './runtime.ts',
         f'--outfile={candidate}', f'--metafile={output / "build-metafile.json"}'], work)
    version = json.loads(run([str(candidate), '--version'], output, True))
    if version['bunVersion'] != lock['bunVersion'] or version['sdkVersion'] != 'js@' + lock['sdkVersion']:
        parser.error('Compiled runtime version check failed')
    completed = subprocess.run([str(candidate), '--stdio'], input=json.dumps(REMOVE_PROBE), text=True,
                               capture_output=True, check=True, timeout=10)
    if completed.stderr or json.loads(completed.stdout)['error']['code'] != 'unsupported_safe_removal':
        parser.error('Compiled one-request protocol smoke failed')
    run(['python3', str(ROOT / 'smoke_test.py'), '--runtime', str(candidate)], output)
    stdout_fixture = output / 'stdout-fixture-test'
    run([str(bun), 'build', '--compile', '--target=bun-linux-x64', '--minify', './stdout_fixture.ts',
         f'--outfile={stdout_fixture}'], work)
    captured = subprocess.run([str(stdout_fixture)], capture_output=True, check=True, timeout=10)
    if captured.stderr or captured.stdout != STDOUT_FIXTURE_BYTES:
        parser.error('Compiled large stdout regression failed')
    print(f'Compiled stdout regression passed: {len(captured.stdout)} bytes drained before exit.')
    artifact = output / 'putgd-proton-runtime'
    candidate.chmod(0o755)
    candidate.replace(artifact)
    notices = write_notices(work, source, output, source_overrides)
    redistribution = output / 'redistribution'
    redistribution.mkdir(exist_ok=True)
    for name in REDISTRIBUTION_FILES:
        shutil.copyfile(ROOT / 'notices' / name, redistribution / name)
    manifest = {'schemaVersion': 1, 'platform': 'linux-x64', 'artifact': artifact.name, 'sha256': sha256(artifact),
                'sourceReference': commit, 'appVersion': version['appVersion'], 'sdkVersion': version['sdkVersion'],
                'bunVersion': version['bunVersion'], 'protocolVersion': 1,
                'cryptoVersion': lock['cryptoVersion'], 'cryptoSourceReference': lock['cryptoSourceReference'],
                'sourceOverrides': source_overrides,
                'redistributionFilesSha256': {name: sha256(redistribution / name) for name in REDISTRIBUTION_FILES},
                'runtimeLockSha256': sha256(ROOT / 'runtime-lock.json'),
                'dependencyLockSha256': sha256(ROOT / 'package-lock.json'),
                'dependencyNotices': notices.name, 'dependencyNoticesSha256': sha256(notices),
                'helperSourceSha256': helper_source_hashes()}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'artifact': str(artifact), 'manifest': str(output / 'manifest.json'), 'sha256': manifest['sha256']}))

def helper_source_hashes():
    return {name: sha256(ROOT / name) for name in ['runtime.ts', 'account.ts', 'crypto-cache.ts', 'protocol.ts', 'stdout.ts',
                                                   'progress.ts', 'provision_runtime.py', 'audit_redistribution.py']}

def provision_cross(args, lock, target, bun, work, source, output, commit, source_overrides):
    """Cross-compile for a platform the build host cannot execute; its checks run there."""
    executable, runtime = windows_bun_runtime(work, output, args.offline)
    suffix = target['suffix']
    compile_args = [str(bun), 'build', '--compile', f"--target={target['bun']}", f'--compile-executable-path={executable}', '--minify']
    candidate = output / f'putgd-proton-runtime.new{suffix}'
    run([*compile_args, './runtime.ts', f'--outfile={candidate}', f'--metafile={output / "build-metafile.json"}'], work)
    stdout_fixture = output / f'stdout-fixture-test{suffix}'
    run([*compile_args, './stdout_fixture.ts', f'--outfile={stdout_fixture}'], work)
    artifact = output / f'putgd-proton-runtime{suffix}'
    candidate.replace(artifact)
    notices = write_notices(work, source, output, source_overrides, 'SOURCES-WINDOWS.json')
    redistribution = output / 'redistribution'
    if redistribution.exists():
        shutil.rmtree(redistribution)
    redistribution.mkdir()
    for name in WINDOWS_REDISTRIBUTION_FILES:
        shutil.copyfile(ROOT / 'notices' / name, redistribution / name)
    expected_version = {'schemaVersion': 1, 'runtime': 'putgd-proton-runtime',
                        'appVersion': 'external-drive-put_gd@' + lock['runtimeVersion'], 'sdkVersion': 'js@' + lock['sdkVersion'],
                        'bunVersion': lock['bunVersion'], 'protocolVersion': 1}
    manifest = {'schemaVersion': 1, 'platform': args.target, 'artifact': artifact.name, 'sha256': sha256(artifact),
                'sourceReference': commit, 'appVersion': expected_version['appVersion'],
                'sdkVersion': expected_version['sdkVersion'], 'bunVersion': lock['bunVersion'], 'protocolVersion': 1,
                'cryptoVersion': lock['cryptoVersion'], 'cryptoSourceReference': lock['cryptoSourceReference'],
                'sourceOverrides': source_overrides, 'bunTargetRuntime': runtime,
                'hostChecks': 'skipped: the Linux build host cannot execute this artifact; run targetChecks on the target',
                'targetChecks': {
                    'version': {'args': ['--version'], 'stdout': expected_version},
                    'removeProbe': {'args': ['--stdio'], 'stdin': REMOVE_PROBE, 'errorCode': 'unsupported_safe_removal',
                                    'stderr': ''},
                    'smokeTest': 'python tools/proton/smoke_test.py --runtime ' + artifact.name,
                    'stdoutRegression': {'artifact': stdout_fixture.name, 'sha256': sha256(stdout_fixture),
                                         'stdoutBytes': len(STDOUT_FIXTURE_BYTES),
                                         'stdoutSha256': hashlib.sha256(STDOUT_FIXTURE_BYTES).hexdigest(), 'stderr': ''}},
                'redistributionFilesSha256': {name: sha256(redistribution / name) for name in WINDOWS_REDISTRIBUTION_FILES},
                'runtimeLockSha256': sha256(ROOT / 'runtime-lock.json'),
                'dependencyLockSha256': sha256(ROOT / 'package-lock.json'),
                'dependencyNotices': notices.name, 'dependencyNoticesSha256': sha256(notices),
                'helperSourceSha256': helper_source_hashes()}
    gaps = windows_redistribution_gaps(manifest, lock, redistribution)
    manifest['redistributionComplete'] = not gaps
    if gaps:
        manifest['redistributionNote'] = 'Not for distribution: ' + '; '.join(gaps)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'artifact': str(artifact), 'manifest': str(output / 'manifest.json'), 'sha256': manifest['sha256'],
                      'redistributionComplete': manifest['redistributionComplete']}))

def windows_redistribution_gaps(manifest, lock, redistribution):
    """What keeps this Windows artifact from shipping with its complete redistribution set.

    The catalog and the Bun scope are written for one helper and one Bun executable; after
    an overlay or lock change they describe another build until they are updated.
    """
    gaps = [f'redistribution/{name} is missing' for name in WINDOWS_REDISTRIBUTION_FILES
            if not (redistribution / name).is_file()]
    if gaps:
        return gaps
    catalog = json.loads((redistribution / 'SOURCES-WINDOWS.json').read_text())
    scope = json.loads((redistribution / 'BUN-WINDOWS-SCOPE.json').read_text())
    runtime = manifest['bunTargetRuntime']
    if catalog.get('platform') != manifest['platform'] or catalog.get('materialsReady') is not True:
        gaps.append('SOURCES-WINDOWS.json is not a ready windows-x64 catalog')
    if catalog.get('helperSha256') != manifest['sha256']:
        gaps.append(f"SOURCES-WINDOWS.json pins helper {catalog.get('helperSha256')}, this build is {manifest['sha256']}")
    if catalog.get('bunTargetRuntime') != runtime:
        gaps.append('SOURCES-WINDOWS.json names another Windows Bun runtime')
    if scope.get('bunExecutable', {}).get('executableSha256') != runtime['executableSha256']:
        gaps.append('BUN-WINDOWS-SCOPE.json describes another bun.exe')
    pins = catalog.get('sourcePins', {})
    if pins.get('sdk') != lock['sourceReference'] or pins.get('crypto') != lock['cryptoSourceReference']:
        gaps.append('SOURCES-WINDOWS.json source pins differ from runtime-lock.json')
    return gaps

if __name__ == '__main__':
    main()
