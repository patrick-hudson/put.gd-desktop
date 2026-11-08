#!/usr/bin/env python3
"""pacman repository metadata from verified package bytes; signatures are detached."""
import hashlib
import io
from pathlib import Path
import subprocess
import tarfile


def info(package):
    text = subprocess.check_output(['tar', '--zstd', '-xOf', str(package), '.PKGINFO'], text=True)
    fields = {}
    for line in text.splitlines():
        if ' = ' in line and not line.startswith('#'):
            key, value = line.split(' = ', 1)
            fields.setdefault(key, []).append(value)
    return fields


def database(directory, channel, package=None):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    # One current package per channel. Old payloads remain available for cached dbs.
    with tarfile.open(directory / ('putgd-' + channel + '.db.tar.gz'), 'w:gz') as archive:
        if package is None:
            return
        fields = info(package)
        digest = hashlib.sha256(Path(package).read_bytes()).hexdigest()
        values = {'FILENAME': [Path(package).name], 'NAME': fields['pkgname'], 'BASE': fields['pkgname'],
                  'VERSION': fields['pkgver'], 'DESC': fields['pkgdesc'], 'ARCH': fields['arch'],
                  'CSIZE': [str(Path(package).stat().st_size)], 'ISIZE': fields['size'],
                  'SHA256SUM': [digest], 'URL': fields.get('url', []), 'LICENSE': fields.get('license', []),
                  'DEPENDS': fields.get('depend', []), 'OPTDEPENDS': fields.get('optdepend', []),
                  'BUILDDATE': fields['builddate'], 'PACKAGER': fields['packager']}
        data = ''.join('%' + key + '%\n' + '\n'.join(value) + '\n\n' for key, value in values.items() if value).encode()
        entry = tarfile.TarInfo('putgd-' + fields['pkgver'][0] + '/desc')
        entry.size, entry.mode = len(data), 0o644
        archive.addfile(entry, io.BytesIO(data))


def check_database(data, asset=None, digest=None):
    """Stable workstation checks staged index against independently fetched checksums."""
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        members = archive.getmembers()
        if asset is None:
            if members:
                raise ValueError('Expected empty pacman database')
            return
        if len(members) != 1 or not members[0].isfile() or not members[0].name.endswith('/desc'):
            raise ValueError('Expected exactly one pacman package description')
        text = archive.extractfile(members[0]).read().decode()
        fields = {}
        for block in text.strip().split('\n\n'):
            lines = block.splitlines()
            fields[lines[0]] = lines[1:]
        if fields.get('%NAME%') != ['putgd'] or fields.get('%FILENAME%') != [asset] or fields.get('%SHA256SUM%') != [digest]:
            raise ValueError('Pacman database differs from the release asset')
