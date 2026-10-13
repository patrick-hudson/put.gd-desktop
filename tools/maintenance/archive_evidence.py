#!/usr/bin/env python3
"""Losslessly archive explicit cold evidence files; never selects or deletes runs."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile


def digest(path, compressed=False):
    h = hashlib.sha256()
    opener = gzip.open if compressed else open
    with opener(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def checked(root, relative):
    path = root / relative
    if Path(relative).is_absolute() or '..' in Path(relative).parts:
        raise ValueError('Require a relative path within the evidence root')
    for part in Path(relative).parts:
        if part in {'.git', 'lab-images', 'lab-runs', 'windows'} or any(
                word in part.lower() for word in ('credential', 'password', 'secret', 'login')):
            raise ValueError('Protected path')
    cursor = path
    while cursor != root:
        if cursor.is_symlink():
            raise ValueError('Symlink paths are not supported')
        cursor = cursor.parent
    return path


def save_manifest(path, manifest):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix='.archive-index-')
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(manifest, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        if hasattr(os, 'O_DIRECTORY'):
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)


def archive(root, relative, manifest_path, manifest):
    source = checked(root, relative)
    target = checked(root, relative + '.gz')
    if source.suffix not in {'.tar', '.log', '.xml'}:
        raise ValueError('Only cold tar, log and XML evidence is supported')
    if manifest_path.is_symlink() or manifest_path.resolve() in {source, target}:
        raise ValueError('Manifest must be separate from the evidence')
    before = source.stat()
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise ValueError('Require an ordinary file with one hard link')
    if target.exists() or relative in manifest['files']:
        raise ValueError('Archive or manifest entry already exists')
    expected = digest(source)
    created = False
    try:
        with target.open('xb') as raw:
            created = True
            os.chmod(target, stat.S_IMODE(before.st_mode))
            with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as output:
                with source.open('rb') as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        output.write(block)
            raw.flush()
            os.fsync(raw.fileno())
        if digest(target, compressed=True) != expected:
            raise ValueError('Archive round-trip hash mismatch')
        after = source.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError('Source changed during compression')
        if target.stat().st_size >= before.st_size:
            target.unlink()
            return False
        manifest['files'][relative] = dict(sha256=expected, bytes=before.st_size,
            archive=relative + '.gz', archive_sha256=digest(target),
            archive_bytes=target.stat().st_size, mode=stat.S_IMODE(before.st_mode),
            mtime_ns=before.st_mtime_ns)
        # A durable recovery entry and verified archive exist before original removal.
        save_manifest(manifest_path, manifest)
        source.unlink()
        return True
    except Exception:
        if created and relative not in manifest['files']:
            target.unlink(missing_ok=True)
        raise


def restore(root, relative, manifest):
    entry = manifest['files'][relative]
    target = checked(root, relative)
    packed = checked(root, entry['archive'])
    if target.exists():
        raise ValueError('Refusing to overwrite an existing original')
    if digest(packed) != entry['archive_sha256'] or digest(packed, True) != entry['sha256']:
        raise ValueError('Archive checksum mismatch')
    fd, temporary = tempfile.mkstemp(dir=target.parent, prefix='.evidence-restore-')
    try:
        with os.fdopen(fd, 'wb') as output:
            os.fchmod(output.fileno(), entry['mode'])
            with gzip.open(packed, 'rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    output.write(block)
            output.flush()
            os.fsync(output.fileno())
        if digest(temporary) != entry['sha256']:
            raise ValueError('Restored checksum mismatch')
        os.utime(temporary, ns=(entry['mtime_ns'], entry['mtime_ns']))
        # Atomic publication, with no overwrite even if another writer races us.
        os.link(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)
    # Keep the archive and its receipt, including after restoration.


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('archive', 'restore'))
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    manifest = json.loads(args.manifest.read_text()) if args.manifest.exists() else {
        'schema': 1, 'root': str(root), 'files': {}}
    if manifest['root'] != str(root):
        raise ValueError('Manifest belongs to a different root')
    for relative in args.files:
        if args.action == 'archive':
            archive(root, relative, args.manifest, manifest)
        else:
            restore(root, relative, manifest)
    print(json.dumps({'action': args.action, 'requested_files': len(args.files),
                      'indexed_files': len(manifest['files'])}))


if __name__ == '__main__':
    main()
