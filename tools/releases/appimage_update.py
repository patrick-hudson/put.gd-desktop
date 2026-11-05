"""AppImage update information and its `.zsync` release asset (#293).

A tagged AppImage embeds `zsync|https://app.put.gd/dl/<channel>/appimage.zsync`
in its runtime's `.upd_info` section: the put.gd-site permalink to the newest
`.zsync` of its channel. Nightly tags follow `nightly`; candidates and stable
releases follow `stable`. The `.zsync` is made from the final image bytes and
names the image by its absolute URL on the release signer's mirror,
`https://pkg.put.gd/releases/<tag>/<name>`, so AppImageUpdate, Gear Lever,
AppImageLauncher and put.gd's own updater fetch exactly the image whose SHA-1 it
records, whichever redirect served the `.zsync`. The mirror's URL doesn't
redirect: zsync2 resolves redirects with a request that ignores the CA bundle,
which fails on Fedora 44 (#297). The signer (pkgrepo.py) mirrors each release's
image and `.zsync` before the feed names it.

Untagged CI and development builds embed nothing and have no `.zsync`, so no
updater offers to replace a development build with a release. A lab origin
(`--update-origin`) points both URLs at a local server for update tests; the
collector rejects such an image. A lab base (`--update-base`, for example
https://pkg.put.gd/lab/) gives a tagged lab build the live layout under that
prefix, as the signer's lab scope publishes it.
"""
import hashlib
from pathlib import Path
import re
import struct

from release_identity import parse_tag

REPOSITORY = 'patrick-hudson/put.gd-desktop'
PERMALINK = 'https://app.put.gd/dl/{}/appimage.zsync'
MIRROR = 'https://pkg.put.gd/'
SECTION = '.upd_info'
# zsync 0.6.2's zsyncmake, as the pinned appimagetool bundles it (tools.lock.json).
ZSYNC_VERSION = '0.6.2'


def channel(tag):
    """The update channel of a release tag: release candidates follow stable."""
    return 'nightly' if parse_tag(tag).channel == 'nightly' else 'stable'


def update_information(tag):
    return 'zsync|' + PERMALINK.format(channel(tag))


def asset_url(repository, tag, name):
    parse_tag(tag)
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository or ''):
        raise ValueError('Release repository must be OWNER/NAME')
    if Path(name).name != name or not name.endswith('.AppImage'):
        raise ValueError('Invalid AppImage asset name')
    return 'https://github.com/' + repository + '/releases/download/' + tag + '/' + name


def mirror_url(tag, name, base=MIRROR):
    """The signer's copy of a release's image (#293 phase 3)."""
    parse_tag(tag)
    if Path(name).name != name or not name.endswith('.AppImage'):
        raise ValueError('Invalid AppImage asset name')
    if not re.fullmatch(r'https://[^\s|?#]+/', base):
        raise ValueError('Update base must be an https URL ending in /')
    return base + 'releases/' + tag + '/' + name


def zsync_name(image_name):
    return image_name + '.zsync'


def targets(name, tag=None, repository=REPOSITORY, origin=None, base=None):
    """Return (update information, image URL for the .zsync), or None to embed nothing."""
    if origin:
        if not re.fullmatch(r'https?://[^\s|]+', origin):
            raise ValueError('Lab update origin must be an http(s) URL')
        origin = origin.rstrip('/')
        return 'zsync|' + origin + '/appimage.zsync', origin + '/' + name
    if not tag:
        return None
    asset_url(repository, tag, name)  # checks the repository and name
    if base:
        url = mirror_url(tag, name, base)
        return 'zsync|' + base + f'dl/{channel(tag)}/appimage.zsync', url
    return update_information(tag), mirror_url(tag, name)


def read_update_information(path):
    """The NUL-terminated text in the AppImage runtime's .upd_info ELF section."""
    with Path(path).open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:4] != b'\x7fELF' or header[4] != 2 or header[5] != 1:
            raise ValueError('AppImage runtime is not a little-endian ELF64 file')
        offset, = struct.unpack_from('<Q', header, 0x28)
        size, count, names = struct.unpack_from('<HHH', header, 0x3A)
        if size != 64 or not 0 < names < count <= 256:
            raise ValueError('AppImage runtime has no usable section table')
        stream.seek(offset)
        table = stream.read(size * count)
        if len(table) != size * count:
            raise ValueError('AppImage runtime section table is truncated')
        sections = [struct.unpack_from('<IIQQQQ', table, index * size) for index in range(count)]

        def read(section):
            stream.seek(section[4])
            data = stream.read(section[5])
            if len(data) != section[5]:
                raise ValueError('AppImage runtime section is truncated')
            return data
        strings = read(sections[names])
        for section in sections:
            end = strings.find(b'\0', section[0])
            if end >= 0 and strings[section[0]:end] == SECTION.encode():
                data = read(section)
                text = data.split(b'\0', 1)[0]
                if data[len(text):].strip(b'\0'):
                    raise ValueError('AppImage update information is not NUL-terminated')
                return text.decode('ascii')
    raise ValueError('AppImage runtime has no ' + SECTION + ' section')


def zsync_header(path):
    """Parse a zsync 0.6.2 control file into its header fields and block list size."""
    data = Path(path).read_bytes()
    head, separator, blocks = data.partition(b'\n\n')
    if not separator:
        raise ValueError('zsync control file has no header terminator')
    fields = {}
    for line in head.decode('ascii').split('\n'):
        key, separator, value = line.partition(': ')
        if not separator or key in fields:
            raise ValueError('Invalid zsync header line')
        fields[key] = value
    return fields, len(blocks)


def sha1(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha1').hexdigest()


def verify_zsync(path, image, name, url):
    """Check that a .zsync describes exactly these image bytes, published as name at url."""
    fields, blocks_bytes = zsync_header(path)
    image = Path(image)
    length = image.stat().st_size
    try:
        block_size = int(fields['Blocksize'])
        _, rsum_bytes, checksum_bytes = (int(value) for value in fields['Hash-Lengths'].split(','))
        recorded = int(fields['Length'])
    except (KeyError, ValueError) as error:
        raise ValueError('zsync header lacks its block parameters') from error
    if fields.get('zsync') != ZSYNC_VERSION:
        raise ValueError('zsync control file is not zsync ' + ZSYNC_VERSION)
    if fields.get('Filename') != name or fields.get('URL') != url:
        raise ValueError('zsync control file names another image or URL')
    if recorded != length or fields.get('SHA-1', '').lower() != sha1(image):
        raise ValueError('zsync control file describes other image bytes')
    if block_size <= 0 or blocks_bytes != -(-length // block_size) * (rsum_bytes + checksum_bytes):
        raise ValueError('zsync block list does not cover the whole image')
    return fields
