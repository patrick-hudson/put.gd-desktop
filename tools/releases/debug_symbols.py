#!/usr/bin/env python3
"""Kept debug symbols (#124): stage each build's split .debug files by GNU build ID,
and assemble the release asset putgd-<version>-debug-<arch>.tar.zst from them.

Release builds compile with -g and split each product binary's debug information
into <binary>.debug at link time (CMakeLists.txt). `stage` checks every product
binary in a build tree against its .debug file and against the binary the
package installs (same GNU build ID), then writes debug-symbols-<label>.tar and
its manifest. `combine` checks every staged build and writes one tar.zst laid
out like a debuginfod cache: .build-id/<2 hex>/<rest>.debug, plus manifest.json
naming each build's programs and build IDs. See docs/diagnostics.md.

The builds keep zlib-compressed debug sections, which keeps the staged files
and the jobs' artifacts small. The asset decompresses them, moves the DWARF
each build's programs share into one dwz file per build, and compresses the
whole tar in one long-window zstd pass: about half the size (#162).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

PRODUCTS = ('putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-kde-region', 'putgd-record',
            'putgd-s3-worker', 'putgd-b2-worker', 'libputgd-s3.so')
# Built on every Linux packaging job; the others depend on the desktop libraries present.
REQUIRED = ('putgd', 'putgd-cli', 'putgd-ui', 'putgd-capture', 'putgd-s3-worker', 'putgd-b2-worker', 'libputgd-s3.so')
# Package matrix names in .github/workflows/package.yml, plus the AppImage.
LABELS = ('ubuntu24.04', 'ubuntu26.04', 'debian13', 'fedora44', 'appimage')
ARCHITECTURE = 'x86_64'
BUILD_ID = re.compile(r'[0-9a-f]{16,64}')
SHT_NOTE = 7
NT_GNU_BUILD_ID = 3
SHF_COMPRESSED = 0x800
# 128 MiB is the window every zstd decoder accepts without --long or
# --memory, so `tar --zstd -xf` still unpacks the asset.
ASSET_ZSTD = ('-19', '--long=27')


def asset_name(version: str, architecture: str = ARCHITECTURE) -> str:
    return 'putgd-' + version + '-debug-' + architecture + '.tar.zst'


def sha256(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def elf_sections(data: bytes, *, flags: bool = False) -> dict:
    """{name: (type, offset, size)} for a little-endian ELF64 or ELF32 file,
    or {name: (type, offset, size, flags)} with flags=True."""
    if data[:4] != b'\x7fELF' or data[5] != 1:
        raise ValueError('Not a little-endian ELF file')
    wide = data[4] == 2
    if wide:
        shoff, = struct.unpack_from('<Q', data, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
        layout = '<IIQQQQIIQQ'
    else:
        shoff, = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
        layout = '<IIIIIIIIII'
    headers = [struct.unpack_from(layout, data, shoff + index * shentsize) for index in range(shnum)]
    names = headers[shstrndx]
    names_offset, names_size = names[4], names[5]
    table = data[names_offset:names_offset + names_size]
    sections = {}
    for header in headers:
        name = table[header[0]:table.index(b'\0', header[0])].decode('ascii', 'replace')
        sections[name] = (header[1], header[4], header[5], header[2]) if flags else (header[1], header[4], header[5])
    return sections


def compressed_sections(data: bytes) -> list[str]:
    return sorted(name for name, (_, _, _, value) in elf_sections(data, flags=True).items() if value & SHF_COMPRESSED)


def decompressed(data: bytes, temporary: Path) -> bytes:
    """The debug file with its sections uncompressed (objcopy), or unchanged."""
    if not compressed_sections(data):
        return data
    objcopy = shutil.which('objcopy')
    if objcopy is None:
        raise ValueError('objcopy is required to decompress debug sections')
    source, target = temporary / 'compressed.debug', temporary / 'plain.debug'
    source.write_bytes(data)
    subprocess.run([objcopy, '--decompress-debug-sections', str(source), str(target)], check=True)
    plain = target.read_bytes()
    if compressed_sections(plain) or build_id(target) != build_id(source):
        raise ValueError('Debug sections could not be decompressed')
    return plain


def build_id(path: Path) -> str:
    data = path.read_bytes()
    for name, (kind, offset, size) in elf_sections(data).items():
        if kind != SHT_NOTE:
            continue
        at, end = offset, offset + size
        while at + 12 <= end:
            namesz, descsz, note_type = struct.unpack_from('<III', data, at)
            name_at = at + 12
            desc_at = name_at + ((namesz + 3) & ~3)
            if note_type == NT_GNU_BUILD_ID and data[name_at:name_at + namesz] == b'GNU\0':
                return data[desc_at:desc_at + descsz].hex()
            at = desc_at + ((descsz + 3) & ~3)
    raise ValueError('No GNU build ID: ' + path.name)


def debuglink(path: Path) -> str | None:
    data = path.read_bytes()
    section = elf_sections(data).get('.gnu_debuglink')
    if section is None:
        return None
    _, offset, size = section
    raw = data[offset:offset + size]
    return raw[:raw.index(b'\0')].decode()


def has_debug_info(path: Path) -> bool:
    return '.debug_info' in elf_sections(path.read_bytes())


def installed_path(root: Path, product: str) -> Path | None:
    if product.endswith('.so'):
        matches = sorted(root.glob('usr/lib*/putgd/' + product)) + sorted(root.glob('usr/lib/*/putgd/' + product))
        return matches[0] if matches else None
    path = root / 'usr/bin' / product
    return path if path.is_file() else None


def stage(build: Path, installed: Path, output: Path, label: str, timestamp: int = 0) -> dict:
    """Checks one build's split debug files and writes debug-symbols-<label>.tar/.json."""
    if label not in LABELS:
        raise ValueError('Unknown debug symbol label: ' + label)
    identity = json.loads((build / 'build-identity/build-info.json').read_text())
    entries = []
    for product in PRODUCTS:
        binary = build / product
        if not binary.is_file():
            if product in REQUIRED:
                raise ValueError('Product binary missing from the build: ' + product)
            continue
        debug = build / (product + '.debug')
        if not debug.is_file():
            raise ValueError('Split debug file missing: ' + debug.name + ' (configure Release with PUTGD_SPLIT_DEBUG)')
        identifier = build_id(binary)
        if build_id(debug) != identifier:
            raise ValueError('Debug file build ID differs from its binary: ' + product)
        if debuglink(binary) != debug.name:
            raise ValueError('Binary has no .gnu_debuglink to its debug file: ' + product)
        if has_debug_info(binary) or not has_debug_info(debug):
            raise ValueError('Debug information was not split out of ' + product)
        target = installed_path(installed, product)
        if target is None:
            raise ValueError('Packaged binary missing: ' + product)
        if build_id(target) != identifier:
            raise ValueError('Packaged binary has a different build ID than the build tree: ' + product)
        entries.append({'program': product, 'buildId': identifier, 'path': '.build-id/' + identifier[:2] + '/' + identifier[2:] + '.debug',
                        'bytes': debug.stat().st_size, 'sha256': sha256(debug), 'source': debug})
    if len({entry['buildId'] for entry in entries}) != len(entries):
        raise ValueError('Two products share one build ID')
    output.mkdir(parents=True, exist_ok=True)
    archive = output / ('debug-symbols-' + label + '.tar')
    if archive.exists():
        raise ValueError('Debug symbol archive already exists: ' + archive.name)
    with tarfile.open(archive, 'w', format=tarfile.USTAR_FORMAT) as tar:
        for entry in sorted(entries, key=lambda value: value['path']):
            _add(tar, entry['path'], entry['source'], timestamp)
    manifest = {'schemaVersion': 1, 'label': label, 'architecture': ARCHITECTURE, 'build': identity,
                'archive': {'file': archive.name, 'bytes': archive.stat().st_size, 'sha256': sha256(archive)},
                'files': [{key: value for key, value in entry.items() if key != 'source'} for entry in entries]}
    (output / ('debug-symbols-' + label + '.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def _add(tar, name, source, timestamp, data=None):
    info = tarfile.TarInfo(name)
    info.mtime = timestamp
    info.mode = 0o644
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    if data is None:
        info.size = Path(source).stat().st_size
        with Path(source).open('rb') as stream:
            tar.addfile(info, stream)
    else:
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))


