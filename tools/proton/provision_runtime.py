#!/usr/bin/env python3
"""Build the pinned bundled Proton runtime before CMake; never called by CMake."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess

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

def write_notices(work, source, output):
    sections = ['put.gd Proton runtime: installed dependency license inventory.\n'
                'Includes build packages; this is not a claim that every package is embedded.\n'
                'Missing bundled license files and Bun embedded third-party notices remain release gates.\n',
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
        sections.append(f"\nPackage: {actual.get('name', relative)} {package['version']}\n"
                        f"Declared license: {actual.get('license', 'not declared')}\n"
                        f"Integrity: {package.get('integrity', 'local source')}\n")
        licenses = [item for item in sorted(folder.iterdir()) if item.is_file() and
                    item.name.lower().startswith(('license', 'copying', 'notice', 'copyright'))]
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
    parser.add_argument('--output', type=Path, default=ROOT.parents[1] / 'build/dependencies/proton-runtime')
    parser.add_argument('--offline', action='store_true', help='Require cached npm packages and an existing source checkout')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        parser.error('This runtime lock targets Linux x86-64')
    lock = json.loads((ROOT / 'runtime-lock.json').read_text())
    output = args.output.resolve()
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
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/openpgp.ts',
               'import { config } from "openpgp/lightweight";', 'import { config } from "openpgp";')
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/openpgp.ts',
               'export * from "openpgp/lightweight";', 'export * from "openpgp";')
    patch_once(work / 'node_modules/@protontech/crypto/src/pmcrypto/pmcrypto.d.ts',
               '} from "openpgp/lightweight";', '} from "openpgp";')
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
    candidate = output / 'putgd-proton-runtime.new'
    run([str(bun), 'build', '--compile', '--target=bun-linux-x64', '--minify', './runtime.ts',
         f'--outfile={candidate}'], work)
    version = json.loads(run([str(candidate), '--version'], output, True))
    if version['bunVersion'] != lock['bunVersion'] or version['sdkVersion'] != 'js@' + lock['sdkVersion']:
        parser.error('Compiled runtime version check failed')
    probe = {'schemaVersion': 1, 'method': 'remove', 'destination': {'providerId': 'proton-drive',
             'profileId': 'smoke', 'profileRevision': 'smoke', 'settings': {},
             'credentials': {'mode': 'keychain', 'reference': 'smoke'}}}
    completed = subprocess.run([str(candidate), '--stdio'], input=json.dumps(probe), text=True,
                               capture_output=True, check=True, timeout=10)
    if completed.stderr or json.loads(completed.stdout)['error']['code'] != 'unsupported_safe_removal':
        parser.error('Compiled one-request protocol smoke failed')
    run(['python3', str(ROOT / 'smoke_test.py'), '--runtime', str(candidate)], output)
    stdout_fixture = output / 'stdout-fixture-test'
    run([str(bun), 'build', '--compile', '--target=bun-linux-x64', '--minify', './stdout_fixture.ts',
         f'--outfile={stdout_fixture}'], work)
    captured = subprocess.run([str(stdout_fixture)], capture_output=True, check=True, timeout=10)
    expected_stdout = b'{"synthetic":"' + b'x' * (1024 * 1024 - 256) + b'"}\n'
    if captured.stderr or captured.stdout != expected_stdout:
        parser.error('Compiled large stdout regression failed')
    print(f'Compiled stdout regression passed: {len(captured.stdout)} bytes drained before exit.')
    artifact = output / 'putgd-proton-runtime'
    candidate.chmod(0o755)
    candidate.replace(artifact)
    notices = write_notices(work, source, output)
    manifest = {'schemaVersion': 1, 'artifact': artifact.name, 'sha256': sha256(artifact),
                'sourceReference': commit, 'appVersion': version['appVersion'], 'sdkVersion': version['sdkVersion'],
                'bunVersion': version['bunVersion'], 'protocolVersion': 1,
                'dependencyLockSha256': sha256(ROOT / 'package-lock.json'),
                'dependencyNotices': notices.name, 'dependencyNoticesSha256': sha256(notices),
                'helperSourceSha256': {name: sha256(ROOT / name) for name in ['runtime.ts', 'account.ts', 'crypto-cache.ts', 'protocol.ts', 'stdout.ts', 'progress.ts', 'provision_runtime.py']}}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'artifact': str(artifact), 'manifest': str(output / 'manifest.json'), 'sha256': manifest['sha256']}))

if __name__ == '__main__':
    main()
