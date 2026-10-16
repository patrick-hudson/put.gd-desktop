#!/usr/bin/env python3
"""Prepare byte-verified Proton corresponding-source transport without extraction."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import tempfile
from urllib.request import urlopen

from audit_native import sha256 as digest

BUNDLE_NAME = 'putgd-proton-corresponding-source.tar.gz'
PART_NAMES = (BUNDLE_NAME + '.part1', BUNDLE_NAME + '.part2')
CATALOG_NAME = 'proton-source-catalog.json'



def _entry(item, key):
    name = item.get(key)
    if (not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*', name)
            or type(item.get('bytes')) is not int or item['bytes'] <= 0
            or not re.fullmatch(r'[0-9a-f]{64}', str(item.get('sha256', '')))):
        raise ValueError('Invalid Proton source catalog entry')
    return name


def catalog_value(path):
    catalog = json.loads(path.read_bytes())
    if (catalog.get('schemaVersion') != 1 or catalog.get('materialsReady') is not True
            or not re.fullmatch(r'[0-9a-f]{64}', str(catalog.get('helperSha256', '')))):
        raise ValueError('Invalid Proton source catalog')
    delivery = catalog['delivery']
    if _entry(delivery, 'file') != BUNDLE_NAME:
        raise ValueError('Invalid Proton source catalog delivery filename')
    parts = delivery['parts']
    if [_entry(part, 'file') for part in parts] != list(PART_NAMES):
        raise ValueError('Invalid Proton source catalog transport parts')
    if sum(part['bytes'] for part in parts) != delivery['bytes']:
        raise ValueError('Invalid Proton source catalog transport size')
    artifacts = catalog['artifacts']
    names = [_entry(item, 'filename') for item in artifacts]
    if not names or len(names) != len(set(names)):
        raise ValueError('Invalid Proton source catalog artifact inventory')
    return catalog


class Joined:
    """Read each transport part in order; tarfile's streaming mode needs only read."""
    def __init__(self, paths):
        self.paths = iter(paths)
        self.stream = None

    def read(self, size):
        chunks = []
        while size:
            if self.stream is None:
                path = next(self.paths, None)
                if path is None:
                    break
                self.stream = path.open('rb')
            chunk = self.stream.read(size)
            if chunk:
                chunks.append(chunk)
                size -= len(chunk)
            else:
                self.stream.close()
                self.stream = None
        return b''.join(chunks)

    def close(self):
        if self.stream:
            self.stream.close()


def verify(directory, catalog):
    paths = [directory / name for name in PART_NAMES]
    combined = hashlib.sha256()
    for path, item in zip(paths, catalog['delivery']['parts']):
        if not path.is_file() or path.is_symlink() or path.stat().st_size != item['bytes']:
            raise ValueError('Proton source bundle bytes differ from catalog')
        value = hashlib.sha256()
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1048576), b''):
                combined.update(chunk)
                value.update(chunk)
        if value.hexdigest() != item['sha256']:
            raise ValueError('Proton source bundle bytes differ from catalog')
    if combined.hexdigest() != catalog['delivery']['sha256']:
        raise ValueError('Proton source bundle bytes differ from catalog')
    expected = {item['filename']: item for item in catalog['artifacts']}
    seen = set()
    joined = Joined(paths)
    try:
        with tarfile.open(fileobj=joined, mode='r|gz') as archive:
            for member in archive:
                if not member.isfile() or member.name not in expected or member.name in seen:
                    raise ValueError('Unexpected Proton source bundle member')
                item = expected[member.name]
                if member.size != item['bytes']:
                    raise ValueError('Proton source member bytes differ from catalog')
                value = hashlib.sha256()
                with archive.extractfile(member) as stream:
                    for chunk in iter(lambda: stream.read(1048576), b''):
                        value.update(chunk)
                if value.hexdigest() != item['sha256']:
                    raise ValueError('Proton source member bytes differ from catalog')
                seen.add(member.name)
    finally:
        joined.close()
    if seen != set(expected):
        raise ValueError('Missing Proton source bundle member')


def prepare(catalog_path, output, bundle=None):
    catalog = catalog_value(catalog_path)
    with tempfile.TemporaryDirectory(prefix='pg-proton-source-') as temporary:
        staged = Path(temporary)
        if bundle is not None:
            with bundle.open('rb') as source:
                for part in catalog['delivery']['parts']:
                    remaining = part['bytes']
                    with (staged / part['file']).open('wb') as target:
                        while remaining:
                            chunk = source.read(min(remaining, 1048576))
                            if not chunk:
                                raise ValueError('Proton source bundle bytes differ from catalog')
                            target.write(chunk)
                            remaining -= len(chunk)
                if source.read(1):
                    raise ValueError('Proton source bundle bytes differ from catalog')
        else:
            for part in catalog['delivery']['parts']:
                if not part.get('url', '').startswith('https://'):
                    raise ValueError('Proton source catalog requires HTTPS delivery URLs')
                with urlopen(part['url'], timeout=120) as source, (staged / part['file']).open('wb') as target:
                    shutil.copyfileobj(source, target, 1048576)
        verify(staged, catalog)
        result = {'schemaVersion': 1, 'helperSha256': catalog['helperSha256'],
            'sourceCatalogSha256': digest(catalog_path), 'delivery': catalog['delivery'],
            'verifiedArtifacts': catalog['artifacts']}
        output.mkdir(parents=True, exist_ok=False)
        for name in PART_NAMES:
            shutil.copyfile(staged / name, output / name)
        shutil.copyfile(catalog_path, output / CATALOG_NAME)
        (output / 'proton-source-delivery.json').write_text(json.dumps(result, indent=2) + '\n')
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--catalog', type=Path, default=Path('tools/proton/notices/SOURCES.json'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, help='Use an already downloaded complete transport archive')
    args = parser.parse_args()
    print(json.dumps(prepare(args.catalog, args.output, args.bundle)))


if __name__ == '__main__':
    main()
