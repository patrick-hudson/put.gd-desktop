#!/usr/bin/env python3
"""Collect Proton-only P3 review materials without changing the shipping runtime.

The compiler graph covers JavaScript contributions, not Bun's native components.
This tool records missing evidence; its output is not a compliance certificate.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent
HELPER_FILES = ('runtime.ts', 'account.ts', 'crypto-cache.ts', 'protocol.ts',
                'stdout.ts', 'progress.ts', 'runtime.test.ts', 'protocol.test.ts',
                'crypto-cache.test.ts', 'progress.test.ts', 'stdout_fixture.ts',
                'tsconfig.json', 'package.json', 'package-lock.json', 'source-overrides.json')
BUILD_ONLY = {'node_modules/typescript', 'node_modules/bun', 'node_modules/bun-types',
              'node_modules/@types/bun', 'node_modules/@types/node', 'node_modules/undici-types'}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def remaining_evidence(catalog, helper_sha256):
    """Report the current catalog's gates only for its exact helper fingerprint."""
    if catalog['helperSha256'] != helper_sha256:
        raise ValueError('source catalog does not describe the audited helper')
    proof = catalog.get('proof', {})
    if proof.get('nativeBunRelinkVerified') or proof.get('nativeCompiledHelperSourceSelectionVerified'):
        subject = catalog.get('nativeRelinkProof', {})
        if (subject.get('helperSha256') != helper_sha256 or
                any(subject.get('sourcePins', {}).get(name) != catalog['sourcePins'][name]
                    for name in ('bun', 'webkit', 'tinycc'))):
            raise ValueError('native relink proof does not describe the current helper and native sources')
    if proof.get('finalPackageAcceptanceVerified'):
        subject = catalog.get('desktopResourceAcceptance', {})
        if (subject.get('status') != 'passed' or subject.get('helperSha256') != helper_sha256 or
                subject.get('sourcePins') != catalog['sourcePins']):
            raise ValueError('desktop/resource acceptance does not describe the current helper and sources')
    gates = list(catalog['openGates'])
    if not catalog.get('published'):
        gates.append('Publish corresponding source assets with final binary packages')
    return gates


def search_import_graph(metafile):
    """The public client needs capability glue, but this helper never searches.

    Fail the build if an SDK upgrade starts embedding the Rust/WASM search
    engine or its FFI runtime. A type-only input is not a bundled module.
    """
    inputs = Counter()
    for output in metafile['outputs'].values():
        for name, item in output['inputs'].items():
            if '/search/' in name or '@boltffi/runtime/' in name or name.endswith('.wasm'):
                inputs[name] += item['bytesInOutput']
    adapter = 'node_modules/@protontech/drive-sdk/src/search/sdkCapabilities.ts'
    forbidden = {name: size for name, size in inputs.items() if size > 0 and name != adapter}
    if forbidden:
        raise ValueError('unexpected bundled search engine/WASM/FFI inputs: ' + ', '.join(sorted(forbidden)))
    return {'scope': 'Compiler-visible inputs; SDK capability adapter is distinct from the Rust/WASM engine',
            'inputs': dict(sorted(inputs.items())), 'rustWasmSearchEngineBundled': False,
            'boltffiRuntimeBundled': False}