def _verified_build(directory: Path, label: str, identity_check) -> tuple[dict, dict]:
    path = directory / ('debug-symbols-' + label + '.json')
    if not path.is_file() or path.is_symlink():
        raise ValueError('Debug symbols were not staged for this build: ' + label)
    manifest = json.loads(path.read_text())
    archive = directory / ('debug-symbols-' + label + '.tar')
    if (manifest.get('schemaVersion') != 1 or manifest.get('label') != label
            or manifest.get('architecture') != ARCHITECTURE or not archive.is_file() or archive.is_symlink()):
        raise ValueError('Debug symbol manifest does not describe this build: ' + label)
    if sha256(archive) != manifest['archive']['sha256'] or archive.stat().st_size != manifest['archive']['bytes']:
        raise ValueError('Debug symbol archive bytes differ from its manifest: ' + label)
    if not identity_check(manifest['build']):
        raise ValueError('Debug symbols were built from a different source identity: ' + label)
    programs = {entry['program'] for entry in manifest['files']}
    if not set(REQUIRED).issubset(programs) or not programs.issubset(PRODUCTS):
        raise ValueError('Debug symbols do not cover every product binary: ' + label)
    files = {}
    with tarfile.open(archive, 'r:') as tar:
        members = tar.getmembers()
        expected = {entry['path']: entry for entry in manifest['files']}
        if sorted(member.name for member in members) != sorted(expected):
            raise ValueError('Debug symbol archive inventory differs from its manifest: ' + label)
        for member in members:
            entry = expected[member.name]
            if (not member.isfile() or not BUILD_ID.fullmatch(entry['buildId'])
                    or member.name != '.build-id/' + entry['buildId'][:2] + '/' + entry['buildId'][2:] + '.debug'):
                raise ValueError('Unexpected debug symbol archive member: ' + member.name)
            data = tar.extractfile(member).read()
            if hashlib.sha256(data).hexdigest() != entry['sha256'] or len(data) != entry['bytes']:
                raise ValueError('Debug file bytes differ from the manifest: ' + member.name)
            files[member.name] = data
    return manifest, files


