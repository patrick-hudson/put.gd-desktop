#!/usr/bin/env python3
"""Restore one losslessly archived screenshot, verified against its JSONL receipt."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def owned_file(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
        raise ValueError('Expected an owned regular file with one link')
    return info


def restore(row, root):
    original, packed = Path(row['original']), Path(row['compressed'])
    for path in (original, packed):
        if not path.is_absolute() or not path.resolve().is_relative_to(root):
            raise ValueError('Receipt path escapes the allowed evidence root')
    owned_file(packed)
    if packed.stat().st_size != row['compressedBytes'] or digest(packed) != row['compressedSha256']:
        raise ValueError('Compressed file does not match its receipt')
    if original.exists() or original.is_symlink():
        metadata = owned_file(original)
        if original.stat().st_size != row['originalBytes'] or digest(original) != row['originalSha256']:
            raise ValueError('Refusing to overwrite an existing mismatched original')
        if stat.S_IMODE(metadata.st_mode) != row['originalMode'] or metadata.st_mtime_ns != row['originalMtimeNs']:
            raise ValueError('Existing original metadata does not match the receipt')
        return 'already-restored'
    if shutil.disk_usage(original.parent).free < row['originalBytes'] + 64 * 1024 * 1024:
        raise ValueError('Insufficient free space for restoration and 64 MiB reserve')
    fd, name = tempfile.mkstemp(prefix='.restore-', dir=original.parent)
    temporary = Path(name)
    try:
        count, sha = 0, hashlib.sha256()
        with os.fdopen(fd, 'wb') as output, gzip.open(packed, 'rb') as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b''):
                count += len(chunk)
                if count > row['originalBytes']:
                    raise ValueError('Decompressed data exceeds receipt size')
                sha.update(chunk)
                output.write(chunk)
            if count != row['originalBytes'] or sha.hexdigest() != row['originalSha256']:
                raise ValueError('Restored bytes do not match the original receipt')
            output.flush()
            os.fsync(output.fileno())
        os.chmod(temporary, row['originalMode'] & 0o777)
        os.utime(temporary, ns=(row['originalMtimeNs'], row['originalMtimeNs']))
        os.link(temporary, original)  # Exclusive creation; never replace a raced original.
        temporary.unlink()
        directory = os.open(original.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return 'restored'
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--original', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    owned_file(args.manifest)
    matches = [json.loads(line) for line in args.manifest.read_text().splitlines()
               if json.loads(line)['original'] == str(args.original)]
    if len(matches) != 1:
        raise ValueError('Expected exactly one receipt for the requested original')
    result = restore(matches[0], root)
    print(json.dumps({'status': result, 'original': str(args.original),
                      'sha256': matches[0]['originalSha256'], 'compressedPreserved': True}))


if __name__ == '__main__':
    main()