def inventory(work, metafile, source_overrides=None):
    source_overrides = source_overrides or {}
    locked = json.loads((work / 'package-lock.json').read_text())['packages']
    contributions = Counter()
    for output in metafile['outputs'].values():
        for relative, info in output['inputs'].items():
            if not relative.startswith('node_modules/') or info['bytesInOutput'] == 0:
                continue
            parents = [name for name in locked if name and relative.startswith(name + '/')]
            if not parents:
                raise ValueError('unlocked compiler input: ' + relative)
            contributions[max(parents, key=len)] += info['bytesInOutput']
    result = []
    for relative, package in sorted(locked.items()):
        if not relative:
            continue
        folder = work / relative
        if not folder.resolve().is_relative_to(work.resolve()):
            raise ValueError('package path escapes build source')
        metadata = folder / 'package.json'
        actual = json.loads(metadata.read_text()) if metadata.is_file() else {}
        override = source_overrides.get(relative)
        expected_version = override['version'] if override else package['version']
        if override:
            if override['registryVersion'] != package['version']:
                raise ValueError('source bootstrap version mismatch')
            for name, expected in override['sha256'].items():
                path = folder / name
                if path.is_symlink() or not path.resolve().is_relative_to(folder.resolve()) or digest(path) != expected:
                    raise ValueError('source hash mismatch: ' + relative + '/' + name)
        if actual and actual['version'] != expected_version:
            raise ValueError('installed package version mismatch: ' + relative)
        if contributions[relative]:
            classification = 'bundled-js'
        elif relative == 'node_modules/@oven/bun-linux-x64':
            classification = 'embedded-runtime'
        elif not actual:
            classification = 'not-installed'
        elif relative in BUILD_ONLY:
            classification = 'build-only'
        else:
            classification = 'installed-not-bundled'
        licenses = [p for p in sorted(folder.glob('*')) if p.is_file() and
                    p.name.lower().startswith(('license', 'copying', 'notice', 'copyright'))]
        result.append({'location': relative, 'name': actual.get('name', relative),
                       'version': actual.get('version', package['version']), 'registryVersion': package['version'],
                       'sourceOverride': override, 'declaredLicense': actual.get('license', package.get('license')),
                       'integrity': package.get('integrity'), 'classification': classification,
                       'bundledBytes': contributions[relative],
                       'suppliedNotices': [{'path': p.relative_to(work).as_posix(), 'sha256': digest(p)} for p in licenses]})
    return result


def source_paths(work):
    paths = set()
    for name in HELPER_FILES:
        path = work / name
        if path.is_symlink():
            raise ValueError('source symlink: ' + name)
        if path.is_file():
            paths.add(name)
    folders = ['upstream']
    for relative in json.loads((work / 'package-lock.json').read_text())['packages']:
        if relative and not relative.startswith('node_modules/@oven/') and relative != 'node_modules/bun':
            folders.append(relative)
    for relative in folders:
        folder = work / relative
        if folder.is_symlink():
            raise ValueError('source symlink: ' + relative)
        if folder.is_dir():
            for path in folder.rglob('*'):
                if path.is_symlink():
                    raise ValueError('source symlink: ' + str(path.relative_to(work)))
                if path.is_file():
                    paths.add(path.relative_to(work).as_posix())
    return sorted(paths)


