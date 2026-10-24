#!/usr/bin/env python3
"""Export curated committed build inputs; never traverse working-tree contents."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

from audit_native import sha256
from release_identity import calendar_base

EXACT_PATHS = frozenset({
    'CMakeLists.txt', 'CMakePresets.json', 'LICENSE', 'THIRD_PARTY_NOTICES.md', '.gitignore',
    'docs/build.md', 'docs/proton-drive.md', 'tools/verify_dependencies.py', 'tools/releases/provision-sdk.sh',
    'tools/releases/build-appimage.sh', 'tools/releases/provision-appimage.py',
    'tools/releases/portable_appimage.py', 'tools/releases/appimage_notices.py',
    'tools/releases/appimage_update.py', 'tools/releases/appimage_diagnostics.py',
    'tools/releases/appimage_complete_sources.py', 'tools/releases/audit_native.py',
    'tools/releases/audit_proton.py', 'tools/releases/release_identity.py',
})
PREFIXES = ('src', 'cmake', 'packaging/linux', 'packaging/appimage', 'integrations/gnome', 'tools/proton')
EXCLUDED_COMPONENTS = frozenset({
    '.git', '.github', '.agents', '.codex', '.compound-engineering', '.context', '.claude',
    'work', 'build',
    'context', 'agents', 'tests', 'test', 'fixtures', 'plans', 'reviews', 'evidence',
})
IDENTITY_FIELDS = ('version', 'baseVersion', 'packageVersion', 'gitCommit', 'gitTree', 'commitCount')


def _git(root, *args, input=None):
    result = subprocess.run(['git', '-C', str(root), *args], input=input,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        # Git errors can contain local paths; keep the public error bounded.
        raise ValueError('cannot read committed Git source')
    return result.stdout


def _json(value):
    return (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def _identity(root, identity):
    if not isinstance(identity, dict) or any(k not in identity for k in IDENTITY_FIELDS):
        raise ValueError('source identity requires version, base, package, commit, tree, and count')
    # Validate values used in filenames, Git arguments, and generated build commands.
    for key in ('version', 'packageVersion'):
        if not isinstance(identity[key], str) or not re.fullmatch(r'[0-9][A-Za-z0-9.+~_-]{0,127}', identity[key]):
            raise ValueError('unsafe source identity version')
    if not isinstance(identity['baseVersion'], str) or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', identity['baseVersion']):
        raise ValueError('invalid source base version')
    for key in ('gitCommit', 'gitTree'):
        if not isinstance(identity[key], str) or not re.fullmatch(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', identity[key]):
            raise ValueError('full source commit and tree hashes are required')
    count = identity['commitCount']
    if isinstance(count, bool) or not isinstance(count, int) or count < 0:
        raise ValueError('source commit count must be a nonnegative integer')
    commit = identity['gitCommit']
    if _git(root, 'rev-parse', '--verify', commit + '^{commit}').decode().strip() != commit:
        raise ValueError('source commit identity mismatch')
    if _git(root, 'rev-parse', '--verify', commit + '^{tree}').decode().strip() != identity['gitTree']:
        raise ValueError('source tree identity mismatch')
    if int(_git(root, 'rev-list', '--count', commit).strip()) != count:
        raise ValueError('source commit count identity mismatch')
    timestamp = int(_git(root, 'show', '-s', '--format=%ct', commit).strip())
    if not 0 <= timestamp <= 0xffffffff:
        raise ValueError('source timestamp is unsupported by gzip')
    # Make a detached JSON copy, preserving all original build/release identity fields.
    try:
        original = json.loads(_json(identity))
    except (TypeError, ValueError) as error:
        raise ValueError('source identity must be JSON data') from error
    return original, timestamp


def _included(path):
    parts = path.split('/')
    if any(p in EXCLUDED_COMPONENTS for p in parts):
        return False
    if any(p in ('credentials.txt', 'AGENTS.md', 'CLAUDE.md', 'MEMORY.md') or p == '.env' or p.startswith('.env.') for p in parts):
        return False
    return path in EXACT_PATHS or any(path == prefix or path.startswith(prefix + '/') for prefix in PREFIXES)


def _safe_path(path):
    if (not path or PurePosixPath(path).is_absolute() or '\\' in path or ':' in path
            or any(ord(c) < 32 or ord(c) == 127 for c in path)
            or any(p in ('', '.', '..') for p in path.split('/'))):
        raise ValueError('unsafe included source path')


def _entries(root, commit):
    selected = []
    for item in _git(root, 'ls-tree', '-r', '-z', '--full-tree', commit).split(b'\0'):
        if not item:
            continue
        header, raw_path = item.split(b'\t', 1)
        try:
            path = raw_path.decode('utf-8')
        except UnicodeDecodeError as error:
            raise ValueError('unsafe non-UTF-8 source path') from error
        if not _included(path):
            continue
        _safe_path(path)
        mode, kind, oid = header.decode('ascii').split()
        if mode not in ('100644', '100755') or kind != 'blob' or path in PREFIXES:
            raise ValueError('unsupported included source entry: ' + path)
        selected.append((path, mode, oid))
    return sorted(selected)


def _blobs(root, entries):
    # Deduplicate blob IDs and read all selected content in one Git process. Excluded
    # paths never reach cat-file, even when tracked credential fixtures exist.
    ids = list(dict.fromkeys(oid for _, _, oid in entries))
    if not ids:
        return {}
    output = _git(root, 'cat-file', '--batch', input=('\n'.join(ids) + '\n').encode('ascii'))
    blobs, offset = {}, 0
    for expected in ids:
        end = output.index(b'\n', offset)
        oid, kind, size = output[offset:end].decode('ascii').split()
        size = int(size)
        offset = end + 1
        if oid != expected or kind != 'blob' or size < 0 or output[offset + size:offset + size + 1] != b'\n':
            raise ValueError('invalid Git blob response')
        blobs[oid] = output[offset:offset + size]
        offset += size + 1
    if offset != len(output):
        raise ValueError('unexpected Git blob response')
    return blobs


def _readme(identity, local):
    return f'''# Building the put.gd source archive

This curated archive contains committed production build inputs from
`{identity['gitCommit']}`. `source-info.json` preserves the exact original identity,
including its original version `{identity['version']}`, and records file hashes.
Ignored, untracked, dirty working-tree files, agent context, test/lab artifacts,
and credentials are excluded. Proton provisioning checks are retained as rebuild
inputs. Dependencies and binary build outputs are not bundled.

Install the normal system C++20, CMake 3.24+, Ninja, Qt 6.4.2+, QtKeychain, and
Linux development packages listed in `docs/build.md`. Keep Qt dynamically linked
to distribution libraries. Obtain the pinned official AWS SDK and verify its
source and submodules with `tools/verify_dependencies.py` before building it.
The documented default SDK install is `build/sdk`; `tools/releases/provision-sdk.sh`
contains the official SDK's CMake build commands. No compiler or Qt substitute is
provided, and application configuration does not download dependencies.

For a Proton-enabled Linux x86-64 build, install the normal Git/npm build tools
and provision the pinned helper before configuring CMake:

```sh
python3 tools/proton/provision_runtime.py
```

This selects the SDK, Bun and approved GPL crypto sources from
`tools/proton/runtime-lock.json` and `package-lock.json`, and runs the helper's
provisioning checks. Clean existing SDK and crypto checkouts can be selected with
`--source` and `--crypto-source`; add `--offline` only when both checkouts and
locked npm inputs are cached. The helper is rebuilt from this archive's own
`tools/proton/` inputs. See `tools/proton/README.md`,
`tools/proton/notices/REBUILD.md` and `tools/proton/notices/SOURCES.json` for the
corresponding source assets and native-component rebuild instructions.

After provisioning, append `-DPUTGD_BUNDLE_PROTON=ON` to the configure command
below to include Proton. This runtime targets Linux x86-64 only; other targets
leave the option off. No separate Bun/Node installation is required by packaged
users. Proton setup requires a browser and a working desktop keychain; see
`docs/proton-drive.md`.

After provisioning the SDK, run these commands from the extracted archive root:

```sh
cmake --preset release \\
  -DBUILD_TESTING=OFF \\
  -DPUTGD_LAB_FIXTURE=OFF \\
  -DPUTGD_PHYSICAL_BENCHMARKS=OFF \\
  -DPUTGD_SOURCE_COMMIT={identity['gitCommit']} \\
  -DPUTGD_SOURCE_TREE={identity['gitTree']} \\
  -DPUTGD_SOURCE_COMMIT_COUNT={identity['commitCount']} \\
  -DPUTGD_SOURCE_COMMIT_TIME={local['commitTime']}
cmake --build --preset release
```

If using an existing SDK elsewhere, additionally set `CMAKE_PREFIX_PATH`,
`PUTGD_AWS_RUNTIME_DIR` (its library directory), and `PUTGD_AWS_SOURCE_DIR`
(its verified source directory including component licenses), as described in
`docs/build.md`. Application test and lab sources are omitted, so leave `BUILD_TESTING=OFF`,
`PUTGD_LAB_FIXTURE=OFF`, and `PUTGD_PHYSICAL_BENCHMARKS=OFF`.

This archive has no Git checkout. These flags provide supplied provenance;
they do not prove locally observed clean Git state or a release tag. A local
build is labeled `{local['version']}` with `sourceProvenance: supplied`, rather
than inheriting an official binary release label. Do not pass
`PUTGD_RELEASE_TAG` to an archive build. Official binary releases are built
from the verified exact clean Git tag by the release workflow. If the original
identity describes a dirty development prototype, this archive still exports
only its committed revision, not the prototype's uncommitted modifications.

`source-info.json` hashes every other file, including this generated guide;
its own recursive checksum is omitted. The external `source-archive.json`
receipt pins every file and the compressed archive bytes.
'''.encode('utf-8')


def create_archive(root: Path, output_file: Path, identity: dict) -> dict:
    """Write a deterministic curated archive from identity.gitCommit, exclusively."""
    root, output_file = Path(root), Path(output_file)
    if output_file.exists() or output_file.is_symlink():
        raise ValueError('source archive output already exists')
    original, timestamp = _identity(root, identity)
    entries = _entries(root, original['gitCommit'])
    blobs = _blobs(root, entries)
    # The development version of the same commit: its calendar base (#373) and short hash.
    base = calendar_base(timestamp, original['commitCount'])
    local = {'sourceProvenance': 'supplied', 'releaseTagBuild': False, 'commitTime': timestamp,
             'version': f"{base}-dev+g{original['gitCommit'][:12]}"}
    files = {path: (blobs[oid], 0o755 if mode == '100755' else 0o644, oid) for path, mode, oid in entries}
    files['README_SOURCE.md'] = (_readme(original, local), 0o644, None)

    def members():
        return [{'path': path, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
                 'mode': f'{mode:04o}', 'gitBlob': oid} for path, (data, mode, oid) in sorted(files.items())]

    files['source-info.json'] = (_json({'schemaVersion': 1, 'identity': original,
        'sourceSelection': 'positive allowlist of committed Git blobs', 'localBuild': local,
        'files': members(), 'selfChecksumExcluded': 'source-info.json'}), 0o644, None)
    archive_root = 'putgd-' + original['version'] + '-source'
    directories = {archive_root}
    for path in files:
        parent = PurePosixPath(path).parent
        while str(parent) != '.':
            directories.add(archive_root + '/' + str(parent))
            parent = parent.parent

    output_file.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output_file.open('xb') as target:
            with gzip.GzipFile(fileobj=target, mode='wb', filename='', mtime=timestamp, compresslevel=9) as compressed:
                with tarfile.open(fileobj=compressed, mode='w', format=tarfile.PAX_FORMAT) as archive:
                    for name in sorted(directories | {archive_root + '/' + p for p in files}):
                        item = tarfile.TarInfo(name)
                        item.uid = item.gid = 0
                        item.uname = item.gname = 'root'
                        item.mtime = timestamp
                        if name in directories:
                            item.type, item.mode = tarfile.DIRTYPE, 0o755
                            archive.addfile(item)
                        else:
                            data, item.mode, _ = files[name[len(archive_root) + 1:]]
                            item.size = len(data)
                            archive.addfile(item, io.BytesIO(data))
    except FileExistsError as error:
        raise ValueError('source archive output already exists') from error
    except Exception:
        output_file.unlink(missing_ok=True)
        raise
    return {'schemaVersion': 1, 'format': 'source.tar.gz', 'identity': original,
            'archiveRoot': archive_root, 'timestamp': timestamp, 'localBuild': local,
            'artifact': {'file': output_file.name, 'sha256': sha256(output_file),
                         'bytes': output_file.stat().st_size}, 'members': members()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True, help='new output directory')
    parser.add_argument('--identity', type=Path, required=True, help='observed build manifest or verified release identity JSON')
    args = parser.parse_args()
    try:
        identity = json.loads(args.identity.read_text(encoding='utf-8'))
        # Validate before creating an output directory; CLI input is a manifest,
        # not a free-form archive name or a release-tag override.
        original, _ = _identity(args.root, identity)
        args.output_dir.mkdir(parents=True, exist_ok=False)
        receipt = create_archive(args.root, args.output_dir / ('putgd-' + original['version'] + '-source.tar.gz'), original)
        (args.output_dir / 'source-archive.json').write_bytes(_json(receipt))
    except (OSError, ValueError) as error:
        parser.exit(1, f'Source archive export failed: {error}\n')
    print(json.dumps(receipt['artifact'], sort_keys=True))


if __name__ == '__main__':
    main()