def prepare(inputs: dict, identity_check) -> dict:
    """inputs: {label: directory holding that build's staged files}. Checks every
    build and merges their debug files; nothing is written."""
    if sorted(inputs) != sorted(LABELS):
        raise ValueError('Debug symbols need exactly these builds: ' + ', '.join(LABELS))
    merged, builds = {}, []
    for label in LABELS:
        manifest, files = _verified_build(inputs[label], label, identity_check)
        for name, data in files.items():
            if name in merged and merged[name] != data:
                raise ValueError('One build ID has two different debug files: ' + name)
            merged[name] = data
        builds.append({'label': label, 'build': manifest['build'],
                       'programs': [{'program': entry['program'], 'buildId': entry['buildId']} for entry in manifest['files']]})
    return {'files': merged, 'builds': builds}


def deduplicate(root: Path, builds: list) -> dict:
    """Runs dwz on each build's debug files under root (.build-id layout), moving
    the DWARF they share into .dwz/<label>.debug. Each file's .gnu_debugaltlink
    names it relative to the file (../../.dwz/<label>.debug), so an extracted
    asset resolves it wherever it lies; an absolute /usr/lib/debug path would
    not. Synthetic files without real DWARF are left alone. {label: path}."""
    jobs, seen = [], set()
    for build in builds:
        names = []
        for program in build['programs']:
            identifier = program['buildId']
            name = '.build-id/' + identifier[:2] + '/' + identifier[2:] + '.debug'
            if name in seen or not (root / name).is_file():
                continue
            seen.add(name)
            sections = elf_sections((root / name).read_bytes())
            if '.debug_info' in sections and '.debug_abbrev' in sections:
                names.append(name)
        if names:
            jobs.append((build['label'], names))
    if not jobs:
        return {}
    dwz = shutil.which('dwz')
    if dwz is None:
        raise ValueError('dwz is required to write the debug symbol asset')
    (root / '.dwz').mkdir(exist_ok=True)

    def run(job):
        label, names = job
        common = '.dwz/' + label + '.debug'
        before = {name: build_id(root / name) for name in names}
        result = subprocess.run([dwz, '-m', common, '-M', '../../' + common, *names], cwd=root,
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise ValueError('dwz failed for ' + label + ': ' + result.stderr.strip()[-500:])
        if any(build_id(root / name) != identifier for name, identifier in before.items()):
            raise ValueError('dwz changed a build ID: ' + label)
        return label, common if (root / common).is_file() else None

    with concurrent.futures.ThreadPoolExecutor(max_workers=min(len(jobs), os.cpu_count() or 1)) as pool:
        return {label: common for label, common in pool.map(run, jobs) if common}


def write(prepared: dict, output: Path, version: str, timestamp: int) -> dict:
    """Writes putgd-<version>-debug-<arch>.tar.zst into an existing folder."""
    target = output / asset_name(version)
    if target.exists():
        raise ValueError('Debug symbol asset already exists')
    zstd = shutil.which('zstd')
    if zstd is None:
        raise ValueError('zstd is required to write the debug symbol asset')
    with tempfile.TemporaryDirectory(prefix='pgdbg-') as temporary:
        root = Path(temporary) / 'tree'
        for name, data in prepared['files'].items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_bytes(decompressed(data, Path(temporary)))
        common = deduplicate(root, prepared['builds'])
        index = {'schemaVersion': 1, 'version': version, 'architecture': ARCHITECTURE,
                 'layout': '.build-id/<2 hex>/<rest>.debug, sharing .dwz/<label>.debug',
                 'builds': [{**build, **({'dwz': common[build['label']]} if build['label'] in common else {})}
                            for build in prepared['builds']]}
        plain = Path(temporary) / 'debug.tar'
        with tarfile.open(plain, 'w', format=tarfile.USTAR_FORMAT) as tar:
            _add(tar, 'manifest.json', None, timestamp, (json.dumps(index, indent=2) + '\n').encode())
            for name in [*sorted(prepared['files']), *sorted(common.values())]:
                _add(tar, name, root / name, timestamp)
        subprocess.run([zstd, '-q', *ASSET_ZSTD, '-T0', '--no-progress', '-o', str(target), str(plain)], check=True)
    return {'file': target.name, 'bytes': target.stat().st_size, 'sha256': sha256(target), 'buildIds': len(prepared['files']),
            'builds': [{'label': build['label'], 'programs': build['programs']} for build in prepared['builds']]}


def read_asset(path: Path) -> tuple[dict, dict]:
    """The manifest and {member: bytes} of a written asset (tests and maintainers)."""
    plain = subprocess.run(['zstd', '-q', '-d', '-c', str(path)], check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(plain), mode='r:') as tar:
        files = {member.name: tar.extractfile(member).read() for member in tar.getmembers()}
    return json.loads(files.pop('manifest.json')), files


def dry_run(downloaded: Path, output: Path) -> dict:
    """CI without a release tag: the same checks and archive for a development build."""
    manifests = {label: json.loads((downloaded / _artifact(label) / ('debug-symbols-' + label + '.json')).read_text())
                 for label in LABELS}
    reference = manifests['ubuntu24.04']['build']
    fields = ('version', 'gitCommit', 'gitTree', 'commitCount')
    same = lambda build: all(build.get(field) == reference.get(field) for field in fields)
    timestamp = int(subprocess.check_output(['git', 'show', '-s', '--format=%ct', reference['gitCommit']], text=True).strip()) \
        if reference.get('gitCommit') else 0
    prepared = prepare({label: downloaded / _artifact(label) for label in LABELS}, same)
    output.mkdir(parents=True, exist_ok=False)
    return write(prepared, output, reference['version'], timestamp)


def _artifact(label: str) -> str:
    return 'release-appimage' if label == 'appimage' else 'release-native-' + label


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    staged = commands.add_parser('stage', help='Check and stage one build tree')
    staged.add_argument('--build', type=Path, required=True, help='Built Release tree')
    staged.add_argument('--installed', type=Path, required=True, help='The installed or extracted package tree')
    staged.add_argument('--label', required=True, choices=LABELS)
    staged.add_argument('--output', type=Path, required=True)
    staged.add_argument('--timestamp', type=int, default=0)
    dry = commands.add_parser('dry-run', help='Assemble a development debug asset from downloaded package artifacts')
    dry.add_argument('--inputs', type=Path, required=True)
    dry.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.command == 'stage':
        result = stage(args.build, args.installed, args.output, args.label, args.timestamp)
        print(json.dumps({'label': result['label'], 'files': len(result['files'])}))
    else:
        print(json.dumps(dry_run(args.inputs, args.output)))


if __name__ == '__main__':
    main()
