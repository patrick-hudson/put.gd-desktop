#!/usr/bin/env python3
"""Collect complete AppImage sources, bound to every component of its receipt.

put.gd and Proton sources remain the release's separately verified assets. All
other bundled components (including static runtime dependencies) are collected
here. Downloads never execute source code. Missing exact sources are fatal.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import urllib.request
from urllib.parse import urlsplit

from audit_native import sha256

RECEIPT_NAME = 'appimage-complete-source.json'
ROOT = Path(__file__).resolve().parents[2]


def archive_name(version):
    if not re.fullmatch(r'[0-9][A-Za-z0-9.+~_-]{0,127}', version):
        raise ValueError('Unsafe source asset version')
    return 'putgd-' + version + '-appimage-complete-source.tar.gz'


def inventory(receipt):
    components = receipt['notices']['inventory']['components']
    records = []
    seen = set()
    for item in components:
        name = item['id']
        if name in seen or name in ('.', '..') or not re.fullmatch(r'[A-Za-z0-9:+_.-]+', name):
            raise ValueError('Duplicate or unsafe component in AppImage inventory')
        seen.add(name)
        source = item['source']
        if name == 'putgd':
            if source.get('commit') != receipt['build']['gitCommit']:
                raise ValueError('put.gd source identity differs from AppImage')
            record = {'id': name, 'external': 'putgd-' + receipt['build']['version'] + '-source.tar.gz', 'source': source}
        elif name == 'proton-helper':
            record = {'id': name, 'external': 'proton-source-catalog.json', 'source': source}
        elif name.startswith('ubuntu:'):
            if not re.fullmatch(r'[a-z0-9][a-z0-9+.-]*', source.get('sourcePackage', '')) or not re.fullmatch(r'[A-Za-z0-9:.+~_-]+', source.get('sourceVersion', '')):
                raise ValueError('Distro component has no exact source package')
            record = {'id': name, 'source': source}
        elif source.get('repository') and re.fullmatch(r'[0-9a-f]{40}', source.get('commit', '')):
            record = {'id': name, 'source': source}
        else:
            raise ValueError('Bundled component has no complete source mapping: ' + name)
        records.append(record)
    required = {'putgd', 'proton-helper', 'appimage-runtime', 'ffmpeg', 'icu', 'layershell', 'aws-sdk-cpp'}
    if not required.issubset(seen):
        raise ValueError('Incomplete AppImage component inventory')
    # Also require every current Qt module pin, rather than silently accepting
    # an old inventory that predates Qt's complete SPDX/module mapping.
    qt = receipt['toolLock']['buildInputs']['qtLicenseSources']
    for pin in qt:
        name = pin['repository'].rsplit('/', 1)[-1].removesuffix('.git')
        match = next((r for r in records if r['id'] == name), None)
        if match is None or match['source']['commit'] != pin['commit']:
            raise ValueError('Qt source inventory differs from pinned modules')
    return sorted(records, key=lambda item: item['id'])


def runtime_inputs(receipt, path):
    lock = json.loads(path.read_text())
    runtime = next(t for t in receipt['toolLock']['tools'] if t['name'] == 'runtime-x86_64')
    if lock['runtime'] != {k: runtime[k] for k in ('commit', 'sha256')}:
        raise ValueError('Runtime source closure is for another runtime binary')
    if not lock.get('archives') or not lock.get('recipes'):
        raise ValueError('Runtime source closure is incomplete')
    return lock


def _run(*args, **kwargs):
    return subprocess.run(list(args), check=True, stdout=subprocess.PIPE, **kwargs)


def git_source(source, cache, target):
    """Archive a pinned tree and its recursive submodules, including sparse trees."""
    repository, commit = source['repository'], source['commit']
    if not repository.startswith('https://') or urlsplit(repository).username is not None or not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Source repository must be HTTPS with a full commit pin')
    key = hashlib.sha256((repository + '@' + commit).encode()).hexdigest()
    checkout = cache / key
    if not checkout.exists():
        _run('git', 'init', str(checkout))
        _run('git', '-C', str(checkout), 'remote', 'add', 'origin', repository)
        _run('git', '-C', str(checkout), 'fetch', '--depth=1', 'origin', commit)
        _run('git', '-C', str(checkout), 'sparse-checkout', 'set', '--no-cone', '/.gitmodules')
        _run('git', '-C', str(checkout), 'checkout', '--detach', commit)
        _run('git', '-C', str(checkout), 'submodule', 'update', '--init', '--recursive', '--depth=1')
    actual = _run('git', '-C', str(checkout), 'rev-parse', 'HEAD').stdout.decode().strip()
    if actual != commit:
        raise ValueError('Source checkout differs from component commit')
    target.mkdir()
    result = []

    def archive_tree(tree, pin, relative):
        file = target / (hashlib.sha256(relative.encode()).hexdigest()[:16] + '.tar')
        _run('git', '-C', str(tree), 'archive', '--format=tar', '--output=' + str(file), pin,
             '--', *source.get('paths', []))
        result.append({'file': file.name, 'treePath': relative, 'commit': pin})
        entries = _run('git', '-C', str(tree), 'ls-tree', '-rz', pin).stdout
        for entry in entries.split(b'\0'):
            if entry.startswith(b'160000 '):
                metadata, path = entry.split(b'\t', 1)
                child = path.decode()
                archive_tree(tree / child, metadata.split()[2].decode(), (relative + '/' + child).lstrip('/'))
    archive_tree(checkout, commit, '')
    (target / 'TREES.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def apt_source(package, version, target):
    subprocess.run(['apt-get', 'source', '--download-only', '-qq', package + '=' + version], check=True, cwd=target)


def validate_dsc(target, package, version):
    files = list(target.iterdir())
    dsc = [p for p in files if p.suffix == '.dsc']
    if len(dsc) != 1:
        raise ValueError('Missing exact distro source package: ' + package)
    text = dsc[0].read_text()
    if not re.search(r'(?m)^Source: ' + re.escape(package) + r'$', text) or not re.search(r'(?m)^Version: ' + re.escape(version) + r'$', text):
        raise ValueError('Distro source identity differs from bundled package')
    checksums = re.search(r'(?m)^Checksums-Sha256:\n((?: [^\n]+\n)+)', text)
    if checksums is None:
        raise ValueError('Distro source package has no SHA-256 manifest')
    expected = {dsc[0].name}
    for line in checksums[1].splitlines():
        digest, size, name = line.split()
        if Path(name).name != name or name in expected:
            raise ValueError('Unsafe or duplicate distro source file')
        expected.add(name)
        path = target / name
        if not path.is_file() or path.is_symlink() or sha256(path) != digest or path.stat().st_size != int(size):
            raise ValueError('Distro source checksum differs: ' + name)
    if {p.name for p in files} != expected:
        raise ValueError('Unrecorded distro source file')


def fetch_archive(item, target):
    if not item['url'].startswith('https://') or not re.fullmatch(r'[A-Za-z0-9._+-]+', item['file']):
        raise ValueError('Unsafe runtime source URL or filename')
    with urllib.request.urlopen(item['url'], timeout=120) as response, target.open('wb') as stream:
        while block := response.read(1024 * 1024):
            stream.write(block)
    algorithm = item.get('algorithm', 'sha256')
    if algorithm not in ('sha256', 'sha512'):
        raise ValueError('Unsupported runtime source digest')
    with target.open('rb') as stream:
        actual = hashlib.file_digest(stream, algorithm).hexdigest()
    if actual != item['digest']:
        raise ValueError('Runtime source checksum differs: ' + item['file'])


def build(receipt_path, output, timestamp, cache, runtime_lock, *, git=git_source, apt=apt_source, fetch=fetch_archive):
    receipt = json.loads(receipt_path.read_text())
    components = inventory(receipt)
    closure = runtime_inputs(receipt, runtime_lock)
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pgsrc-') as temp:
        stage = Path(temp)
        files = []
        sources = []
        for item in components:
            if 'external' in item:
                sources.append(item)
                continue
            source = item['source']
            if item['id'].startswith('ubuntu:'):
                key = source['sourcePackage'] + '@' + source['sourceVersion']
                previous = next((s for s in sources if s.get('sourceKey') == key), None)
                if previous:
                    sources.append({**item, 'sourceKey': key, 'directory': previous['directory']})
                    continue
                name = 'ubuntu/' + source['sourcePackage'] + '-' + hashlib.sha256(key.encode()).hexdigest()[:12]
                target = stage / name
                target.mkdir(parents=True)
                apt(source['sourcePackage'], source['sourceVersion'], target)
                validate_dsc(target, source['sourcePackage'], source['sourceVersion'])
                sources.append({**item, 'sourceKey': key, 'directory': name})
            else:
                name = 'upstream/' + item['id']
                target = stage / name
                target.parent.mkdir(exist_ok=True)
                trees = git(source, cache, target)
                sources.append({**item, 'directory': name, 'trees': trees})
        for item in closure['recipes']:
            target = stage / 'runtime' / item['id']
            target.parent.mkdir(exist_ok=True)
            git(item, cache, target)
        for item in closure['archives']:
            target = stage / 'runtime' / item['file']
            target.parent.mkdir(exist_ok=True)
            fetch(item, target)
        (stage / 'runtime/source-lock.json').write_text(json.dumps(closure, indent=2) + '\n')
        # Include exact build recipes, patches and configure options from the
        # release commit; never scrape arbitrary worktree files or payloads.
        recipes = ['tools/releases/provision-appimage.py', 'tools/releases/build-appimage.sh',
                   'packaging/appimage/tools.lock.json', 'packaging/appimage/media.json',
                   'packaging/appimage/components.json', 'cmake/dependency-lock.json']
        for name in recipes:
            data = _run('git', '-C', str(ROOT), 'show', receipt['build']['gitCommit'] + ':' + name).stdout
            path = stage / 'build-recipes' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                files.append({'file': path.relative_to(stage).as_posix(), 'sha256': sha256(path), 'bytes': path.stat().st_size})
        catalog = {'schemaVersion': 1, 'appImage': receipt['artifact'], 'components': sources,
                   'runtime': closure, 'files': files}
        (stage / 'SOURCES.json').write_text(json.dumps(catalog, indent=2) + '\n')
        (stage / 'README.txt').write_text(
            'Complete bundled-component source for put.gd AppImage ' + receipt['build']['version'] + '.\n'
            'SOURCES.json binds every component to its exact image/source and lists file checksums.\n'
            'put.gd itself and the Proton source parts/catalog are separate assets on the SAME release.\n'
            'upstream/*/TREES.json maps each git archive to its original submodule path; extract each\n'
            'archive there (root first). Ubuntu .dsc + orig/debian archives unpack with dpkg-source -x.\n'
            'Build the image with the exact scripts/configure options in build-recipes; Qt modules\n'
            'include their embedded third-party sources. Runtime source includes its build scripts,\n'
            'libfuse patch, static-library sources and Alpine packaging recipes/patches. See runtime/\n'
            'source-lock.json for the upstream binary identity and version/provenance evidence.\n')
        archive = output / archive_name(receipt['build']['version'])
        with archive.open('wb') as stream, gzip.GzipFile(filename='', fileobj=stream, mode='wb', mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode='w|', format=tarfile.PAX_FORMAT) as packed:
                for path in sorted(stage.rglob('*')):
                    if path.is_file():
                        info = packed.gettarinfo(str(path), path.relative_to(stage).as_posix())
                        info.uid = info.gid = 0
                        info.uname = info.gname = 'root'
                        info.mtime = timestamp
                        info.mode = 0o644
                        with path.open('rb') as data:
                            packed.addfile(info, data)
        result = {'schemaVersion': 1, 'format': 'appimage-complete-source', 'appImage': receipt['artifact'],
                  'file': archive.name, 'sha256': sha256(archive), 'bytes': archive.stat().st_size,
                  'components': components, 'runtime': closure}
        (output / RECEIPT_NAME).write_text(json.dumps(result, indent=2) + '\n')
        return result


def verify(directory, receipt, image):
    path = directory / archive_name(image['build']['version'])
    if receipt.get('format') != 'appimage-complete-source' or receipt.get('file') != path.name:
        raise ValueError('Complete AppImage source asset is required')
    if path.is_symlink() or not path.is_file() or sha256(path) != receipt['sha256'] or path.stat().st_size != receipt['bytes']:
        raise ValueError('Complete AppImage source bytes differ from receipt')
    if receipt['appImage'] != image['artifact'] or receipt['components'] != inventory(image):
        raise ValueError('Complete source component inventory differs from AppImage')
    if receipt['runtime'] != runtime_inputs(image, ROOT / 'packaging/appimage/runtime-source.json'):
        raise ValueError('Runtime source closure differs from approved sources')
    with tarfile.open(path) as packed:
        catalog = json.load(packed.extractfile('SOURCES.json'))
        if catalog['appImage'] != image['artifact'] or catalog['runtime'] != receipt['runtime']:
            raise ValueError('Archived source catalog differs from receipt')
        projected = [{k: v for k, v in item.items() if k in ('id', 'source', 'external')}
                     for item in catalog['components']]
        if projected != receipt['components']:
            raise ValueError('Archived source coverage differs from component inventory')
        expected = {item['file']: item for item in catalog['files']}
        found = set()
        for member in packed:
            if not member.isfile() or Path(member.name).is_absolute() or '..' in Path(member.name).parts or member.name in found:
                raise ValueError('Unsafe source archive member')
            found.add(member.name)
            if member.name in ('SOURCES.json', 'README.txt'):
                continue
            item = expected.get(member.name)
            if item is None or item['bytes'] != member.size or hashlib.file_digest(packed.extractfile(member), 'sha256').hexdigest() != item['sha256']:
                raise ValueError('Archived source file differs from catalog')
        if found != set(expected) | {'SOURCES.json', 'README.txt'}:
            raise ValueError('Archived source files missing from catalog')
        for item in catalog['components']:
            if 'external' not in item and not any(name.startswith(item['directory'] + '/') for name in found):
                raise ValueError('Bundled component source is empty')
    return path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receipt', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--timestamp', type=int, required=True)
    p.add_argument('--cache', type=Path, default=ROOT / 'build/dependencies/appimage-complete-source')
    p.add_argument('--runtime-lock', type=Path, default=ROOT / 'packaging/appimage/runtime-source.json')
    a = p.parse_args()
    print(json.dumps(build(a.receipt, a.output_dir, a.timestamp, a.cache, a.runtime_lock)))


if __name__ == '__main__':
    main()