def archive_sources(work, target):
    paths = source_paths(work)
    with target.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as archive:
            for relative in paths:
                path = work / relative
                info = archive.gettarinfo(str(path), arcname='build-source/' + relative)
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ''
                info.mode = 0o644
                with path.open('rb') as content:
                    archive.addfile(info, content)
    return {relative: digest(work / relative) for relative in paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True, help='Existing provisioned runtime directory')
    parser.add_argument('--output', type=Path, required=True, help='New review output directory')
    args = parser.parse_args()
    runtime, output = args.runtime.resolve(), args.output.resolve()
    if output.exists():
        parser.error('--output must be new; preserve prior attempts')
    manifest = json.loads((runtime / 'manifest.json').read_text())
    catalog_path = ROOT / 'notices/SOURCES.json'
    catalog = json.loads(catalog_path.read_text())
    gates = remaining_evidence(catalog, manifest['sha256'])
    work = runtime / 'build-source'
    if digest(runtime / manifest['artifact']) != manifest['sha256']:
        parser.error('runtime hash does not match manifest')
    if digest(ROOT / 'runtime-lock.json') != manifest['runtimeLockSha256']:
        parser.error('runtime lock does not match runtime manifest')
    if digest(work / 'package-lock.json') != manifest['dependencyLockSha256']:
        parser.error('build lock does not match runtime manifest')
    for name, expected in manifest['helperSourceSha256'].items():
        if digest(ROOT / name) != expected:
            parser.error('helper sources do not match runtime manifest')
    for name, expected in manifest.get('redistributionFilesSha256', {}).items():
        if Path(name).name != name:
            parser.error('invalid redistribution file name')
        if digest(runtime / 'redistribution' / name) != expected or digest(ROOT / 'notices' / name) != expected:
            parser.error('redistribution materials do not match runtime manifest')
    output.mkdir(mode=0o700, parents=True)
    bun = work / 'node_modules/@oven/bun-linux-x64/bin/bun'
    lock = json.loads((ROOT / 'runtime-lock.json').read_text())
    if digest(bun) != lock['bunSha256'] or manifest['bunSha256'] != lock['bunSha256']:
        parser.error('Bun executable does not match the pinned official checksum')
    with tempfile.TemporaryDirectory(prefix='pg-p3-') as temporary:
        temporary = Path(temporary)
        candidate = temporary / 'putgd-proton-runtime.new'
        meta = temporary / 'metafile.json'
        result = subprocess.run([str(bun), 'build', '--compile', '--target=bun-linux-x64', '--minify',
                                 '--compile-executable-path=' + str(bun), './runtime.ts', '--outfile=' + str(candidate), '--metafile=' + str(meta)],
                                cwd=work, capture_output=True, text=True, check=True)
        (output / 'compile.txt').write_text(result.stdout + result.stderr)
        if digest(candidate) != manifest['sha256']:
            raise ValueError('audited compiler output does not reproduce shipping runtime; preserve output')
        metafile = json.loads(meta.read_text())
        (output / 'metafile.json').write_text(json.dumps(metafile, indent=2) + '\n')
    packages = inventory(work, metafile, manifest.get('sourceOverrides', {}))
    sources = archive_sources(work, output / 'helper-rebuild.tar.gz')
    (output / 'source-index.json').write_text(json.dumps(sources, indent=2) + '\n')
    (output / 'REBUILD.md').write_text((ROOT / 'notices/REBUILD.md').read_text())
    sections = ['Supplied npm license texts, attributed by exact installed package.\n'
                'Bun native notices and packages without supplied texts are separate review requirements.\n']
    for package in packages:
        if package['classification'] == 'not-installed':
            continue
        sections.append(f"\n{package['name']} {package['version']} [{package['classification']}]\nLocation: {package['location']}\n")
        for notice in package['suppliedNotices']:
            sections.append(notice['path'] + '\n' + (work / notice['path']).read_text(errors='replace'))
    (output / 'SUPPLIED-NPM-NOTICES.txt').write_text('\n'.join(sections) + '\n')
    report = {'schemaVersion': 1, 'runtimeSha256': manifest['sha256'], 'bunSha256': digest(bun),
              'dependencyLockSha256': manifest['dependencyLockSha256'], 'exactRebuild': True,
              'classificationCounts': dict(Counter(p['classification'] for p in packages)), 'packages': packages,
              'inventoryScope': 'Compiler-visible JavaScript packages only; OpenPGP prebundled dependencies and Bun native components need separate inventories',
              'bundledMissingSuppliedTexts': [p['location'] for p in packages if p['classification'] == 'bundled-js' and not p['suppliedNotices']],
              'reviewComplete': False,
              'sourceOverrides': manifest.get('sourceOverrides', {}),
              'sourceCatalogSha256': digest(catalog_path),
              'searchImportGraph': search_import_graph(metafile),
              'remainingEvidence': gates,
              'sourceFiles': len(sources), 'sourceArchiveSha256': digest(output / 'helper-rebuild.tar.gz')}
    (output / 'inventory.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ['runtimeSha256', 'exactRebuild', 'classificationCounts', 'sourceFiles', 'reviewComplete']}))


if __name__ == '__main__':
    main()
